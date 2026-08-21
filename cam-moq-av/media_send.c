/*
 * media_send — on-demand Annex-B H.264 publisher for ptz-remote.
 *
 * Announces a namespace and its catalog immediately, but ingests video only
 * while a subscriber exists. Each *source* owns one ffmpeg child process
 * pulling one RTSP stream; each source feeds one or more MOQ *tracks*. A
 * supervisor loop starts a source when any of its tracks gains demand and
 * stops it once demand has been absent for the source's idle timeout.
 *
 * Usage:
 *   media_send <url> <namespace> [options]
 *
 * Global options:
 *   --insecure-skip-verify        disable TLS certificate verification
 *   --catalog-keepalive-ms N      catalog refresh interval in ms (0 disables)
 *   --ffmpeg <path>               ffmpeg binary (default "ffmpeg")
 *
 * Source options (apply to the most recent --source):
 *   --source <name>               begin a source definition
 *   --rtsp <url>                  RTSP input; media_send spawns/kills ffmpeg
 *   --fifo <path>                 read Annex-B from an external named pipe
 *   --stdin                       read Annex-B from stdin
 *   --idle-ms N                   linger after last unsubscribe (default 5000)
 *   --always-on                   ignore demand; keep the source running
 *   --warm-with <source>          also run this source while <source> has demand
 *
 * Track options (apply to the most recent --track):
 *   --track <name>                begin a track definition
 *   --from <source>               source feeding this track (default: first)
 *   --width N / --height N        video dimensions for the catalog
 *   --fps N                       catalog framerate (default 30)
 *   --bitrate N                   max bitrate bits/s (default 1500000)
 *   --keyframes-only              publish only sync samples (sidecar track)
 */

#include <moq/endpoint.h>
#include <moq/media_sender.h>
#include <moq/rcbuf.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern char **environ;

static volatile sig_atomic_t g_stop = 0;

enum {
    ENDPOINT_DRAIN_TIMEOUT_US = 5000000,
    SUPERVISOR_TICK_US        = 100000,   /* demand poll / lifecycle cadence */
    TERM_GRACE_US             = 2000000,  /* SIGTERM -> SIGKILL grace */
    BACKOFF_BASE_US           = 500000,
    BACKOFF_MAX_US            = 30000000,
    HEALTHY_RUN_US            = 30000000, /* run this long -> reset backoff */
    DEFAULT_IDLE_US           = 5000000,
    QUEUE_SECONDS             = 2,        /* shared send-ring depth, in seconds */
    QUEUE_MIN_OBJECTS         = 32
};

#define MAX_SOURCES 8
#define MAX_TRACKS  16

typedef enum {
    SRC_INPUT_SPAWN,   /* media_send owns an ffmpeg child */
    SRC_INPUT_FIFO,    /* external producer writes a named pipe */
    SRC_INPUT_STDIN    /* external producer pipes into stdin */
} src_input_t;

typedef enum {
    SRC_IDLE,      /* no process, no reader thread */
    SRC_RUNNING,
    SRC_STOPPING   /* SIGTERM sent, waiting for exit + reader join */
} src_state_t;

typedef struct track_s track_t;

typedef struct {
    /* configuration */
    const char *name;
    src_input_t input;
    const char *rtsp_url;
    const char *fifo_path;
    uint64_t    idle_us;
    bool        always_on;
    const char *warm_with;

    /* bound tracks */
    track_t *tracks[MAX_TRACKS];
    int      n_tracks;

    /* runtime */
    src_state_t state;
    pid_t       pid;
    int         read_fd;
    pthread_t   thread;
    bool        thread_live;
    volatile sig_atomic_t reader_stop;
    volatile sig_atomic_t reader_done;
    uint64_t    started_us;
    uint64_t    idle_deadline_us;
    uint64_t    kill_deadline_us;
    uint64_t    backoff_until_us;
    unsigned    failures;
    unsigned long long objects_sent;

    struct app_s *app;
} source_t;

struct track_s {
    const char *name;
    const char *from;        /* source name, resolved to src at setup */
    source_t   *src;
    uint32_t    width;
    uint32_t    height;
    uint32_t    fps;
    uint64_t    bitrate;
    bool        keyframes_only;
    moq_media_track_t *handle;
};

typedef struct app_s {
    moq_media_sender_t *tx;
    source_t sources[MAX_SOURCES];
    int      n_sources;
    track_t  tracks[MAX_TRACKS];
    int      n_tracks;
    const char *ffmpeg_bin;
} app_t;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static size_t split_namespace(char *buf, moq_bytes_t *parts, size_t max)
{
    size_t n = 0;
    char *p = buf;
    while (*p && n < max) {
        char *slash = strchr(p, '/');
        if (slash)
            *slash = '\0';
        parts[n].data = (const uint8_t *)p;
        parts[n].len = strlen(p);
        n++;
        if (!slash)
            break;
        p = slash + 1;
    }
    return n;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} byte_vec_t;

static bool vec_reserve(byte_vec_t *v, size_t want)
{
    if (want <= v->cap)
        return true;
    size_t next = v->cap ? v->cap : 4096;
    while (next < want)
        next *= 2;
    uint8_t *p = (uint8_t *)realloc(v->data, next);
    if (!p)
        return false;
    v->data = p;
    v->cap = next;
    return true;
}

static bool vec_append(byte_vec_t *v, const uint8_t *src, size_t n)
{
    if (!vec_reserve(v, v->len + n))
        return false;
    memcpy(v->data + v->len, src, n);
    v->len += n;
    return true;
}

static void vec_reset(byte_vec_t *v)
{
    v->len = 0;
}

static void vec_free(byte_vec_t *v)
{
    free(v->data);
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
}

/* Stop predicate shared by the blocking read helpers: either the process is
 * shutting down or this source alone is being torn down. */
static bool halted(const volatile sig_atomic_t *stop)
{
    return g_stop || (stop && *stop);
}

static bool ensure_bytes(FILE *in, byte_vec_t *buf, size_t need,
                         const volatile sig_atomic_t *stop)
{
    uint8_t tmp[4096];
    while (!halted(stop) && buf->len < need) {
        size_t n = fread(tmp, 1, sizeof(tmp), in);
        if (n == 0) {
            /* For optional FIFO tracks opened O_NONBLOCK, "no data yet" is
               surfaced as EAGAIN/EWOULDBLOCK. Keep polling so the thread does
               not exit before the writer starts, and so Ctrl-C can break out. */
            if (ferror(in) && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                clearerr(in);
                usleep(10000);
                continue;
            }
            return false;
        }
        if (!vec_append(buf, tmp, n))
            return false;
    }
    return buf->len >= need;
}

static bool find_start_code(const uint8_t *p, size_t len,
                            size_t *off, size_t *sc_len)
{
    if (len < 3)
        return false;
    for (size_t i = 0; i + 3 <= len; i++) {
        if (p[i] == 0x00 && p[i + 1] == 0x00) {
            if (p[i + 2] == 0x01) {
                *off = i;
                *sc_len = 3;
                return true;
            }
            if (i + 4 <= len && p[i + 2] == 0x00 && p[i + 3] == 0x01) {
                *off = i;
                *sc_len = 4;
                return true;
            }
        }
    }
    return false;
}

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t bit;
} bit_reader_t;

static bool br_read_bit(bit_reader_t *br, uint32_t *out)
{
    if (br->bit >= br->len * 8)
        return false;
    size_t byte_idx = br->bit >> 3;
    unsigned bit_idx = 7U - (unsigned)(br->bit & 7U);
    *out = (br->p[byte_idx] >> bit_idx) & 1U;
    br->bit++;
    return true;
}

static bool br_read_ue(bit_reader_t *br, uint32_t *out)
{
    uint32_t b = 0;
    unsigned zeros = 0;
    while (true) {
        if (!br_read_bit(br, &b))
            return false;
        if (b)
            break;
        zeros++;
        if (zeros > 31)
            return false;
    }
    uint32_t suffix = 0;
    for (unsigned i = 0; i < zeros; i++) {
        if (!br_read_bit(br, &b))
            return false;
        suffix = (suffix << 1) | b;
    }
    *out = ((1u << zeros) - 1u) + suffix;
    return true;
}

static bool nal_is_vcl(uint8_t nal_type)
{
    return nal_type >= 1 && nal_type <= 5;
}

static bool starts_new_au_on_vcl(const uint8_t *nal, size_t nal_len)
{
    if (nal_len < 2)
        return false;
    bit_reader_t br = { .p = nal + 1, .len = nal_len - 1, .bit = 0 };
    uint32_t first_mb = 0;
    if (!br_read_ue(&br, &first_mb))
        return false;
    return first_mb == 0;
}

typedef struct {
    byte_vec_t pending;
    bool have_vcl;
    bool current_is_keyframe;
} h264_annexb_reader_t;

static void h264_reader_init(h264_annexb_reader_t *r)
{
    memset(r, 0, sizeof(*r));
}

static void h264_reader_free(h264_annexb_reader_t *r)
{
    vec_free(&r->pending);
}

static bool read_h264_access_unit(FILE *in,
                                  h264_annexb_reader_t *r,
                                  byte_vec_t *out_au,
                                  bool *is_keyframe,
                                  const volatile sig_atomic_t *stop)
{
    vec_reset(out_au);
    *is_keyframe = false;

    while (!halted(stop)) {
        if (!ensure_bytes(in, &r->pending, 6, stop)) {
            if (out_au->len > 0) {
                *is_keyframe = r->current_is_keyframe;
                r->have_vcl = false;
                r->current_is_keyframe = false;
                vec_reset(&r->pending);
                return true;
            }
            return false;
        }

        size_t sc_off = 0, sc_len = 0;
        if (!find_start_code(r->pending.data, r->pending.len, &sc_off, &sc_len)) {
            uint8_t tmp[4096];
            size_t n = fread(tmp, 1, sizeof(tmp), in);
            if (n == 0)
                return false;
            if (!vec_append(&r->pending, tmp, n))
                return false;
            continue;
        }

        if (sc_off > 0) {
            memmove(r->pending.data, r->pending.data + sc_off, r->pending.len - sc_off);
            r->pending.len -= sc_off;
        }

        size_t next_off = 0, next_sc_len = 0;
        if (!find_start_code(r->pending.data + sc_len,
                             r->pending.len - sc_len,
                             &next_off, &next_sc_len)) {
            uint8_t tmp[4096];
            size_t n = fread(tmp, 1, sizeof(tmp), in);
            if (n == 0) {
                if (r->pending.len > sc_len) {
                    if (!vec_append(out_au, r->pending.data, r->pending.len))
                        return false;
                    const uint8_t *nal = r->pending.data + sc_len;
                    size_t nal_len = r->pending.len - sc_len;
                    uint8_t nal_type = nal_len ? (nal[0] & 0x1f) : 0;
                    if (nal_type == 5)
                        r->current_is_keyframe = true;
                    *is_keyframe = r->current_is_keyframe;
                    vec_reset(&r->pending);
                    r->have_vcl = false;
                    r->current_is_keyframe = false;
                    return true;
                }
                return false;
            }
            if (!vec_append(&r->pending, tmp, n))
                return false;
            continue;
        }

        size_t next_abs = sc_len + next_off;
        const uint8_t *nal = r->pending.data + sc_len;
        size_t nal_len = next_abs - sc_len;
        uint8_t nal_type = nal_len ? (nal[0] & 0x1f) : 0;
        bool vcl = nal_is_vcl(nal_type);
        bool au_boundary = false;

        if (vcl) {
            bool first_mb_zero = starts_new_au_on_vcl(nal, nal_len);
            if (r->have_vcl && first_mb_zero && out_au->len > 0)
                au_boundary = true;
        } else if ((nal_type == 9 || nal_type == 7 || nal_type == 8 || nal_type == 6) &&
                   r->have_vcl && out_au->len > 0) {
            au_boundary = true;
        }

        if (au_boundary) {
            *is_keyframe = r->current_is_keyframe;
            r->have_vcl = false;
            r->current_is_keyframe = false;
            return true;
        }

        if (!vec_append(out_au, r->pending.data, next_abs))
            return false;

        if (nal_type == 5)
            r->current_is_keyframe = true;
        if (vcl)
            r->have_vcl = true;

        memmove(r->pending.data, r->pending.data + next_abs,
                r->pending.len - next_abs);
        r->pending.len -= next_abs;
    }

    return false;
}

static void payload_release(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)len;
    free((void *)data);
}

static void drain_before_stop(moq_endpoint_t *ep)
{
    moq_result_t dr = moq_endpoint_drain(ep, ENDPOINT_DRAIN_TIMEOUT_US);
    if (dr == MOQ_DONE) {
        fprintf(stderr, "endpoint drain timed out; stopping anyway\n");
    } else if (dr == MOQ_ERR_UNSUPPORTED) {
        fprintf(stderr, "endpoint drain unsupported by this backend; stopping\n");
    } else if (dr != MOQ_OK && dr != MOQ_ERR_CLOSED) {
        fprintf(stderr, "endpoint drain failed: %d; stopping\n", (int)dr);
    }
}

static uint64_t realtime_time_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000ull);
}

static uint64_t monotonic_time_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000ull);
}

/* One epoch for every source, so timestamps stay comparable across tracks and
 * across on-demand restarts (a per-source frame counter would reset to zero and
 * make an idle gap look like no time had passed). */
static uint64_t g_epoch_us;

static uint64_t stream_time_us(void)
{
    uint64_t now = monotonic_time_us();
    return now > g_epoch_us ? now - g_epoch_us : 0;
}

/* ── Per-source ingest thread ────────────────────────────────────────── */

/* Open the source's byte stream. SPAWN sources inherit the pipe fd the
 * supervisor already created; FIFO sources poll until a writer appears. */
static FILE *source_open(source_t *s)
{
    if (s->input == SRC_INPUT_STDIN)
        return stdin;

    if (s->input == SRC_INPUT_SPAWN) {
        FILE *in = fdopen(s->read_fd, "rb");
        if (!in) {
            fprintf(stderr, "source '%s': fdopen: %s\n", s->name, strerror(errno));
            close(s->read_fd);
        }
        s->read_fd = -1;
        return in;
    }

    /* FIFO: opening O_RDONLY blocks until a writer arrives, which would make
     * the join at shutdown hang. Poll with O_NONBLOCK instead. */
    int fd = -1;
    while (!halted(&s->reader_stop)) {
        fd = open(s->fifo_path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0)
            break;
        if (errno == ENXIO || errno == ENOENT) {
            usleep(100000);
            continue;
        }
        fprintf(stderr, "source '%s': open '%s': %s\n",
                s->name, s->fifo_path, strerror(errno));
        return NULL;
    }
    if (fd < 0)
        return NULL;

    FILE *in = fdopen(fd, "rb");
    if (!in) {
        fprintf(stderr, "source '%s': fdopen '%s': %s\n",
                s->name, s->fifo_path, strerror(errno));
        close(fd);
    }
    return in;
}

/* A track takes this access unit only when it wants it AND someone is watching.
 * The sender's send queue is one ring shared by every track, drained in strict
 * order, and an entry for a track with no subscriber holds the head -- so
 * enqueueing for an unwatched track stalls every other track behind it. */
static bool track_accepts(const app_t *app, const track_t *t, bool keyframe)
{
    if (t->keyframes_only && !keyframe)
        return false;
    return moq_media_sender_track_has_subscriber(app->tx, t->handle);
}

/* Publish one access unit to every track fed by this source. The payload is
 * shared: each write consumes exactly one reference, so pre-charge the
 * refcount to the number of writes we are about to attempt. */
static bool source_publish(source_t *s, const byte_vec_t *au, bool keyframe)
{
    /* Snapshot the decision: demand can change under us, and the refcount we
     * pre-charge below must match the number of writes exactly. */
    bool accept[MAX_TRACKS];
    int writers = 0;
    for (int i = 0; i < s->n_tracks; i++) {
        accept[i] = track_accepts(s->app, s->tracks[i], keyframe);
        if (accept[i])
            writers++;
    }
    if (writers == 0)
        return true;

    uint8_t *owned = (uint8_t *)malloc(au->len);
    if (!owned)
        return false;
    memcpy(owned, au->data, au->len);

    moq_rcbuf_t *payload = NULL;
    if (moq_rcbuf_wrap(moq_alloc_default(), owned, au->len,
                       payload_release, NULL, &payload) != MOQ_OK) {
        free(owned);
        return false;
    }
    for (int i = 1; i < writers; i++)
        moq_rcbuf_incref(payload);

    int refs_held = writers;
    uint64_t pts = stream_time_us();
    uint64_t capture = realtime_time_us();
    bool ok = true;

    for (int i = 0; i < s->n_tracks && refs_held > 0; i++) {
        track_t *t = s->tracks[i];
        if (!accept[i])
            continue;

        moq_media_send_object_t o;
        memset(&o, 0, sizeof(o));
        o.struct_size          = sizeof(o);
        o.payload              = payload;
        o.is_sync              = keyframe;
        o.starts_group         = keyframe;
        o.presentation_time_us = pts;
        o.decode_time_us       = pts;
        o.has_capture_time     = true;
        o.capture_time_us      = capture;

        moq_result_t wr = moq_media_sender_write(s->app->tx, t->handle, &o);
        refs_held--;
        if (wr == MOQ_OK) {
            s->objects_sent++;
            continue;
        }
        moq_rcbuf_decref(payload);
        if (wr == MOQ_ERR_WOULD_BLOCK)
            continue;   /* transient: the send queue is full, drop this object */
        if (wr != MOQ_ERR_INTERRUPTED && wr != MOQ_ERR_CLOSED)
            fprintf(stderr, "track '%s' write failed: %d\n", t->name, (int)wr);
        ok = false;
        break;
    }

    while (refs_held-- > 0)
        moq_rcbuf_decref(payload);
    return ok;
}

static void *source_reader(void *arg)
{
    source_t *s = (source_t *)arg;

    FILE *in = source_open(s);
    if (!in) {
        s->reader_done = 1;
        return NULL;
    }

    h264_annexb_reader_t reader;
    h264_reader_init(&reader);
    byte_vec_t au = {0};
    bool started = false;

    while (!halted(&s->reader_stop)) {
        bool keyframe = false;
        if (!read_h264_access_unit(in, &reader, &au, &keyframe, &s->reader_stop))
            break;
        if (au.len == 0)
            continue;
        if (!started) {
            if (!keyframe)
                continue;   /* a subscriber can only decode from an IDR */
            started = true;
            fprintf(stderr, "source '%s': first keyframe; publishing\n", s->name);
        }
        if (!source_publish(s, &au, keyframe))
            break;
    }

    if (in != stdin)
        fclose(in);
    h264_reader_free(&reader);
    vec_free(&au);
    s->reader_done = 1;
    return NULL;
}

/* ── ffmpeg child lifecycle ──────────────────────────────────────────── */

static bool source_spawn_ffmpeg(source_t *s)
{
    int fds[2];
    if (pipe(fds) != 0) {
        fprintf(stderr, "source '%s': pipe: %s\n", s->name, strerror(errno));
        return false;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);

    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    /* Own process group: the supervisor signals -pgid so one source's teardown
     * can never reach a sibling ffmpeg. */
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);

    /* argv, never a shell command string: the RTSP URL carries credentials and
     * arbitrary query characters. */
    char *argv[] = {
        (char *)"ffmpeg",
        (char *)"-nostdin",
        (char *)"-loglevel", (char *)"warning",
        (char *)"-rtsp_transport", (char *)"tcp",
        (char *)"-fflags", (char *)"nobuffer",
        (char *)"-flags", (char *)"low_delay",
        (char *)"-max_delay", (char *)"0",
        (char *)"-reorder_queue_size", (char *)"0",
        (char *)"-use_wallclock_as_timestamps", (char *)"1",
        (char *)"-analyzeduration", (char *)"100000",
        (char *)"-probesize", (char *)"32768",
        (char *)"-i", (char *)s->rtsp_url,
        (char *)"-map", (char *)"0:v:0",
        (char *)"-an",
        (char *)"-c:v", (char *)"copy",
        (char *)"-bsf:v", (char *)"h264_mp4toannexb",
        (char *)"-f", (char *)"h264", (char *)"pipe:1",
        NULL
    };

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, s->app->ffmpeg_bin, &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(fds[1]);

    if (rc != 0) {
        close(fds[0]);
        fprintf(stderr, "source '%s': spawn %s: %s\n",
                s->name, s->app->ffmpeg_bin, strerror(rc));
        return false;
    }

    s->pid = pid;
    s->read_fd = fds[0];
    return true;
}

static bool source_start(source_t *s)
{
    s->reader_stop = 0;
    s->reader_done = 0;
    s->objects_sent = 0;
    s->idle_deadline_us = 0;
    s->read_fd = -1;
    s->pid = -1;

    if (s->input == SRC_INPUT_SPAWN && !source_spawn_ffmpeg(s))
        return false;

    if (pthread_create(&s->thread, NULL, source_reader, s) != 0) {
        fprintf(stderr, "source '%s': pthread_create failed\n", s->name);
        if (s->pid > 0)
            kill(-s->pid, SIGKILL);
        if (s->read_fd >= 0)
            close(s->read_fd);
        return false;
    }

    s->thread_live = true;
    s->started_us = monotonic_time_us();
    s->state = SRC_RUNNING;
    fprintf(stderr, "source '%s': started\n", s->name);
    return true;
}

/* Ask the source to wind down. For a spawned source, killing ffmpeg closes the
 * pipe and the reader ends on EOF; setting the stop flag first would close the
 * read end under ffmpeg and make it log a broken pipe on the way out. The flag
 * is only needed where there is no child to kill. */
static void source_begin_stop(source_t *s)
{
    if (s->state != SRC_RUNNING)
        return;
    if (s->pid > 0)
        kill(-s->pid, SIGTERM);
    else
        s->reader_stop = 1;
    s->kill_deadline_us = monotonic_time_us() + TERM_GRACE_US;
    s->state = SRC_STOPPING;
    fprintf(stderr, "source '%s': stopping (%llu objects)\n",
            s->name, s->objects_sent);
}

/* True once the child is reaped and the reader thread has been joined. */
static bool source_settle(source_t *s)
{
    if (s->pid > 0) {
        int st = 0;
        pid_t r = waitpid(s->pid, &st, WNOHANG);
        if (r == 0) {
            if (monotonic_time_us() >= s->kill_deadline_us) {
                s->reader_stop = 1;
                kill(-s->pid, SIGKILL);
            }
            return false;
        }
        if (r < 0 && errno == EINTR)
            return false;
        s->pid = -1;
    }
    if (!s->reader_done)
        return false;
    if (s->thread_live) {
        pthread_join(s->thread, NULL);
        s->thread_live = false;
    }
    s->state = SRC_IDLE;
    return true;
}

/* The reader ended on its own: ffmpeg died, the camera dropped, or the fifo
 * writer went away. Distinguish that from a demand-driven stop so a flapping
 * source backs off instead of hammering the camera. */
static void source_handle_failure(source_t *s)
{
    uint64_t now = monotonic_time_us();
    bool healthy = now - s->started_us >= HEALTHY_RUN_US;

    s->reader_stop = 1;
    if (s->pid > 0)
        kill(-s->pid, SIGTERM);
    s->kill_deadline_us = now + TERM_GRACE_US;
    s->state = SRC_STOPPING;

    if (healthy)
        s->failures = 0;
    s->failures++;

    uint64_t backoff = BACKOFF_BASE_US;
    for (unsigned i = 1; i < s->failures && backoff < BACKOFF_MAX_US; i++)
        backoff *= 2;
    if (backoff > BACKOFF_MAX_US)
        backoff = BACKOFF_MAX_US;
    s->backoff_until_us = now + backoff;

    fprintf(stderr, "source '%s': input ended unexpectedly (failure %u); "
                    "retrying in %llums if demand persists\n",
            s->name, s->failures, (unsigned long long)(backoff / 1000));
}

/* ── Demand-driven supervisor ────────────────────────────────────────── */

static size_t source_demand(const app_t *app, const source_t *s)
{
    size_t n = 0;
    for (int i = 0; i < s->n_tracks; i++)
        n += moq_media_sender_track_subscriptions(app->tx, s->tracks[i]->handle);
    return n;
}

/* A source is wanted when it has its own demand, when it is pinned always-on,
 * or when another source it warms is in demand (so a quality switch does not
 * pay an RTSP connect plus a GOP). */
static bool source_wanted(const app_t *app, const source_t *s)
{
    if (s->always_on)
        return true;
    if (source_demand(app, s) > 0)
        return true;
    for (int i = 0; i < app->n_sources; i++) {
        const source_t *o = &app->sources[i];
        if (o != s && o->warm_with && strcmp(o->warm_with, s->name) == 0 &&
            source_demand(app, o) > 0)
            return true;
    }
    return false;
}

static void supervisor_tick(app_t *app)
{
    uint64_t now = monotonic_time_us();

    for (int i = 0; i < app->n_sources; i++) {
        source_t *s = &app->sources[i];
        bool wanted = !g_stop && source_wanted(app, s);

        switch (s->state) {
        case SRC_IDLE:
            if (wanted && now >= s->backoff_until_us && !source_start(s))
                s->backoff_until_us = now + BACKOFF_BASE_US;
            break;

        case SRC_RUNNING:
            if (s->reader_done) {
                if (s->input == SRC_INPUT_STDIN) {
                    /* stdin cannot be reopened; treat EOF as end of process. */
                    fprintf(stderr, "source '%s': stdin closed; shutting down\n",
                            s->name);
                    g_stop = 1;
                    source_begin_stop(s);
                } else {
                    source_handle_failure(s);
                }
            } else if (!wanted) {
                if (s->idle_deadline_us == 0) {
                    s->idle_deadline_us = now + s->idle_us;
                    fprintf(stderr, "source '%s': no demand; stopping in %llums\n",
                            s->name, (unsigned long long)(s->idle_us / 1000));
                } else if (now >= s->idle_deadline_us) {
                    source_begin_stop(s);
                }
            } else if (s->idle_deadline_us != 0) {
                s->idle_deadline_us = 0;
                fprintf(stderr, "source '%s': demand returned; staying up\n", s->name);
            }
            break;

        case SRC_STOPPING:
            source_settle(s);
            break;
        }
    }
}

static void supervisor_shutdown(app_t *app)
{
    for (int i = 0; i < app->n_sources; i++)
        source_begin_stop(&app->sources[i]);

    for (int i = 0; i < app->n_sources; i++) {
        source_t *s = &app->sources[i];
        while (s->state == SRC_STOPPING && !source_settle(s))
            usleep(20000);
    }
}

/* ── Demand callbacks ────────────────────────────────────────────────── */

/* Network-thread callbacks: log only. They must not touch sender mutators, and
 * the supervisor re-reads the authoritative counts each tick anyway. */
static void on_subscriber_joined(void *ctx, moq_media_sender_t *sender,
                                 moq_media_track_t *track, size_t active)
{
    app_t *app = (app_t *)ctx;
    (void)sender;
    for (int i = 0; i < app->n_tracks; i++) {
        if (app->tracks[i].handle == track) {
            fprintf(stderr, "demand: +1 track '%s' (%zu active)\n",
                    app->tracks[i].name, active);
            return;
        }
    }
}

static void on_subscriber_left(void *ctx, moq_media_sender_t *sender,
                               moq_media_track_t *track, size_t active)
{
    app_t *app = (app_t *)ctx;
    (void)sender;
    for (int i = 0; i < app->n_tracks; i++) {
        if (app->tracks[i].handle == track) {
            fprintf(stderr, "demand: -1 track '%s' (%zu active)\n",
                    app->tracks[i].name, active);
            return;
        }
    }
}

/* ── CLI ─────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s <url> <namespace> [global options] [source/track definitions]\n"
        "\n"
        "global:\n"
        "  --insecure-skip-verify        disable TLS certificate verification\n"
        "  --catalog-keepalive-ms N      catalog refresh interval in ms (0 disables)\n"
        "  --ffmpeg <path>               ffmpeg binary (default \"ffmpeg\")\n"
        "  --queue-objects N             shared send-queue depth (0 = 2s of media)\n"
        "  --stats-ms N                  log sender stats every N ms (0 = off)\n"
        "\n"
        "source (repeatable; options apply to the preceding --source):\n"
        "  --source <name>               begin a source definition\n"
        "  --rtsp <url>                  RTSP input; media_send runs ffmpeg on demand\n"
        "  --fifo <path>                 read Annex-B from an external named pipe\n"
        "  --stdin                       read Annex-B from stdin\n"
        "  --idle-ms N                   linger after last unsubscribe (default 5000)\n"
        "  --always-on                   ignore demand; keep the source running\n"
        "  --warm-with <source>          run this source while <source> has demand\n"
        "\n"
        "track (repeatable; options apply to the preceding --track):\n"
        "  --track <name>                begin a track definition\n"
        "  --from <source>               source feeding this track (default: first)\n"
        "  --width N  --height N         catalog dimensions\n"
        "  --fps N                       catalog framerate (default 30)\n"
        "  --bitrate N                   max bitrate bits/s (default 1500000)\n"
        "  --keyframes-only              publish only sync samples\n",
        prog);
}

static const char *need_arg(int argc, char **argv, int *i, const char *flag)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "%s requires a value\n", flag);
        exit(2);
    }
    return argv[++(*i)];
}

static source_t *find_source(app_t *app, const char *name)
{
    for (int i = 0; i < app->n_sources; i++) {
        if (strcmp(app->sources[i].name, name) == 0)
            return &app->sources[i];
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    const char *url = argv[1];
    char nsbuf[256];
    snprintf(nsbuf, sizeof(nsbuf), "%s", argv[2]);
    moq_bytes_t ns_parts[32];
    size_t ns_count = split_namespace(nsbuf, ns_parts, 32);

    app_t app;
    memset(&app, 0, sizeof(app));
    app.ffmpeg_bin = "ffmpeg";

    bool     insecure_skip_verify   = false;
    bool     has_catalog_keepalive  = false;
    uint64_t catalog_keepalive_ms   = 0;
    uint32_t queue_objects          = 0;
    uint64_t stats_interval_us      = 0;

    source_t *cur_src = NULL;
    track_t  *cur_trk = NULL;

    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--insecure-skip-verify") == 0) {
            insecure_skip_verify = true;
        } else if (strcmp(a, "--catalog-keepalive-ms") == 0) {
            catalog_keepalive_ms = strtoull(need_arg(argc, argv, &i, a), NULL, 10);
            has_catalog_keepalive = true;
        } else if (strcmp(a, "--ffmpeg") == 0) {
            app.ffmpeg_bin = need_arg(argc, argv, &i, a);
        } else if (strcmp(a, "--queue-objects") == 0) {
            queue_objects = (uint32_t)strtoul(need_arg(argc, argv, &i, a), NULL, 10);
        } else if (strcmp(a, "--stats-ms") == 0) {
            stats_interval_us = strtoull(need_arg(argc, argv, &i, a), NULL, 10) * 1000ull;
        } else if (strcmp(a, "--source") == 0) {
            if (app.n_sources >= MAX_SOURCES) {
                fprintf(stderr, "too many sources (max %d)\n", MAX_SOURCES);
                return 2;
            }
            cur_src = &app.sources[app.n_sources++];
            cur_src->name    = need_arg(argc, argv, &i, a);
            cur_src->input   = SRC_INPUT_SPAWN;
            cur_src->idle_us = DEFAULT_IDLE_US;
            cur_src->app     = &app;
            cur_src->read_fd = -1;
            cur_src->pid     = -1;
            cur_trk = NULL;
        } else if (strcmp(a, "--track") == 0) {
            if (app.n_tracks >= MAX_TRACKS) {
                fprintf(stderr, "too many tracks (max %d)\n", MAX_TRACKS);
                return 2;
            }
            cur_trk = &app.tracks[app.n_tracks++];
            cur_trk->name    = need_arg(argc, argv, &i, a);
            cur_trk->fps     = 30;
            cur_trk->bitrate = 1500000;
        } else if (strcmp(a, "--rtsp") == 0 || strcmp(a, "--fifo") == 0 ||
                   strcmp(a, "--stdin") == 0 || strcmp(a, "--idle-ms") == 0 ||
                   strcmp(a, "--always-on") == 0 || strcmp(a, "--warm-with") == 0) {
            if (!cur_src) {
                fprintf(stderr, "%s must follow a --source\n", a);
                return 2;
            }
            if (strcmp(a, "--rtsp") == 0) {
                cur_src->rtsp_url = need_arg(argc, argv, &i, a);
                cur_src->input = SRC_INPUT_SPAWN;
            } else if (strcmp(a, "--fifo") == 0) {
                cur_src->fifo_path = need_arg(argc, argv, &i, a);
                cur_src->input = SRC_INPUT_FIFO;
                cur_src->always_on = true;   /* an external writer sets the pace */
            } else if (strcmp(a, "--stdin") == 0) {
                cur_src->input = SRC_INPUT_STDIN;
                cur_src->always_on = true;
            } else if (strcmp(a, "--idle-ms") == 0) {
                cur_src->idle_us = strtoull(need_arg(argc, argv, &i, a), NULL, 10) * 1000ull;
            } else if (strcmp(a, "--always-on") == 0) {
                cur_src->always_on = true;
            } else {
                cur_src->warm_with = need_arg(argc, argv, &i, a);
            }
        } else if (strcmp(a, "--from") == 0 || strcmp(a, "--width") == 0 ||
                   strcmp(a, "--height") == 0 || strcmp(a, "--fps") == 0 ||
                   strcmp(a, "--bitrate") == 0 || strcmp(a, "--keyframes-only") == 0) {
            if (!cur_trk) {
                fprintf(stderr, "%s must follow a --track\n", a);
                return 2;
            }
            if (strcmp(a, "--from") == 0)
                cur_trk->from = need_arg(argc, argv, &i, a);
            else if (strcmp(a, "--width") == 0)
                cur_trk->width = (uint32_t)strtoul(need_arg(argc, argv, &i, a), NULL, 10);
            else if (strcmp(a, "--height") == 0)
                cur_trk->height = (uint32_t)strtoul(need_arg(argc, argv, &i, a), NULL, 10);
            else if (strcmp(a, "--fps") == 0)
                cur_trk->fps = (uint32_t)strtoul(need_arg(argc, argv, &i, a), NULL, 10);
            else if (strcmp(a, "--bitrate") == 0)
                cur_trk->bitrate = strtoull(need_arg(argc, argv, &i, a), NULL, 10);
            else
                cur_trk->keyframes_only = true;
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (app.n_sources == 0 || app.n_tracks == 0) {
        fprintf(stderr, "at least one --source and one --track are required\n");
        usage(argv[0]);
        return 2;
    }

    /* Resolve track -> source bindings and validate each source. */
    for (int i = 0; i < app.n_tracks; i++) {
        track_t *t = &app.tracks[i];
        source_t *s = t->from ? find_source(&app, t->from) : &app.sources[0];
        if (!s) {
            fprintf(stderr, "track '%s': unknown source '%s'\n", t->name, t->from);
            return 2;
        }
        if (s->n_tracks >= MAX_TRACKS) {
            fprintf(stderr, "source '%s': too many tracks\n", s->name);
            return 2;
        }
        t->src = s;
        s->tracks[s->n_tracks++] = t;
    }
    for (int i = 0; i < app.n_sources; i++) {
        source_t *s = &app.sources[i];
        if (s->n_tracks == 0) {
            fprintf(stderr, "source '%s': no tracks bound to it\n", s->name);
            return 2;
        }
        if (s->input == SRC_INPUT_SPAWN && !s->rtsp_url) {
            fprintf(stderr, "source '%s': needs --rtsp, --fifo or --stdin\n", s->name);
            return 2;
        }
        if (s->warm_with && !find_source(&app, s->warm_with)) {
            fprintf(stderr, "source '%s': unknown --warm-with '%s'\n",
                    s->name, s->warm_with);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);   /* a dying ffmpeg must not kill the publisher */

    moq_endpoint_cfg_t ec;
    moq_endpoint_cfg_init(&ec);
    ec.url.data = (const uint8_t *)url;
    ec.url.len = strlen(url);
    ec.insecure_skip_verify = insecure_skip_verify;

    moq_endpoint_t *ep = NULL;
    moq_result_t rc = moq_endpoint_connect(&ec, &ep);
    if (rc != MOQ_OK) {
        fprintf(stderr, "endpoint connect failed: %d\n", (int)rc);
        return 1;
    }

    moq_media_sender_cfg_t scfg;
    moq_media_sender_cfg_init_live_sized(&scfg, sizeof(scfg));
    scfg.endpoint = NULL;
    scfg.namespace_.parts = ns_parts;
    scfg.namespace_.count = ns_count;
    if (has_catalog_keepalive) {
        scfg.catalog_refresh_interval_us =
            catalog_keepalive_ms == 0 ? UINT64_MAX : (catalog_keepalive_ms * 1000ull);
    }

    /* The send queue is one ring shared by every track and drained in strict
     * order, so its object bound is the latency floor: a full ring costs
     * queue_max_objects / (total objects per second) of delay. Default to
     * QUEUE_SECONDS worth of the configured object rate. */
    if (queue_objects == 0) {
        uint32_t obj_per_sec = 0;
        for (int i = 0; i < app.n_tracks; i++)
            obj_per_sec += app.tracks[i].keyframes_only ? 1 : app.tracks[i].fps;
        queue_objects = obj_per_sec * QUEUE_SECONDS;
        if (queue_objects < QUEUE_MIN_OBJECTS)
            queue_objects = QUEUE_MIN_OBJECTS;
    }
    scfg.queue_max_objects = queue_objects;

    /* Without this, an object queued for a track with no subscriber holds the
     * head of the shared ring and blocks every other track behind it. */
    scfg.drop_without_demand = true;

    moq_media_sender_callbacks_init_sized(&scfg.callbacks, sizeof(scfg.callbacks));
    scfg.callbacks.ctx = &app;
    scfg.callbacks.on_subscriber_joined = on_subscriber_joined;
    scfg.callbacks.on_subscriber_left = on_subscriber_left;

    rc = moq_media_sender_attach(ep, &scfg, &app.tx);
    if (rc != MOQ_OK) {
        fprintf(stderr, "sender attach failed: %d\n", (int)rc);
        moq_endpoint_stop(ep);
        moq_endpoint_destroy(ep);
        return 1;
    }

    for (int i = 0; i < app.n_tracks; i++) {
        track_t *t = &app.tracks[i];
        moq_media_track_cfg_t tc;
        moq_media_track_cfg_init(&tc);
        tc.name.data        = (const uint8_t *)t->name;
        tc.name.len         = strlen(t->name);
        tc.media_type       = MOQ_MEDIA_TYPE_VIDEO;
        tc.packaging        = MOQ_MEDIA_PACKAGING_RAW;
        tc.codec.data       = (const uint8_t *)"avc1.42e01e";
        tc.codec.len        = 11;
        tc.bitrate          = t->bitrate;
        tc.width            = t->width;
        tc.height           = t->height;
        tc.framerate_millis = t->keyframes_only ? 0 : (uint64_t)t->fps * 1000u;
        tc.is_live          = true;

        rc = moq_media_sender_add_track(app.tx, &tc, &t->handle);
        if (rc != MOQ_OK) {
            fprintf(stderr, "add_track '%s' failed: %d\n", t->name, (int)rc);
            moq_media_sender_destroy(app.tx);
            moq_endpoint_stop(ep);
            moq_endpoint_destroy(ep);
            return 1;
        }
        fprintf(stderr, "track '%s' <- source '%s'%s\n",
                t->name, t->src->name, t->keyframes_only ? " (keyframes only)" : "");
    }

    g_epoch_us = monotonic_time_us();
    fprintf(stderr, "announced; send queue bound %u objects; waiting for subscribers\n",
            queue_objects);

    uint64_t next_stats_us = stats_interval_us ? monotonic_time_us() + stats_interval_us : 0;

    while (!g_stop) {
        supervisor_tick(&app);
        if (moq_media_sender_is_closed(app.tx) || moq_media_sender_is_fatal(app.tx)) {
            fprintf(stderr, "sender terminal (fatal=%d code=0x%llx)\n",
                    (int)moq_media_sender_is_fatal(app.tx),
                    (unsigned long long)moq_media_sender_fatal_code(app.tx));
            break;
        }
        if (next_stats_us && monotonic_time_us() >= next_stats_us) {
            moq_media_sender_stats_t st;
            memset(&st, 0, sizeof(st));
            if (moq_media_sender_get_stats(app.tx, &st, sizeof(st)) == MOQ_OK) {
                fprintf(stderr,
                        "stats: queued=%llu/%u bytes_queued=%llu written=%llu sent=%llu "
                        "dropped=%llu groups_dropped=%llu abandoned=%llu last_err=%d\n",
                        (unsigned long long)st.objects_queued, queue_objects,
                        (unsigned long long)st.bytes_queued,
                        (unsigned long long)st.objects_written,
                        (unsigned long long)st.objects_sent,
                        (unsigned long long)st.objects_dropped,
                        (unsigned long long)st.groups_dropped,
                        (unsigned long long)st.groups_abandoned,
                        (int)st.last_error);
            }
            next_stats_us = monotonic_time_us() + stats_interval_us;
        }
        usleep(SUPERVISOR_TICK_US);
    }

    supervisor_shutdown(&app);

    moq_media_sender_destroy(app.tx);
    drain_before_stop(ep);
    moq_endpoint_stop(ep);
    moq_endpoint_destroy(ep);
    return 0;
}

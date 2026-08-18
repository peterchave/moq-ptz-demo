/*
 * cmake --build /Users/pchave/Documents/Alpha/MoQ/ptz-demo/moq5/build --target moq_example_ptz_control
 *
 * ptz_control.c — PTZ camera controller via MOQT.
 *
 * Subscribes DIRECTLY to a namespace/track on a MoQ relay (no MSF catalog
 * required) and translates atomic JSON move commands into Amcrest CGI calls.
 *
 * Uses moq::adapter-picoquic-threaded + moq_subscriber_t — works with any
 * publisher that sends objects to the relay, including browsers using
 * connection.publish() without an MSF catalog.
 *
 * Command JSON format (UTF-8 bytes):
 *   Move:   {"type":"ptz","seq":N,"ts":T,"action":"move","axis":"pan","value":-0.4,"duration_ms":300}
 *   Preset: {"type":"ptz","seq":N,"ts":T,"action":"preset","preset_id":3}
 *
 * axis:  "pan"  (value < 0 = left,  > 0 = right)
 *        "tilt" (value < 0 = down,  > 0 = up)
 *        "zoom" (value < 0 = out,   > 0 = in)
 *
 * Usage:
 *   ptz_control <url> <namespace> [track] [options]
 *
 * Options:
 *   --cam-ip <ip>            camera IP (default: CAMERA_IP)
 *   --cam-user <user>        camera username (default: admin)
 *   --cam-pass <pass>        camera password
 *   --cam-speed <1-8>        default PTZ speed (default: 5)
 *   --max-duration-ms <N>    safety cap per move (default: 2000)
 *   --curl-path <path>       path to curl binary (default: curl)
 *   --insecure-skip-verify   skip TLS certificate check
 */

#include <moq/picoquic_threaded.h>
#include <moq/rcbuf.h>
#include <moq/url.h>
#include <moq/session.h>

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Platform sleep ─────────────────────────────────────────────────── */

#ifdef _WIN32
#  include <windows.h>
static void sleep_ms(uint32_t ms) { Sleep(ms); }
#else
#  include <unistd.h>
static void sleep_ms(uint32_t ms)
{
    struct timespec ts = { .tv_sec  = ms / 1000,
                           .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* ── Signal ─────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_stop    = 0;
static moq_pq_threaded_t    *g_threaded = NULL;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_threaded) moq_pq_threaded_wake(g_threaded);
}

/* ── Camera configuration ───────────────────────────────────────────── */

typedef struct {
    const char *cam_ip;
    const char *cam_user;
    const char *cam_pass;
    uint32_t    cam_speed;
    uint32_t    max_duration_ms;
    const char *curl_path;
} cam_cfg_t;

/* ── Amcrest direction codes ─────────────────────────────────────────── */

typedef enum { PTZ_UP, PTZ_DOWN, PTZ_LEFT, PTZ_RIGHT,
               PTZ_ZOOM_IN, PTZ_ZOOM_OUT, PTZ_PRESET } ptz_dir_t;

static const char *ptz_code(ptz_dir_t d)
{
    switch (d) {
        case PTZ_UP:       return "Up";
        case PTZ_DOWN:     return "Down";
        case PTZ_LEFT:     return "Left";
        case PTZ_RIGHT:    return "Right";
        case PTZ_ZOOM_IN:  return "ZoomTele";
        case PTZ_ZOOM_OUT: return "ZoomWide";
        case PTZ_PRESET:   return "GotoPreset";
        default:           return NULL;
    }
}

/* ── Camera HTTP via curl CLI ───────────────────────────────────────── */

static bool cam_cgi(const cam_cfg_t *cfg, const char *action,
                    const char *code, uint32_t speed, uint32_t arg2)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "%s -X POST -s --max-time 3 --digest"
             " -u '%s:%s'"
             " 'http://%s/cgi-bin/ptz.cgi?action=%s&channel=0&code=%s&arg1=%u&arg2=%u&arg3=0'"
             " > /dev/null 2>&1",
             cfg->curl_path, cfg->cam_user, cfg->cam_pass,
             cfg->cam_ip, action, code, speed, arg2);

    fprintf(stderr, "cam_cgi cmd: %s\n", cmd);
    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "cam_cgi %s %s returned %d\n", action, code, rc);
    return rc == 0;
}

/* ── Command queue ───────────────────────────────────────────────────── */

typedef struct {
    bool      is_preset;
    ptz_dir_t dir;
    uint32_t  speed;
    uint32_t  duration_ms;
    int64_t   preset_id;
} cam_cmd_t;

#define CMD_DEPTH 16

typedef struct {
    cam_cmd_t       items[CMD_DEPTH];
    int             head, tail, count;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            stopped;
} cmd_queue_t;

static void q_init(cmd_queue_t *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
}

static bool q_push(cmd_queue_t *q, const cam_cmd_t *c) {
    pthread_mutex_lock(&q->mu);
    if (q->stopped) { pthread_mutex_unlock(&q->mu); return false; }
    if (q->count < CMD_DEPTH) {
        q->items[q->head] = *c;
        q->head = (q->head + 1) % CMD_DEPTH;
        q->count++;
    } else {
        /* Full: overwrite oldest (advance tail past the slot we're reusing) */
        q->items[q->head] = *c;
        q->head = (q->head + 1) % CMD_DEPTH;
        q->tail = (q->tail + 1) % CMD_DEPTH;
        fprintf(stderr, "PTZ: queue full, oldest dropped\n");
    }
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return true;
}

static bool q_pop(cmd_queue_t *q, cam_cmd_t *out) {
    pthread_mutex_lock(&q->mu);
    while (!q->stopped && q->count == 0) pthread_cond_wait(&q->cv, &q->mu);
    if (q->stopped && q->count == 0) { pthread_mutex_unlock(&q->mu); return false; }
    *out = q->items[q->tail];
    q->tail = (q->tail + 1) % CMD_DEPTH;
    q->count--;
    pthread_mutex_unlock(&q->mu);
    return true;
}

static void q_stop(cmd_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    q->stopped = true;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

/* ── Worker thread ───────────────────────────────────────────────────── */

typedef struct { cmd_queue_t *q; const cam_cfg_t *cfg; } worker_ctx_t;

static void *worker_thread(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    cam_cmd_t cmd;
    while (q_pop(ctx->q, &cmd)) {
        if (cmd.is_preset) {
            fprintf(stderr, "PTZ: goto preset %lld\n", (long long)cmd.preset_id);
            cam_cgi(ctx->cfg, "start", ptz_code(PTZ_PRESET), 0, (uint32_t)cmd.preset_id);
        } else {
            const char *code = ptz_code(cmd.dir);
            uint32_t dur = cmd.duration_ms;
            if (dur > ctx->cfg->max_duration_ms) dur = ctx->cfg->max_duration_ms;
            fprintf(stderr, "PTZ: %s speed=%u dur=%ums\n", code, cmd.speed, dur);
            cam_cgi(ctx->cfg, "start", code, cmd.speed, 0);
            sleep_ms(dur);
            cam_cgi(ctx->cfg, "stop",  code, cmd.speed, 0);
            fprintf(stderr, "PTZ: stopped\n");
        }
    }
    return NULL;
}

/* ── JSON helpers ────────────────────────────────────────────────────── */

static bool js_str(const char *s, const char *k, char *o, size_t n) {
    char p[128]; snprintf(p, sizeof(p), "\"%s\":\"", k);
    const char *f = strstr(s, p); if (!f) return false;
    f += strlen(p);
    const char *e = strchr(f, '"'); if (!e) return false;
    size_t l = (size_t)(e - f); if (l >= n) l = n-1;
    memcpy(o, f, l); o[l] = '\0'; return true;
}

static bool js_int(const char *s, const char *k, int64_t *o) {
    char p[128]; snprintf(p, sizeof(p), "\"%s\":", k);
    const char *f = strstr(s, p); if (!f) return false;
    f += strlen(p); char *e = NULL; *o = strtoll(f, &e, 10); return e != f;
}

static bool js_flt(const char *s, const char *k, double *o) {
    char p[128]; snprintf(p, sizeof(p), "\"%s\":", k);
    const char *f = strstr(s, p); if (!f) return false;
    f += strlen(p); char *e = NULL; *o = strtod(f, &e); return e != f;
}

/* ── Dispatch one object payload → command queue ────────────────────── */

static void dispatch(cmd_queue_t *q, const cam_cfg_t *cfg,
                     const uint8_t *data, size_t len)
{
    if (!data || !len) return;
    size_t l = len < 4095 ? len : 4095;
    char js[4096]; memcpy(js, data, l); js[l] = '\0';

    /* Skip keepalives */
    char type_buf[32] = {0}; js_str(js, "type", type_buf, sizeof(type_buf));
    if (strcmp(type_buf, "ptz") != 0) return;

    char action[32] = {0};
    if (!js_str(js, "action", action, sizeof(action))) {
        fprintf(stderr, "PTZ: no 'action' field\n"); return;
    }

    cam_cmd_t cmd; memset(&cmd, 0, sizeof(cmd));

    if (strcmp(action, "preset") == 0) {
        cmd.is_preset = true;
        int64_t pid = 1; js_int(js, "preset_id", &pid);
        cmd.preset_id = pid;
        q_push(q, &cmd); return;
    }

    if (strcmp(action, "move") != 0) {
        fprintf(stderr, "PTZ: unknown action '%s'\n", action); return;
    }

    char axis[16] = {0}; double value = 0.0;
    int64_t dur = 300, spd = 0;
    js_str(js, "axis",        axis,  sizeof(axis));
    js_flt(js, "value",       &value);
    js_int(js, "duration_ms", &dur);
    js_int(js, "speed",       &spd);
    if (dur <= 0) dur = 300;

    cmd.speed       = spd > 0 ? (uint32_t)spd : cfg->cam_speed;
    cmd.duration_ms = (uint32_t)dur;

    if      (!strcmp(axis, "pan"))  cmd.dir = value < 0.0 ? PTZ_LEFT     : PTZ_RIGHT;
    else if (!strcmp(axis, "tilt")) cmd.dir = value < 0.0 ? PTZ_DOWN     : PTZ_UP;
    else if (!strcmp(axis, "zoom")) cmd.dir = value < 0.0 ? PTZ_ZOOM_OUT : PTZ_ZOOM_IN;
    else { fprintf(stderr, "PTZ: unknown axis '%s'\n", axis); return; }

    q_push(q, &cmd);
}

/* ── Application context ─────────────────────────────────────────────── */

typedef struct {
    moq_ns_sub_handle_t ns_sub;
    moq_subscription_t  sub;
    bool                ns_sub_ok;
    bool                control_seen;
    bool                subscribed;
    moq_bytes_t        ns_parts[32];
    size_t             ns_count;
    const char        *track_name;
    cmd_queue_t        q;
    const cam_cfg_t   *cfg;
    unsigned long long n_commands;
    uint64_t           ns_subscribe_sent_us;
    uint64_t           subscribe_sent_us;
    bool               need_retry;        /* timeout — ask main to reconnect    */
    bool               dots_pending;      /* dots printed, needs \n before next  */
} app_ctx_t;

static bool bytes_eq_cstr(moq_bytes_t b, const char *s)
{
    size_t n = strlen(s);
    return b.data && b.len == n && memcmp(b.data, s, n) == 0;
}

static bool ns_suffix_is_exact_prefix(const moq_namespace_t *suffix)
{
    return suffix && suffix->count == 0;
}

static void log_ns_suffix(const char *prefix, const moq_namespace_t *suffix)
{
    fprintf(stderr, "%s", prefix);
    if (!suffix || suffix->count == 0) {
        fprintf(stderr, "<empty>\n");
        return;
    }

    for (size_t i = 0; i < suffix->count; i++) {
        fprintf(stderr, "%s%.*s", (i == 0 ? "" : "/"),
                (int)suffix->parts[i].len, suffix->parts[i].data);
    }
    fprintf(stderr, "\n");
}

static moq_result_t subscribe_command_track(moq_session_t *sess,
                                            const app_ctx_t *app,
                                            uint64_t now_us,
                                            moq_subscription_t *out)
{
    moq_subscribe_cfg_t sc;
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace.parts = app->ns_parts;
    sc.track_namespace.count = app->ns_count;
    sc.track_name.data       = (const uint8_t *)app->track_name;
    sc.track_name.len        = strlen(app->track_name);
    sc.filter                = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP;

    return moq_session_subscribe(sess, &sc, now_us, out);
}

/* ── on_pump (network thread) ───────────────────────────────────────── */

static int on_pump(moq_pq_threaded_t *t, moq_pq_threaded_lane_t *lane,
                   uint64_t now_us, void *vctx)
{
    (void)lane;
    app_ctx_t *app = (app_ctx_t *)vctx;
    if (g_stop) return 1;

    moq_session_t *sess = moq_pq_threaded_session(t);
    if (!sess) return 0;

    /* One-shot: subscribe namespace prefix once established. */
    if (!moq_ns_sub_handle_is_valid(app->ns_sub) &&
        moq_session_state(sess) == MOQ_SESS_ESTABLISHED) {
        moq_subscribe_namespace_cfg_t ncfg;
        moq_subscribe_namespace_cfg_init(&ncfg);
        ncfg.track_namespace_prefix.parts = app->ns_parts;
        ncfg.track_namespace_prefix.count = app->ns_count;
        ncfg.namespace_interest           = MOQ_NAMESPACE_INTEREST_BOTH;

        moq_result_t rc = moq_session_subscribe_namespace(sess, &ncfg, now_us, &app->ns_sub);
        if (rc == MOQ_OK) {
            app->ns_subscribe_sent_us = now_us;
            if (app->dots_pending) { fprintf(stderr, "\n"); app->dots_pending = false; }
            fprintf(stderr, "PTZ: SUBSCRIBE_NAMESPACE (%s) sent (handle=%llu)\n",
                    app->ns_parts[0].data, (unsigned long long)moq_ns_sub_handle_id_for_trace(app->ns_sub));
        } else if (rc != MOQ_ERR_WOULD_BLOCK && rc != MOQ_ERR_REQUEST_BLOCKED) {
            fprintf(stderr, "PTZ: moq_session_subscribe_namespace failed: %d\n", (int)rc);
            app->need_retry = true;
            return 1;
        }
    }

    moq_event_t ev;
    while (moq_session_poll_events(sess, &ev, 1) == 1) {
        switch (ev.kind) {
        case MOQ_EVENT_NS_SUB_OK:
            if (moq_ns_sub_handle_eq(ev.u.ns_sub_ok.handle, app->ns_sub)) {
                app->ns_sub_ok = true;
                fprintf(stderr, "PTZ: Waiting for NAMESPACE...\n");
            }
            break;

        case MOQ_EVENT_NS_SUB_ERROR:
            if (moq_ns_sub_handle_eq(ev.u.ns_sub_error.handle, app->ns_sub)) {
                fprintf(stderr, "PTZ: namespace subscribe rejected (err=%u)\n",
                        (unsigned)ev.u.ns_sub_error.error_code);
                app->need_retry = true;
            }
            break;

        case MOQ_EVENT_NAMESPACE_FOUND:
            if (moq_ns_sub_handle_eq(ev.u.namespace_found.handle, app->ns_sub)) {
                log_ns_suffix("PTZ: namespace found suffix: ",
                              &ev.u.namespace_found.track_namespace_suffix);
                if (ns_suffix_is_exact_prefix(&ev.u.namespace_found.track_namespace_suffix)) {
                    app->control_seen = true;
                    if (app->dots_pending) { fprintf(stderr, "\n"); app->dots_pending = false; }
                    fprintf(stderr, "PTZ: target namespace discovered\n");
                }
            }
            break;

        case MOQ_EVENT_NAMESPACE_GONE:
            if (moq_ns_sub_handle_eq(ev.u.namespace_gone.handle, app->ns_sub)) {
                log_ns_suffix("PTZ: namespace gone suffix: ",
                              &ev.u.namespace_gone.track_namespace_suffix);
                if (ns_suffix_is_exact_prefix(&ev.u.namespace_gone.track_namespace_suffix)) {
                    fprintf(stderr, "PTZ: target namespace withdrawn, reconnecting...\n");
                    app->need_retry = true;
                }
            }
            break;

        case MOQ_EVENT_SESSION_CLOSED:
            fprintf(stderr, "PTZ: session closed (code=%llu), reconnecting...\n",
                    (unsigned long long)ev.u.closed.code);
            app->need_retry = true;
            break;

        case MOQ_EVENT_SUBSCRIBE_OK:
            if (moq_subscription_eq(ev.u.subscribe_ok.sub, app->sub)) {
                app->subscribed = true;
                fprintf(stderr, "PTZ: subscription ACTIVE — waiting for commands...\n");
            }
            break;

        case MOQ_EVENT_SUBSCRIBE_ERROR:
            if (moq_subscription_eq(ev.u.subscribe_error.sub, app->sub)) {
                fprintf(stderr, "PTZ: SUBSCRIBE rejected (err=%u), retrying...\n",
                        (unsigned)ev.u.subscribe_error.error_code);
                app->need_retry = true;
            }
            break;

        case MOQ_EVENT_SUBSCRIBE_DONE:
            if (moq_subscription_eq(ev.u.subscribe_done.sub, app->sub)) {
                fprintf(stderr, "PTZ: publisher ended track, reconnecting...\n");
                app->need_retry = true;
            }
            break;

        case MOQ_EVENT_UNSUBSCRIBED:
            if (moq_subscription_eq(ev.u.unsubscribed.sub, app->sub)) {
                fprintf(stderr, "PTZ: unsubscribed, reconnecting...\n");
                app->need_retry = true;
            }
            break;

        case MOQ_EVENT_OBJECT_RECEIVED:
            if (moq_subscription_eq(ev.u.object_received.sub, app->sub) &&
                ev.u.object_received.payload &&
                moq_rcbuf_len(ev.u.object_received.payload) > 0) {
                const uint8_t *data =
                    (const uint8_t *)moq_rcbuf_data(ev.u.object_received.payload);
                size_t len = moq_rcbuf_len(ev.u.object_received.payload);
                fprintf(stderr, "PTZ: object %zu bytes: %.*s\n",
                        len, (int)(len < 120 ? len : 120), data);
                dispatch(&app->q, app->cfg, data, len);
                app->n_commands++;
            }
            break;

        default:
            break;
        }

        moq_event_cleanup(&ev);
        if (app->need_retry) return 1;
    }

    if (app->control_seen && !moq_subscription_is_valid(app->sub)) {
        moq_subscription_t sub = MOQ_SUBSCRIPTION_INVALID;
        moq_result_t rc = subscribe_command_track(sess, app, now_us, &sub);
        if (rc == MOQ_OK) {
            app->sub = sub;
            app->subscribe_sent_us = now_us;
            fprintf(stderr, "PTZ: SUBSCRIBE sent for discovered namespace track '%s'\n",
                    app->track_name);
        } else if (rc != MOQ_ERR_WOULD_BLOCK && rc != MOQ_ERR_REQUEST_BLOCKED) {
            fprintf(stderr, "PTZ: moq_session_subscribe failed: %d\n", (int)rc);
            app->need_retry = true;
            return 1;
        }
    }

    if (moq_ns_sub_handle_is_valid(app->ns_sub) && !app->ns_sub_ok &&
        app->ns_subscribe_sent_us &&
        now_us - app->ns_subscribe_sent_us > 5000000ULL) {
        app->need_retry = true;
        return 1;
    }

    if (moq_subscription_is_valid(app->sub) && !app->subscribed &&
        app->subscribe_sent_us &&
        now_us - app->subscribe_sent_us > 5000000ULL) {
        app->need_retry = true;
        return 1;
    }

    if (moq_session_state(sess) == MOQ_SESS_CLOSED) {
        app->need_retry = true;
        return 1;
    }

    return 0;
}

/* ── Namespace split ─────────────────────────────────────────────────── */

static size_t split_ns(char *buf, moq_bytes_t *parts, size_t max)
{
    size_t n = 0; char *p = buf;
    while (*p && n < max) {
        char *sl = strchr(p, '/');
        if (sl) *sl = '\0';
        parts[n].data = (const uint8_t *)p; parts[n].len = strlen(p); n++;
        if (!sl) break; p = sl + 1;
    }
    return n;
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <url> <namespace> [track] [options]\n"
                "  url  moqt://host:port/path\n"
                "\nOptions:\n"
                "  --cam-ip <ip>          default 192.168.1.105\n"
                "  --cam-user <user>      default admin\n"
                "  --cam-pass <pass>\n"
                "  --cam-speed <1-8>      default 5\n"
                "  --max-duration-ms <N>  default 2000\n"
                "  --curl-path <path>     default curl\n"
                "  --insecure-skip-verify\n",
                argv[0]);
        return 2;
    }

    bool insecure = false;
    cam_cfg_t cam = { "CAMERA_IP", "admin", "", 5, 2000, "curl" };

    static char nsbuf[256];
    snprintf(nsbuf, sizeof(nsbuf), "%s", argv[2]);
    const char *track_name = "command";

    for (int i = 3; i < argc; i++) {
        if      (!strcmp(argv[i], "--insecure-skip-verify"))              insecure = true;
        else if (!strcmp(argv[i], "--cam-ip")          && i+1 < argc)    cam.cam_ip          = argv[++i];
        else if (!strcmp(argv[i], "--cam-user")        && i+1 < argc)    cam.cam_user        = argv[++i];
        else if (!strcmp(argv[i], "--cam-pass")        && i+1 < argc)    cam.cam_pass        = argv[++i];
        else if (!strcmp(argv[i], "--cam-speed")       && i+1 < argc)    cam.cam_speed       = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--max-duration-ms") && i+1 < argc)    cam.max_duration_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--curl-path")       && i+1 < argc)    cam.curl_path       = argv[++i];
        else                                                               track_name          = argv[i];
    }

    /* Parse URL */
    moq_url_t u; moq_url_init(&u);
    moq_bytes_t ub = { .data = (const uint8_t *)argv[1], .len = strlen(argv[1]) };
    if (moq_url_parse(ub, &u) != MOQ_OK) {
        fprintf(stderr, "bad URL: %s\n", argv[1]); return 1;
    }
    static char host_buf[256];
    size_t hl = u.host.len < sizeof(host_buf)-1 ? u.host.len : sizeof(host_buf)-1;
    memcpy(host_buf, u.host.data, hl); host_buf[hl] = '\0';

    app_ctx_t app; memset(&app, 0, sizeof(app));
    app.ns_count  = split_ns(nsbuf, app.ns_parts, 32);
    app.track_name = track_name;
    app.cfg       = &cam;
    q_init(&app.q);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* Start camera worker thread */
    worker_ctx_t wctx = { &app.q, &cam };
    pthread_t worker;
    pthread_create(&worker, NULL, worker_thread, &wctx);

    /* Create threaded MOQT connection */
    moq_pq_threaded_cfg_t tcfg;
    moq_pq_threaded_cfg_init_sized(&tcfg, sizeof(tcfg));
    tcfg.alloc                    = moq_alloc_default();
    tcfg.perspective              = MOQ_PERSPECTIVE_CLIENT;
    tcfg.host                     = host_buf;
    tcfg.port                     = (int)u.port;
    tcfg.insecure_skip_verify     = insecure;
    tcfg.send_request_capacity    = true;
    tcfg.initial_request_capacity = 64;
    tcfg.on_lane_pump             = on_pump;
    tcfg.on_lane_pump_ctx         = &app;

    fprintf(stderr, "PTZ control receiver started - v0.1.0\n");
    fprintf(stderr, "  relay:     %s:%d\n", host_buf, (int)u.port);
    fprintf(stderr, "  namespace: %s\n", argv[2]);
    fprintf(stderr, "  track:     %s\n", track_name);
    fprintf(stderr, "  camera:    http://%s (speed=%u, max_ms=%u)\n",
            cam.cam_ip, cam.cam_speed, cam.max_duration_ms);

    int retry_count = 0;
    int exit_code   = 0;

    for (;;) {
        moq_pq_threaded_t *t = NULL;
        moq_result_t rc = moq_pq_threaded_create(&tcfg, &t);
        if (rc != MOQ_OK) {
            fprintf(stderr, "moq_pq_threaded_create failed: %d\n", (int)rc);
            exit_code = 1;
            break;
        }
        g_threaded = t;

        /* Block until stopped, fatal, or pump_exit (on_pump returned non-zero).
           _wait returns MOQ_ERR_CLOSED on all three — pump_exit does NOT set
           is_fatal, so we must check the return value to avoid spinning. */
        while (!g_stop && !moq_pq_threaded_is_fatal(t)) {
            if (moq_pq_threaded_wait(t, 500000) == MOQ_ERR_CLOSED) break;
        }

        bool     fatal  = moq_pq_threaded_is_fatal(t);
        uint64_t fcode  = fatal ? moq_pq_threaded_fatal_code(t) : 0;
        /* Retry on our timeout/rejection OR on any fatal/disconnect — whether
           or not we were previously subscribed. Only user Ctrl-C skips retry. */
        bool  do_retry  = !g_stop && (app.need_retry || fatal);

        moq_pq_threaded_stop(t);
        moq_pq_threaded_destroy(t);
        g_threaded = NULL;

        if (do_retry) {
            /* Reset subscription state for the next attempt */
            app.ns_sub            = MOQ_NS_SUB_HANDLE_INVALID;
            app.sub               = MOQ_SUBSCRIPTION_INVALID;
            app.ns_sub_ok         = false;
            app.control_seen      = false;
            app.subscribed        = false;
            app.ns_subscribe_sent_us = 0;
            app.subscribe_sent_us = 0;
            app.need_retry        = false;

            if (retry_count == 0)
                fprintf(stderr, "PTZ: waiting for publisher ");
            //fprintf(stderr, ".");
            fflush(stderr);
            app.dots_pending = true;
            retry_count++;
            sleep_ms(1000);
            continue;
        }

        /* Clean newline after any dots before the final status line */
        if (app.dots_pending)
            fprintf(stderr, "\n");

        if (fatal)
            fprintf(stderr, "PTZ: fatal (code=%llu)\n", (unsigned long long)fcode);

        break;
    }

    q_stop(&app.q);
    pthread_join(worker, NULL);

    fprintf(stderr, "PTZ: %llu commands received\n", app.n_commands);
    return exit_code;
}

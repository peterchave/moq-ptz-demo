/*
 * media_send — Annex-B H.264 sender for ptz-remote.
 *
 * Reads Annex-B H.264 from stdin, splits into access units, and publishes
 * each access unit as one RAW/LOC media object.
 *
 * Usage:
 *   media_send <url> <namespace> [track] [options]
 *
 * Options:
 *   --insecure-skip-verify   disable TLS certificate verification
 *   --fps N                  frame rate metadata and PTS step (default 30)
 *   --width N                video width metadata (default 0)
 *   --height N               video height metadata (default 0)
 *   --framerate N            catalog framerate override (default fps)
 *   --bitrate N              max bitrate bits/s (default 1500000)
 */

#include <moq/endpoint.h>
#include <moq/media_sender.h>
#include <moq/rcbuf.h>

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;
enum { ENDPOINT_DRAIN_TIMEOUT_US = 5000000 };

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

static bool ensure_bytes(FILE *in, byte_vec_t *buf, size_t need)
{
    uint8_t tmp[4096];
    while (!g_stop && buf->len < need) {
        size_t n = fread(tmp, 1, sizeof(tmp), in);
        if (n == 0)
            return false;
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
                                  bool *is_keyframe)
{
    vec_reset(out_au);
    *is_keyframe = false;

    while (!g_stop) {
        if (!ensure_bytes(in, &r->pending, 6)) {
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

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <url> <namespace> [track] [options]\n"
                "  --insecure-skip-verify   disable TLS certificate verification\n"
                "  --fps N                  frame rate (default 30)\n"
                "  --width N                video width in pixels (default 0)\n"
                "  --height N               video height in pixels (default 0)\n"
                "  --framerate N            catalog framerate override (default fps)\n"
                "  --bitrate N              max bitrate bits/s (default 1500000)\n",
                argv[0]);
        return 2;
    }

    const char *url = argv[1];
    char nsbuf[256];
    snprintf(nsbuf, sizeof(nsbuf), "%s", argv[2]);
    moq_bytes_t ns_parts[32];
    size_t ns_count = split_namespace(nsbuf, ns_parts, 32);

    const char *track = "video";
    bool insecure_skip_verify = false;
    uint32_t fps = 30;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t framerate = 0;
    uint64_t bitrate = 1500000;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--insecure-skip-verify") == 0)
            insecure_skip_verify = true;
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
            fps = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
            width = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
            height = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--framerate") == 0 && i + 1 < argc)
            framerate = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc)
            bitrate = (uint64_t)strtoull(argv[++i], NULL, 10);
        else
            track = argv[i];
    }

    if (fps == 0)
        fps = 30;

    signal(SIGINT, on_signal);

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
    moq_media_sender_cfg_init_live(&scfg);
    scfg.endpoint = NULL;
    scfg.namespace_.parts = ns_parts;
    scfg.namespace_.count = ns_count;

    moq_media_sender_t *tx = NULL;
    rc = moq_media_sender_attach(ep, &scfg, &tx);
    if (rc != MOQ_OK) {
        fprintf(stderr, "sender attach failed: %d\n", (int)rc);
        moq_endpoint_stop(ep);
        moq_endpoint_destroy(ep);
        return 1;
    }

    moq_media_track_cfg_t tc;
    moq_media_track_cfg_init(&tc);
    tc.name.data = (const uint8_t *)track;
    tc.name.len = strlen(track);
    tc.media_type = MOQ_MEDIA_TYPE_VIDEO;
    tc.packaging = MOQ_MEDIA_PACKAGING_RAW;
    tc.codec.data = (const uint8_t *)"avc1.42e01e";
    tc.codec.len = 11;
    tc.bitrate = bitrate;
    tc.width = width;
    tc.height = height;
    tc.framerate_millis = (uint64_t)(framerate ? framerate : fps) * 1000u;
    tc.is_live = true;

    moq_media_track_t *trk = NULL;
    rc = moq_media_sender_add_track(tx, &tc, &trk);
    if (rc != MOQ_OK) {
        fprintf(stderr, "add_track failed: %d\n", (int)rc);
        moq_media_sender_destroy(tx);
        moq_endpoint_stop(ep);
        moq_endpoint_destroy(ep);
        return 1;
    }

    fprintf(stderr, "annex-b stdin ingest enabled (fps=%u)\n", fps);
    const uint64_t frame_duration_us = 1000000ull / fps;
    uint64_t frame_index = 0;
    unsigned long long sent = 0;
    bool started = false;

    h264_annexb_reader_t reader;
    h264_reader_init(&reader);
    byte_vec_t au = {0};

    while (!g_stop) {
        bool keyframe = false;
        if (!read_h264_access_unit(stdin, &reader, &au, &keyframe))
            break;
        if (au.len == 0)
            continue;

        if (!started) {
            if (!keyframe)
                continue;
            started = true;
            fprintf(stderr, "first keyframe detected; starting publish\n");
        }

        uint8_t *owned = (uint8_t *)malloc(au.len);
        if (!owned)
            break;
        memcpy(owned, au.data, au.len);

        moq_rcbuf_t *payload = NULL;
        if (moq_rcbuf_wrap(moq_alloc_default(), owned, au.len,
                           payload_release, NULL, &payload) != MOQ_OK) {
            free(owned);
            break;
        }

        moq_media_send_object_t o;
        memset(&o, 0, sizeof(o));
        o.struct_size = sizeof(o);
        o.payload = payload;
        o.is_sync = keyframe;
        o.starts_group = keyframe;
        o.presentation_time_us = frame_index * frame_duration_us;
        o.decode_time_us = frame_index * frame_duration_us;
        o.has_capture_time = true;
        o.capture_time_us = realtime_time_us();

        moq_result_t wr = moq_media_sender_write(tx, trk, &o);
        if (wr == MOQ_OK) {
            sent++;
            frame_index++;
        } else if (wr == MOQ_ERR_WOULD_BLOCK) {
            moq_rcbuf_decref(payload);
        } else if (wr == MOQ_ERR_INTERRUPTED || wr == MOQ_ERR_CLOSED) {
            moq_rcbuf_decref(payload);
            break;
        } else {
            moq_rcbuf_decref(payload);
            fprintf(stderr, "write failed: %d\n", (int)wr);
            break;
        }
    }

    vec_free(&au);
    h264_reader_free(&reader);

    fprintf(stderr, "wrote %llu objects\n", sent);

    moq_media_sender_destroy(tx);
    drain_before_stop(ep);
    moq_endpoint_stop(ep);
    moq_endpoint_destroy(ep);
    return 0;
}

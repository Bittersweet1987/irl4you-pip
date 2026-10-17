/* irl4you-encoder: Encoder für IRL4YOU.
 *
 * Eine Haupt-Pipeline (Mischen, Kodieren, Muxen) und je Kamera eine eigene Pipeline. Bilder und Ton gehen im Prozess von der
 * Kamera-Pipeline (appsink vout/aout) in die Haupt-Pipeline (appsrc camN_vin/camN_ain) und bekommen dabei Zeitstempel der Haupt-Uhr.
 * Fällt eine Kamera aus, wird nur ihre Pipeline neu gestartet. Den MPEG-TS-Ausgang (appsink out) sendet ein eigener Faden per SRT.
 * Die Bitrate regelt ratectl.c anhand der SRT-Messwerte. Entwurf: ../DESIGN.md.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib-unix.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <json-glib/json-glib.h>
#include <srt/srt.h>

#include "ratectl.h"

#define VERSION "0.2.0"
#define IFACE_VERSION 1
#define MAX_CAMS 8
#define TS_PACKET 188
#define SRT_CHUNK (7 * TS_PACKET)
#define CAM_SILENT_MS 3000
#define CAM_RESTART_MS 10000
#define CAM_RETRY_MS 3000
#define OUT_STALL_MS 5000
#define SRT_GIVE_UP_MS 30000
#define RC_TICK_MS 250
#define QUEUE_MAX_CHUNKS 4000   /* ca. 5 MB, bei 20 Mbit/s etwa 2 s */

enum { EXIT_OK = 0, EXIT_USAGE = 2, EXIT_STALL = 3, EXIT_SRT = 4, EXIT_PIPELINE = 5 };

typedef struct {
    int id;
    char *desc;             /* gst-launch-Beschreibung oder NULL = abwesend */
    gboolean idle;          /* #idle: startet gestoppt, erst ein Steuerbefehl startet sie */
    GstElement *pipe;
    GstAppSrc *vin, *ain;   /* in der Haupt-Pipeline */
    gboolean want_run;      /* soll laufen (Steuerbefehl) */
    gboolean running;       /* Pipeline gestartet */
    gint64 last_v_us, last_a_us, started_us;
    guint frames, fps_frames;
    double fps;
    int offset_ms;
    int restarts;
    int fails;              /* Fehlversuche seit dem letzten Bild (für weniger Meldungen) */
    guint retry_src;
    GstClockTime v_last_in_pts, v_last_out_pts;
    gint64 lag_ms;
} cam_t;

typedef struct {
    /* Einstellungen */
    char *pipeline_file, *srt_host, *streamid, *bitrate_file, *control_path, *stats_path;
    int srt_port, latency_ms, align_ms;
    /* Pipelines */
    GstElement *main;
    GstElement *venc;
    gboolean venc_bps;       /* Eigenschaft "bps" (bit/s) statt "bitrate" (kbit/s) */
    cam_t cams[MAX_CAMS];
    GMainLoop *loop;
    int exit_code;
    /* Ausgang */
    GAsyncQueue *outq;
    GThread *sender;
    volatile gint sender_stop;
    volatile gint64 last_out_us;
    volatile gint64 srt_ok_us;
    volatile SRTSOCKET sock;
    GByteArray *pending;
    gint64 out_bytes;
    /* Regelung */
    ratectl_t rc;
    int applied_kbps;
    gint64 bitrate_sig;      /* Änderungszeit (ns) und Größe der Bitraten-Datei */
    /* Statistik */
    GQueue *events;          /* "Unix-Zeit\tText" */
    gint64 start_us;
    volatile gint venc_frames;
    gint fps_last_frames;
    double fps;
    double rtt_ms, sndbuf_ms, send_mbps;
    gint64 loss_total, sent_total, resent_total, drop_total, sndbuf_pkts;
    int ctl_fd, ctl_dummy;
    GHashTable *props;       /* "element\x1feigenschaft" -> Wert: wird nach einem Kamera-Neustart wieder gesetzt */
} app_t;

static app_t A;

static gint64 now_us(void) { return g_get_monotonic_time(); }

static void event(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void event(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    fprintf(stderr, "irl4you-encoder: %s\n", msg);
    g_queue_push_tail(A.events, g_strdup_printf("%" G_GINT64_FORMAT "\t%s", g_get_real_time() / 1000000, msg));
    g_free(msg);
    while (g_queue_get_length(A.events) > 20)
        g_free(g_queue_pop_head(A.events));
}

static void quit(int code)
{
    if (A.exit_code == EXIT_OK)
        A.exit_code = code;
    g_main_loop_quit(A.loop);
}

/* ------------------------------------------------------------------ Pipeline-Datei */

static gboolean load_pipeline_file(const char *path, char **main_desc)
{
    gchar *text = NULL;
    GError *err = NULL;
    if (!g_file_get_contents(path, &text, NULL, &err)) {
        fprintf(stderr, "irl4you-encoder: Pipeline-Datei %s: %s\n", path, err->message);
        g_error_free(err);
        return FALSE;
    }
    GString *cur = NULL, *mainbuf = NULL;
    int cur_cam = -1;
    gboolean off = FALSE, idle = FALSE;
    gchar **lines = g_strsplit(text, "\n", -1);
    /* Ohne Abschnittskopf ist die ganze Datei [main] */
    gboolean headed = FALSE;
    for (gchar **l = lines; *l; l++) {
        gchar *t = g_strstrip(g_strdup(*l));
        if (t[0] == '[' && strchr(t, ']'))
            headed = TRUE;
        g_free(t);
    }
    if (!headed)
        cur = g_string_new(NULL);
    for (gchar **l = lines;; l++) {
        gboolean end = (*l == NULL);
        gchar *s = end ? NULL : g_strstrip(*l);
        if (end || (s[0] == '[' && strchr(s, ']'))) {
            if (cur) { /* Abschnitt abschließen */
                if (cur_cam < 0)
                    mainbuf = cur;
                else {
                    A.cams[cur_cam].desc = off || cur->len == 0 ? NULL : g_strdup(cur->str);
                    A.cams[cur_cam].idle = idle;
                    g_string_free(cur, TRUE);
                }
                cur = NULL;
            }
            if (end)
                break;
            off = idle = FALSE;
            int n;
            if (g_str_has_prefix(s, "[main]"))
                cur_cam = -1;
            else if (sscanf(s, "[cam %d]", &n) == 1 && n >= 0 && n < MAX_CAMS)
                cur_cam = n;
            else {
                fprintf(stderr, "irl4you-encoder: unbekannter Abschnitt %s\n", s);
                g_strfreev(lines);
                g_free(text);
                return FALSE;
            }
            cur = g_string_new(NULL);
            continue;
        }
        if (!cur || s[0] == '\0')
            continue;
        if (s[0] == '#') {
            if (!strcmp(s, "#off"))
                off = TRUE;
            else if (!strcmp(s, "#idle"))
                idle = TRUE;
            continue;
        }
        g_string_append(cur, s);
        g_string_append_c(cur, '\n');
    }
    g_strfreev(lines);
    g_free(text);
    if (!mainbuf || mainbuf->len == 0) {
        fprintf(stderr, "irl4you-encoder: Abschnitt [main] fehlt\n");
        if (mainbuf) g_string_free(mainbuf, TRUE);
        return FALSE;
    }
    *main_desc = g_string_free(mainbuf, FALSE);
    return TRUE;
}

/* ------------------------------------------------------------------ Kamera -> Haupt-Pipeline */

static GstClockTime main_running_time(void)
{
    GstClock *clk = gst_element_get_clock(A.main);
    if (!clk)
        return GST_CLOCK_TIME_NONE;
    GstClockTime t = gst_clock_get_time(clk) - gst_element_get_base_time(A.main);
    gst_object_unref(clk);
    return t;
}

static GstFlowReturn forward(GstAppSink *sink, cam_t *c, GstAppSrc *dst, gboolean video)
{
    GstSample *smp = gst_app_sink_pull_sample(sink);
    if (!smp)
        return GST_FLOW_EOS;
    if (!dst) {
        gst_sample_unref(smp);
        return GST_FLOW_OK;
    }
    GstCaps *caps = gst_sample_get_caps(smp);
    GstCaps *cur = gst_app_src_get_caps(dst);
    if (caps && (!cur || !gst_caps_is_equal(cur, caps)))
        gst_app_src_set_caps(dst, caps);
    if (cur)
        gst_caps_unref(cur);

    GstClockTime rt = main_running_time();
    if (!GST_CLOCK_TIME_IS_VALID(rt)) {
        gst_sample_unref(smp);
        return GST_FLOW_OK;
    }
    GstBuffer *in = gst_sample_get_buffer(smp);
    GstBuffer *b = gst_buffer_copy(in); /* teilt den Speicher (DMA-Puffer), nur die Metadaten sind neu */
    GstClockTime pts = rt + (GstClockTime)(A.align_ms + c->offset_ms) * GST_MSECOND;
    GST_BUFFER_PTS(b) = pts;
    GST_BUFFER_DTS(b) = GST_CLOCK_TIME_NONE;
    gint64 t = now_us();
    if (video) {
        c->last_v_us = t;
        if (c->frames++ == 0)
            c->fails = 0;
        g_atomic_int_inc((gint *)&c->fps_frames);
        if (GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(in)) && GST_CLOCK_TIME_IS_VALID(c->v_last_in_pts) &&
            GST_CLOCK_TIME_IS_VALID(c->v_last_out_pts)) {
            /* Abweichung zwischen Takt der Kamera und Takt der Box: zeigt, ob die Kamera nachhängt */
            gint64 din = (gint64)GST_BUFFER_PTS(in) - (gint64)c->v_last_in_pts;
            gint64 dout = (gint64)pts - (gint64)c->v_last_out_pts;
            c->lag_ms += (dout - din) / (gint64)GST_MSECOND;
        }
        c->v_last_in_pts = GST_BUFFER_PTS(in);
        c->v_last_out_pts = pts;
    } else {
        c->last_a_us = t;
    }
    gst_sample_unref(smp);
    gst_app_src_push_buffer(dst, b);
    return GST_FLOW_OK;
}

static GstFlowReturn on_cam_video(GstAppSink *s, gpointer u) { cam_t *c = u; return forward(s, c, c->vin, TRUE); }
static GstFlowReturn on_cam_audio(GstAppSink *s, gpointer u) { cam_t *c = u; return forward(s, c, c->ain, FALSE); }

static void cam_stop(cam_t *c);
static gboolean cam_start(cam_t *c);

/* Nach dem (Neu-)Start einer Kamera die zuletzt gesetzten Eigenschaften ihrer Elemente wieder anwenden */
static void reapply_props(GstElement *pipe)
{
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, A.props);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        gchar **kv = g_strsplit(k, "\x1f", 2);
        GstElement *e = kv[0] && kv[1] ? gst_bin_get_by_name(GST_BIN(pipe), kv[0]) : NULL;
        if (e) {
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(e), kv[1]))
                gst_util_set_object_arg(G_OBJECT(e), kv[1], v);
            gst_object_unref(e);
        }
        g_strfreev(kv);
    }
}

static gboolean cam_retry(gpointer u)
{
    cam_t *c = u;
    c->retry_src = 0;
    if (c->want_run && !c->running)
        cam_start(c);
    return G_SOURCE_REMOVE;
}

static void cam_schedule_retry(cam_t *c, guint ms)
{
    if (!c->retry_src)
        c->retry_src = g_timeout_add(ms, cam_retry, c);
}

static gboolean on_cam_bus(GstBus *bus, GstMessage *m, gpointer u)
{
    (void)bus;
    cam_t *c = u;
    if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS) {
        gboolean had_frames = c->frames > 0;
        if (had_frames || c->fails % 20 == 0) {
            if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
                GError *e = NULL;
                gst_message_parse_error(m, &e, NULL);
                event("kamera %d: %s (%s)", c->id, e ? e->message : "Fehler", GST_OBJECT_NAME(GST_MESSAGE_SRC(m)));
                if (e) g_error_free(e);
            } else {
                event("kamera %d: Strom beendet", c->id);
            }
        }
        c->fails = had_frames ? 1 : c->fails + 1;
        cam_stop(c);
        if (c->want_run)
            cam_schedule_retry(c, CAM_RETRY_MS);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean cam_start(cam_t *c)
{
    if (c->running || !c->desc)
        return c->running;
    GError *err = NULL;
    GstElement *p = gst_parse_launch_full(c->desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    if (!p) {
        event("kamera %d: Pipeline ungültig: %s", c->id, err ? err->message : "?");
        if (err) g_error_free(err);
        return FALSE;
    }
    GstAppSinkCallbacks vcb = {.new_sample = on_cam_video}, acb = {.new_sample = on_cam_audio};
    GstElement *vout = gst_bin_get_by_name(GST_BIN(p), "vout"), *aout = gst_bin_get_by_name(GST_BIN(p), "aout");
    if (vout) {
        g_object_set(vout, "sync", FALSE, "max-buffers", 4, "drop", TRUE, NULL);
        gst_app_sink_set_callbacks(GST_APP_SINK(vout), &vcb, c, NULL);
        gst_object_unref(vout);
    }
    if (aout) {
        g_object_set(aout, "sync", FALSE, "max-buffers", 50, "drop", TRUE, NULL);
        gst_app_sink_set_callbacks(GST_APP_SINK(aout), &acb, c, NULL);
        gst_object_unref(aout);
    }
    reapply_props(p);
    GstBus *bus = gst_element_get_bus(p);
    gst_bus_add_watch(bus, on_cam_bus, c);
    gst_object_unref(bus);
    c->pipe = p;
    c->running = TRUE;
    c->started_us = c->last_v_us = c->last_a_us = now_us();
    c->v_last_in_pts = c->v_last_out_pts = GST_CLOCK_TIME_NONE;
    c->lag_ms = 0;
    c->frames = 0;
    if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (c->fails % 20 == 0) /* nicht alle 3 s dieselbe Meldung */
            event("kamera %d: startet nicht%s", c->id, c->fails ? " (weiter)" : "");
        c->fails++;
        cam_stop(c);
        cam_schedule_retry(c, CAM_RETRY_MS);
        return FALSE;
    }
    return TRUE;
}

static void cam_stop(cam_t *c)
{
    if (!c->pipe)
        return;
    GstElement *p = c->pipe;
    c->pipe = NULL;
    c->running = FALSE;
    GstBus *bus = gst_element_get_bus(p);
    gst_bus_remove_watch(bus);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
}

static void cam_restart(cam_t *c, const char *why)
{
    event("kamera %d: %s, wird neu gestartet", c->id, why);
    c->restarts++;
    cam_stop(c);
    cam_start(c);
}

/* ------------------------------------------------------------------ Ausgang per SRT */

static GstFlowReturn on_out(GstAppSink *sink, gpointer u)
{
    (void)u;
    GstSample *smp = gst_app_sink_pull_sample(sink);
    if (!smp)
        return GST_FLOW_EOS;
    GstBuffer *b = gst_sample_get_buffer(smp);
    GstMapInfo mi;
    if (gst_buffer_map(b, &mi, GST_MAP_READ)) {
        g_byte_array_append(A.pending, mi.data, mi.size);
        A.out_bytes += mi.size;
        gst_buffer_unmap(b, &mi);
    }
    gst_sample_unref(smp);
    A.last_out_us = now_us();
    while (A.pending->len >= SRT_CHUNK) {
        GBytes *chunk = g_bytes_new(A.pending->data, SRT_CHUNK);
        g_byte_array_remove_range(A.pending, 0, SRT_CHUNK);
        if (g_async_queue_length(A.outq) > QUEUE_MAX_CHUNKS)
            g_bytes_unref(g_async_queue_try_pop(A.outq)); /* ältestes verwerfen statt zu stauen */
        g_async_queue_push(A.outq, chunk);
    }
    return GST_FLOW_OK;
}

static SRTSOCKET srt_open(void)
{
    SRTSOCKET s = srt_create_socket();
    if (s == SRT_INVALID_SOCK)
        return s;
    int lat = A.latency_ms, yes = 1, tt = SRTT_LIVE, pl = SRT_CHUNK, conto = 3000;
    srt_setsockflag(s, SRTO_TRANSTYPE, &tt, sizeof tt);
    srt_setsockflag(s, SRTO_LATENCY, &lat, sizeof lat);
    srt_setsockflag(s, SRTO_PAYLOADSIZE, &pl, sizeof pl);
    srt_setsockflag(s, SRTO_SENDER, &yes, sizeof yes);
    srt_setsockflag(s, SRTO_CONNTIMEO, &conto, sizeof conto);
    if (A.streamid && *A.streamid)
        srt_setsockflag(s, SRTO_STREAMID, A.streamid, (int)strlen(A.streamid));
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *ai = NULL;
    char port[16];
    snprintf(port, sizeof port, "%d", A.srt_port);
    if (getaddrinfo(A.srt_host, port, &hints, &ai) != 0 || !ai) {
        srt_close(s);
        return SRT_INVALID_SOCK;
    }
    int r = srt_connect(s, ai->ai_addr, (int)ai->ai_addrlen);
    freeaddrinfo(ai);
    if (r == SRT_ERROR) {
        srt_close(s);
        return SRT_INVALID_SOCK;
    }
    return s;
}

static gpointer sender_thread(gpointer u)
{
    (void)u;
    gint64 fail_since = 0;
    while (!g_atomic_int_get(&A.sender_stop)) {
        if (A.sock == SRT_INVALID_SOCK) {
            SRTSOCKET s = srt_open();
            if (s == SRT_INVALID_SOCK) {
                if (!fail_since)
                    fail_since = now_us();
                g_usleep(500000);
                /* während keiner Verbindung nicht unbegrenzt puffern */
                while (g_async_queue_length(A.outq) > 200)
                    g_bytes_unref(g_async_queue_try_pop(A.outq));
                continue;
            }
            A.sock = s;
            fail_since = 0;
            A.srt_ok_us = now_us();
        }
        GBytes *chunk = g_async_queue_timeout_pop(A.outq, 200000);
        if (!chunk)
            continue;
        gsize n;
        const char *d = g_bytes_get_data(chunk, &n);
        if (srt_sendmsg2(A.sock, d, (int)n, NULL) == SRT_ERROR) {
            srt_close(A.sock);
            A.sock = SRT_INVALID_SOCK;
        } else {
            A.srt_ok_us = now_us();
        }
        g_bytes_unref(chunk);
    }
    return NULL;
}

/* ------------------------------------------------------------------ Bitrate */

static void read_bitrate_file(gboolean force)
{
    if (!A.bitrate_file)
        return;
    struct stat st;
    if (stat(A.bitrate_file, &st) != 0)
        return;
    gint64 sig = ((gint64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec) ^ ((gint64)st.st_size * 1000003);
    if (!force && sig == A.bitrate_sig)
        return;
    A.bitrate_sig = sig;
    FILE *f = fopen(A.bitrate_file, "r");
    if (!f)
        return;
    int lo = 0, hi = 0;
    if (fscanf(f, "%d %d", &lo, &hi) == 2 && lo > 0 && hi > 0) {
        ratectl_set_limits(&A.rc, lo, hi);
        event("regler: Bitrate %d bis %d kbit/s", A.rc.min_kbps, A.rc.max_kbps);
    }
    fclose(f);
}

static void apply_bitrate(int kbps)
{
    if (!A.venc || kbps == A.applied_kbps)
        return;
    if (A.venc_bps)
        g_object_set(A.venc, "bps", (guint)(kbps * 1000), NULL);
    else
        g_object_set(A.venc, "bitrate", (guint)kbps, NULL);
    A.applied_kbps = kbps;
}

static gboolean on_rc_tick(gpointer u)
{
    (void)u;
    read_bitrate_file(FALSE);
    SRT_TRACEBSTATS st;
    rc_sample_t s = {.now_ms = now_us() / 1000};
    if (A.sock != SRT_INVALID_SOCK && srt_bstats(A.sock, &st, 1) == 0) {
        A.rtt_ms = st.msRTT;
        A.sndbuf_ms = st.msSndBuf;
        A.send_mbps = st.mbpsSendRate;
        A.loss_total = st.pktSndLossTotal;
        A.sent_total = st.pktSentTotal;
        A.resent_total = st.pktRetransTotal;
        A.drop_total = st.pktSndDropTotal;
        A.sndbuf_pkts = st.pktSndBuf;
        s.rtt_ms = st.msRTT;
        s.sndbuf_ms = st.msSndBuf;
    }
    apply_bitrate(ratectl_update(&A.rc, &s));
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ Wächter und Statistik */

static int cam_state(cam_t *c, gint64 t)
{
    if (!c->running && !c->want_run)
        return 1; /* gestoppt */
    if (!c->running)
        return 2; /* soll laufen, wartet auf neuen Versuch */
    return c->frames > 0 && t - c->last_v_us < (gint64)CAM_SILENT_MS * 1000 ? 3 : 2;
}

static gboolean on_watch(gpointer u)
{
    (void)u;
    static int srt_was = -1;
    gint64 t = now_us();
    int srt_now = A.sock != SRT_INVALID_SOCK;
    if (srt_now != srt_was) {
        if (srt_now)
            event("srt: verbunden mit %s:%d", A.srt_host, A.srt_port);
        else if (srt_was == 1)
            event("srt: Verbindung zu %s:%d getrennt, neuer Versuch", A.srt_host, A.srt_port);
        srt_was = srt_now;
    }
    gint f = g_atomic_int_get(&A.venc_frames);
    A.fps = f - A.fps_last_frames;
    A.fps_last_frames = f;
    if (A.last_out_us && t - A.last_out_us > (gint64)OUT_STALL_MS * 1000) {
        event("pipeline: Ausgang steht seit %d s", OUT_STALL_MS / 1000);
        quit(EXIT_STALL);
        return G_SOURCE_REMOVE;
    }
    if (A.srt_ok_us && t - A.srt_ok_us > (gint64)SRT_GIVE_UP_MS * 1000) {
        event("srt: Ziel %s:%d seit %d s nicht erreichbar", A.srt_host, A.srt_port, SRT_GIVE_UP_MS / 1000);
        quit(EXIT_SRT);
        return G_SOURCE_REMOVE;
    }
    for (int i = 0; i < MAX_CAMS; i++) {
        cam_t *c = &A.cams[i];
        guint n = g_atomic_int_get((gint *)&c->fps_frames);
        g_atomic_int_add((gint *)&c->fps_frames, -(gint)n);
        c->fps = n;
        if (c->running && c->vin && t - c->last_v_us > (gint64)CAM_RESTART_MS * 1000 && t - c->started_us > (gint64)CAM_RESTART_MS * 1000)
            cam_restart(c, "liefert kein Bild");
    }
    return G_SOURCE_CONTINUE;
}

#define JINT(b, k, v) do { json_builder_set_member_name(b, k); json_builder_add_int_value(b, (gint64)(v)); } while (0)
#define JBOOL(b, k, v) do { json_builder_set_member_name(b, k); json_builder_add_boolean_value(b, (v)); } while (0)
#define JDBL(b, k, v) do { json_builder_set_member_name(b, k); json_builder_add_double_value(b, (v)); } while (0)
#define JSTR(b, k, v) do { json_builder_set_member_name(b, k); json_builder_add_string_value(b, (v)); } while (0)

static void write_stats(void)
{
    if (!A.stats_path)
        return;
    gint64 t = now_us();
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    JINT(b, "v", IFACE_VERSION);
    JINT(b, "t", g_get_real_time() / 1000000);
    JINT(b, "up_s", (t - A.start_us) / 1000000);
    JDBL(b, "fps", A.fps);
    JINT(b, "frames", g_atomic_int_get(&A.venc_frames));

    json_builder_set_member_name(b, "rate");
    json_builder_begin_object(b);
    JINT(b, "kbps", A.rc.target_kbps);
    JINT(b, "min", A.rc.min_kbps);
    JINT(b, "max", A.rc.max_kbps);
    JSTR(b, "state", ratectl_state_name(A.rc.state));
    json_builder_end_object(b);

    json_builder_set_member_name(b, "srt");
    json_builder_begin_object(b);
    JBOOL(b, "connected", A.sock != SRT_INVALID_SOCK);
    JINT(b, "rtt_ms", A.rtt_ms);
    JINT(b, "rtt_base_ms", A.rc.rtt_base_ms);
    JINT(b, "send_kbps", A.send_mbps * 1000);
    JINT(b, "buf_pkts", A.sndbuf_pkts);
    JINT(b, "buf_ms", A.sndbuf_ms);
    JINT(b, "sent", A.sent_total);
    JINT(b, "lost", A.loss_total);
    JINT(b, "resent", A.resent_total);
    JINT(b, "dropped", A.drop_total);
    json_builder_end_object(b);

    json_builder_set_member_name(b, "out");
    json_builder_begin_object(b);
    JINT(b, "age_ms", A.last_out_us ? (t - A.last_out_us) / 1000 : -1);
    JINT(b, "bytes", A.out_bytes);
    json_builder_end_object(b);

    json_builder_set_member_name(b, "cams");
    json_builder_begin_object(b);
    for (int i = 0; i < MAX_CAMS; i++) {
        cam_t *c = &A.cams[i];
        if (!c->desc && !c->vin)
            continue;
        char key[4];
        snprintf(key, sizeof key, "%d", i);
        json_builder_set_member_name(b, key);
        json_builder_begin_object(b);
        int st = cam_state(c, t);
        JINT(b, "state", st);
        JBOOL(b, "live", st == 3);
        JDBL(b, "fps", st == 3 ? c->fps : 0.0);
        JINT(b, "lag_ms", c->lag_ms);
        JINT(b, "offset_ms", c->offset_ms);
        JINT(b, "restarts", c->restarts);
        JBOOL(b, "audio", st == 3 && t - c->last_a_us < (gint64)CAM_SILENT_MS * 1000);
        json_builder_end_object(b);
    }
    json_builder_end_object(b);

    json_builder_set_member_name(b, "events");
    json_builder_begin_array(b);
    for (GList *l = A.events->head; l; l = l->next) {
        const char *e = l->data, *tab = strchr(e, '\t');
        json_builder_begin_object(b);
        JINT(b, "t", g_ascii_strtoll(e, NULL, 10));
        JSTR(b, "text", tab ? tab + 1 : e);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    gchar *txt = json_generator_to_data(g, NULL);
    gchar *tmp = g_strconcat(A.stats_path, ".tmp", NULL);
    if (g_file_set_contents(tmp, txt, -1, NULL)) {
        chmod(tmp, 0644);
        rename(tmp, A.stats_path);
    }
    g_free(tmp);
    g_free(txt);
    json_node_unref(root);
    g_object_unref(g);
    g_object_unref(b);
}

static GstPadProbeReturn on_venc_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    (void)pad; (void)info; (void)u;
    g_atomic_int_inc(&A.venc_frames);
    return GST_PAD_PROBE_OK;
}

static gboolean on_stats(gpointer u) { (void)u; write_stats(); return G_SOURCE_CONTINUE; }

/* ------------------------------------------------------------------ Steuerbefehle */

static GstElement *find_element(const char *name)
{
    GstElement *e = gst_bin_get_by_name(GST_BIN(A.main), name);
    for (int i = 0; !e && i < MAX_CAMS; i++)
        if (A.cams[i].pipe)
            e = gst_bin_get_by_name(GST_BIN(A.cams[i].pipe), name);
    return e;
}

static gchar *node_to_string(JsonNode *n)
{
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE)
        return NULL;
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_STRING) return g_strdup(json_node_get_string(n));
    if (t == G_TYPE_BOOLEAN) return g_strdup(json_node_get_boolean(n) ? "true" : "false");
    if (t == G_TYPE_INT64) return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(n));
    if (t == G_TYPE_DOUBLE) {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        return g_strdup(g_ascii_dtostr(buf, sizeof buf, json_node_get_double(n)));
    }
    return NULL;
}

static gboolean run_cmd(JsonObject *o);

static gboolean set_on(GObject *obj, const char *prop, JsonNode *val, const char *what)
{
    gchar *s = node_to_string(val);
    if (!s || !g_object_class_find_property(G_OBJECT_GET_CLASS(obj), prop)) {
        event("befehl: %s hat keine Eigenschaft %s", what, prop);
        g_free(s);
        return FALSE;
    }
    gst_util_set_object_arg(obj, prop, s);
    g_free(s);
    return TRUE;
}

static gboolean run_cmd(JsonObject *o)
{
    const char *op = json_object_get_string_member_with_default(o, "op", "");
    if (!strcmp(op, "batch")) {
        JsonArray *a = json_object_get_array_member(o, "cmds");
        gboolean ok = TRUE;
        for (guint i = 0; a && i < json_array_get_length(a); i++) {
            JsonNode *n = json_array_get_element(a, i);
            if (JSON_NODE_HOLDS_OBJECT(n))
                ok = run_cmd(json_node_get_object(n)) && ok;
        }
        return ok;
    }
    if (!strcmp(op, "prop") || !strcmp(op, "pad")) {
        const char *el = json_object_get_string_member_with_default(o, "el", "");
        const char *name = json_object_get_string_member_with_default(o, "name", "");
        GstElement *e = find_element(el);
        if (!e && !strcmp(op, "prop") && g_str_has_prefix(el, "cam")) {
            /* Kamera gerade aus: merken, beim nächsten Start wird es gesetzt */
            gchar *val = node_to_string(json_object_get_member(o, "value"));
            if (val)
                g_hash_table_replace(A.props, g_strdup_printf("%s\x1f%s", el, name), val);
            return val != NULL;
        }
        if (!e) {
            event("befehl: Element %s nicht gefunden", el);
            return FALSE;
        }
        gboolean ok;
        if (!strcmp(op, "prop")) {
            ok = set_on(G_OBJECT(e), name, json_object_get_member(o, "value"), el);
            gchar *val = ok ? node_to_string(json_object_get_member(o, "value")) : NULL;
            if (val)
                g_hash_table_replace(A.props, g_strdup_printf("%s\x1f%s", el, name), val);
        } else {
            const char *padn = json_object_get_string_member_with_default(o, "pad", "");
            GstPad *p = gst_element_get_static_pad(e, padn);
            ok = p && set_on(G_OBJECT(p), name, json_object_get_member(o, "value"), padn);
            if (!p) event("befehl: Pad %s.%s nicht gefunden", el, padn);
            if (p) gst_object_unref(p);
        }
        gst_object_unref(e);
        return ok;
    }
    if (!strcmp(op, "audio")) {
        GstElement *sel = find_element(json_object_get_string_member_with_default(o, "el", "audiosel"));
        if (!sel) { event("befehl: Ton-Umschalter fehlt"); return FALSE; }
        GstPad *p = gst_element_get_static_pad(sel, json_object_get_string_member_with_default(o, "pad", ""));
        if (p) { g_object_set(sel, "active-pad", p, NULL); gst_object_unref(p); }
        else event("befehl: Ton-Pad nicht gefunden");
        gst_object_unref(sel);
        return p != NULL;
    }
    if (!strcmp(op, "cam") || !strcmp(op, "offset")) {
        gint64 id = json_object_get_int_member_with_default(o, "id", -1);
        if (id < 0 || id >= MAX_CAMS) { event("befehl: Kamera %" G_GINT64_FORMAT " gibt es nicht", id); return FALSE; }
        cam_t *c = &A.cams[id];
        if (!strcmp(op, "offset")) {
            c->offset_ms = (int)CLAMP(json_object_get_int_member_with_default(o, "ms", 0), 0, 10000);
            return TRUE;
        }
        gboolean run = json_object_get_boolean_member_with_default(o, "run", TRUE);
        if (json_object_has_member(o, "pipeline")) { /* neue Beschreibung (z. B. andere Quelle) */
            g_free(c->desc);
            c->desc = g_strdup(json_object_get_string_member(o, "pipeline"));
            cam_stop(c);
        }
        c->want_run = run;
        if (run) cam_start(c); else cam_stop(c);
        return TRUE;
    }
    event("befehl: unbekannt (%s)", op);
    return FALSE;
}

static gboolean on_control(gint fd, GIOCondition cond, gpointer u)
{
    (void)cond; (void)u;
    static GString *buf;
    if (!buf) buf = g_string_new(NULL);
    char tmp[4096];
    ssize_t n;
    while ((n = read(fd, tmp, sizeof tmp)) > 0)
        g_string_append_len(buf, tmp, n);
    char *nl;
    while ((nl = memchr(buf->str, '\n', buf->len))) {
        *nl = '\0';
        if (buf->str[0]) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, buf->str, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p)))
                run_cmd(json_node_get_object(json_parser_get_root(p)));
            else
                event("befehl: kein gültiges JSON");
            g_object_unref(p);
        }
        g_string_erase(buf, 0, nl - buf->str + 1);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ Haupt-Pipeline */

static gboolean on_main_bus(GstBus *bus, GstMessage *m, gpointer u)
{
    (void)bus; (void)u;
    switch (GST_MESSAGE_TYPE(m)) {
    case GST_MESSAGE_ERROR: {
        GError *e = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(m, &e, &dbg);
        event("pipeline: Fehler in %s: %s", GST_OBJECT_NAME(GST_MESSAGE_SRC(m)), e ? e->message : "?");
        if (dbg) fprintf(stderr, "  %s\n", dbg);
        g_clear_error(&e);
        g_free(dbg);
        quit(EXIT_PIPELINE);
        break;
    }
    case GST_MESSAGE_EOS:
        event("pipeline: beendet (EOS)");
        quit(EXIT_PIPELINE);
        break;
    default:
        break;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_signal(gpointer u) { (void)u; event("ende: Signal erhalten"); quit(EXIT_OK); return G_SOURCE_REMOVE; }
static gboolean on_hup(gpointer u) { (void)u; read_bitrate_file(TRUE); return G_SOURCE_CONTINUE; }

static void usage(void)
{
    fprintf(stderr,
            "Aufruf: irl4you-encoder --pipeline DATEI --srt HOST:PORT [--latency MS] [--streamid ID]\n"
            "        [--bitrate-file DATEI] [--control FIFO] [--stats DATEI] [--align MS] [--start-kbps N]\n");
}

int main(int argc, char **argv)
{
    A.sock = SRT_INVALID_SOCK;
    A.latency_ms = 2000;
    A.ctl_fd = A.ctl_dummy = -1;
    int start_kbps = 0;
    gboolean version = FALSE;
    gchar *srt = NULL;
    GOptionEntry opts[] = {
        {"pipeline", 0, 0, G_OPTION_ARG_FILENAME, &A.pipeline_file, "Pipeline-Datei", "DATEI"},
        {"srt", 0, 0, G_OPTION_ARG_STRING, &srt, "SRT-Ziel", "HOST:PORT"},
        {"latency", 0, 0, G_OPTION_ARG_INT, &A.latency_ms, "SRT-Latenz in ms", "MS"},
        {"streamid", 0, 0, G_OPTION_ARG_STRING, &A.streamid, "SRT-Stream-ID", "ID"},
        {"bitrate-file", 0, 0, G_OPTION_ARG_FILENAME, &A.bitrate_file, "Datei mit 'min max' in kbit/s", "DATEI"},
        {"control", 0, 0, G_OPTION_ARG_FILENAME, &A.control_path, "FIFO für Steuerbefehle", "FIFO"},
        {"stats", 0, 0, G_OPTION_ARG_FILENAME, &A.stats_path, "Statistik-Datei (JSON)", "DATEI"},
        {"align", 0, 0, G_OPTION_ARG_INT, &A.align_ms, "Ausrichtungspuffer in ms", "MS"},
        {"start-kbps", 0, 0, G_OPTION_ARG_INT, &start_kbps, "Startbitrate in kbit/s", "N"},
        {"version", 0, 0, G_OPTION_ARG_NONE, &version, "Version ausgeben", NULL},
        {NULL}};
    GOptionContext *ctx = g_option_context_new("- Encoder für IRL4YOU");
    g_option_context_add_main_entries(ctx, opts, NULL);
    g_option_context_add_group(ctx, gst_init_get_option_group());
    GError *err = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
        fprintf(stderr, "irl4you-encoder: %s\n", err->message);
        usage();
        return EXIT_USAGE;
    }
    g_option_context_free(ctx);
    if (version) {
        printf("irl4you-encoder %s schnittstelle=%d\n", VERSION, IFACE_VERSION);
        return EXIT_OK;
    }
    A.start_us = now_us();
    if (!A.pipeline_file || !srt) {
        usage();
        return EXIT_USAGE;
    }
    char *colon = strrchr(srt, ':');
    if (!colon || (A.srt_port = atoi(colon + 1)) <= 0) {
        usage();
        return EXIT_USAGE;
    }
    A.srt_host = g_strndup(srt, colon - srt);
    if (A.srt_host[0] == '[') { /* [IPv6]:Port */
        char *h = g_strndup(A.srt_host + 1, strlen(A.srt_host) - 2);
        g_free(A.srt_host);
        A.srt_host = h;
    }
    A.events = g_queue_new();
    A.props = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    A.pending = g_byte_array_new();
    A.outq = g_async_queue_new_full((GDestroyNotify)g_bytes_unref);
    for (int i = 0; i < MAX_CAMS; i++) A.cams[i].id = i;

    char *main_desc = NULL;
    if (!load_pipeline_file(A.pipeline_file, &main_desc))
        return EXIT_USAGE;
    A.main = gst_parse_launch_full(main_desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    if (!A.main) {
        fprintf(stderr, "irl4you-encoder: Haupt-Pipeline ungültig: %s\n", err ? err->message : "?");
        return EXIT_USAGE;
    }
    g_free(main_desc);
    GstElement *out = gst_bin_get_by_name(GST_BIN(A.main), "out");
    if (!out || !GST_IS_APP_SINK(out)) {
        fprintf(stderr, "irl4you-encoder: in [main] fehlt 'appsink name=out'\n");
        return EXIT_USAGE;
    }
    GstAppSinkCallbacks ocb = {.new_sample = on_out};
    g_object_set(out, "sync", FALSE, "max-buffers", 0, NULL);
    gst_app_sink_set_callbacks(GST_APP_SINK(out), &ocb, NULL, NULL);
    gst_object_unref(out);
    A.venc = gst_bin_get_by_name(GST_BIN(A.main), "venc");
    if (A.venc) {
        A.venc_bps = g_object_class_find_property(G_OBJECT_GET_CLASS(A.venc), "bps") != NULL;
        GstPad *vp = gst_element_get_static_pad(A.venc, "sink");
        if (vp) {
            gst_pad_add_probe(vp, GST_PAD_PROBE_TYPE_BUFFER, on_venc_buffer, NULL, NULL);
            gst_object_unref(vp);
        }
    }
    else
        event("start: kein Element 'venc': Bitrate wird nicht geregelt");

    for (int i = 0; i < MAX_CAMS; i++) {
        cam_t *c = &A.cams[i];
        char n[16];
        snprintf(n, sizeof n, "cam%d_vin", i);
        GstElement *v = gst_bin_get_by_name(GST_BIN(A.main), n);
        snprintf(n, sizeof n, "cam%d_ain", i);
        GstElement *a = gst_bin_get_by_name(GST_BIN(A.main), n);
        if (v && GST_IS_APP_SRC(v)) {
            c->vin = GST_APP_SRC(v);
            g_object_set(v, "is-live", TRUE, "format", GST_FORMAT_TIME, "max-buffers", (guint64)8, "leaky-type", 2 /* downstream */, NULL);
        }
        if (a && GST_IS_APP_SRC(a)) {
            c->ain = GST_APP_SRC(a);
            g_object_set(a, "is-live", TRUE, "format", GST_FORMAT_TIME, "max-buffers", (guint64)100, "leaky-type", 2, NULL);
        }
        c->want_run = c->desc != NULL && !c->idle;
    }

    srt_startup();
    ratectl_init(&A.rc, 500, 6000, start_kbps, A.latency_ms);
    read_bitrate_file(TRUE);
    apply_bitrate(A.rc.target_kbps);

    A.loop = g_main_loop_new(NULL, FALSE);
    GstBus *bus = gst_element_get_bus(A.main);
    gst_bus_add_watch(bus, on_main_bus, NULL);
    gst_object_unref(bus);
    if (A.control_path) {
        if (access(A.control_path, F_OK) != 0 && mkfifo(A.control_path, 0660) != 0)
            event("start: Steuer-FIFO %s: %s", A.control_path, strerror(errno));
        A.ctl_fd = open(A.control_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        A.ctl_dummy = open(A.control_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC); /* hält den FIFO offen, kein Dauer-EOF */
        if (A.ctl_fd >= 0)
            g_unix_fd_add(A.ctl_fd, G_IO_IN, on_control, NULL);
    }
    g_unix_signal_add(SIGINT, on_signal, NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_unix_signal_add(SIGHUP, on_hup, NULL);
    g_timeout_add(RC_TICK_MS, on_rc_tick, NULL);
    g_timeout_add(1000, on_watch, NULL);
    g_timeout_add(1000, on_stats, NULL);

    A.sender = g_thread_new("srt", sender_thread, NULL);
    if (gst_element_set_state(A.main, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "irl4you-encoder: Haupt-Pipeline startet nicht\n");
        return EXIT_PIPELINE;
    }
    A.last_out_us = now_us(); /* Stall-Wächter ab Start */
    A.srt_ok_us = now_us();
    for (int i = 0; i < MAX_CAMS; i++)
        if (A.cams[i].want_run)
            cam_start(&A.cams[i]);
    event("start: SRT %s:%d, Latenz %d ms", A.srt_host, A.srt_port, A.latency_ms);

    g_main_loop_run(A.loop);

    write_stats();
    for (int i = 0; i < MAX_CAMS; i++)
        cam_stop(&A.cams[i]);
    gst_element_set_state(A.main, GST_STATE_NULL);
    g_atomic_int_set(&A.sender_stop, 1);
    g_thread_join(A.sender);
    if (A.sock != SRT_INVALID_SOCK)
        srt_close(A.sock);
    srt_cleanup();
    return A.exit_code;
}

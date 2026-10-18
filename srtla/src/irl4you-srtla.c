/*
 * irl4you-srtla: bundles several network uplinks into one SRTLA group.
 *
 * The encoder talks plain SRT (UDP) to the local listen address. Every packet is
 * sent on to the SRTLA receiver over one of the uplinks; answers coming back on any
 * uplink go to the encoder. Wire format: see ../PROTOKOLL.md.
 *
 * Written from scratch for IRL4YOU (MIT licence).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef IRL4YOU_SRTLA_VERSION
#define IRL4YOU_SRTLA_VERSION "dev"
#endif

/* SRTLA packet types (PROTOKOLL.md) */
#define T_ALIVE     0x9000
#define T_RXLIST    0x9100
#define T_GROUP_REQ 0x9200
#define T_JOIN      0x9201
#define T_JOINED    0x9202
#define T_JOIN_ERR  0x9210
#define T_NO_GROUP  0x9211
#define T_GROUP_NAK 0x9212
#define KEY_LEN     256

/* SRT control types we look at */
#define SRT_CTL_ACK 2
#define SRT_CTL_NAK 3

#define MAX_LINKS      16
#define BUF_LEN        2048
#define TRACK_LEN      65536 /* power of two */
#define TRACK_MASK     (TRACK_LEN - 1)

#define US             1000000LL
#define MS             1000LL
#define TICK_MS        20
#define RETRY_US       (400 * MS)     /* group request / join repeat */
#define NAK_RETRY_US   (1 * US)       /* wait after the receiver refused a new group */
#define REOPEN_US      (2 * US)       /* socket re-open after a hard error */
#define LINK_SILENT_US (4 * US)       /* no answer that long: link counts as gone */
#define PROBE_EVERY_US (200 * MS)
#define PROBE_LOST_US  (1500 * MS)
#define RESOLVE_US     (10 * US)
#define EXPIRE_MIN_US  (1500 * MS)

#define REORDER_MIN_MS 10.0  /* scheduling and queueing noise even on a fast link */
#define REORDER_MAX_MS 120.0 /* a gap this old is loss even on a slow, jittery link */

#define WIN_INIT 32.0
#define WIN_MIN  12.0 /* receivers report every 10 packets, stay above that */
#define WIN_MAX  4096.0

enum { L_FREE, L_CLOSED, L_JOINING, L_UP };
enum { G_NONE, G_ASKED, G_OK };
enum { MODE_BEST, MODE_ALL };

typedef struct {
    int slot_state;           /* L_FREE = unused slot */
    char ip[INET_ADDRSTRLEN];
    char dev[IF_NAMESIZE];
    bool seen;                /* reload mark */
    int fd;
    int64_t t_open_try, t_join_tx, t_rx, t_probe;
    bool probing, have_rtt, used;
    double srtt, jitter;      /* ms */
    double win, win_cap;
    int64_t t_cut;
    int inflight;
    double recent;            /* decaying packet count for the share */
    uint64_t tx_pkts, tx_bytes, lost, reordered, last_bytes;
    double kbps;
    double loss;              /* smoothed share of packets reported lost (0..1) */
    uint64_t last_lost, last_pkts;
    int last_errno;
} link_t;

typedef struct {
    uint32_t seq;
    int64_t t;
    int8_t link;
    bool live;
} track_t;

static struct {
    /* options */
    char listen_host[64];
    int listen_port;
    char server_host[256];
    int server_port;
    char links_file[512];
    char status_file[512];
    int mode;
    bool lat_pref;
    int margin_ms;
    int min_share;
    int burst;                /* packets in a row on one link before switching */
    bool bind_dev;
    bool quiet;

    int lfd;
    struct sockaddr_storage client;
    socklen_t client_len;
    bool have_client;

    struct sockaddr_in server;
    bool have_server;
    int64_t t_resolved;

    link_t links[MAX_LINKS];
    uint8_t key[KEY_LEN];
    int group;
    int run_link, run_left;
    int64_t t_group_tx, t_group_nak;
    int ask_next;

    track_t track[TRACK_LEN];
    uint32_t lookup[TRACK_LEN];
    uint32_t head, tail;

    uint32_t ack_seen[8];
    int64_t ack_seen_t[8];
    int ack_seen_pos;

    uint64_t dropped, to_client;
    int64_t t_start;
} S;

static volatile sig_atomic_t want_reload, want_stop;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * US + ts.tv_nsec / 1000;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }

/* ---- output: one JSON object per line on stdout ---- */

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            fprintf(f, "\\u%04x", *s);
        else
            fputc(*s, f);
    }
    fputc('"', f);
}

static void event(const char *ev, const link_t *l, const char *fmt, ...)
{
    if (S.quiet && strcmp(ev, "fatal") != 0)
        return;
    printf("{\"ev\":\"%s\",\"t\":%.3f", ev, (now_us() - S.t_start) / 1e6);
    if (l) {
        printf(",\"ip\":");
        json_str(stdout, l->ip);
    }
    if (fmt) {
        char msg[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        printf(",\"msg\":");
        json_str(stdout, msg);
    }
    printf("}\n");
    fflush(stdout);
}

static const char *link_state_name(const link_t *l)
{
    switch (l->slot_state) {
    case L_UP: return l->used ? "used" : "reserve";
    case L_JOINING: return "joining";
    default: return "down";
    }
}

static void write_stats(FILE *f)
{
    double sum = 0;
    for (int i = 0; i < MAX_LINKS; i++)
        if (S.links[i].slot_state != L_FREE)
            sum += S.links[i].recent;
    fprintf(f, "{\"ev\":\"stats\",\"t\":%.3f,\"group\":\"%s\",\"client\":%s,\"mode\":\"%s\",\"dropped\":%llu,\"links\":[",
            (now_us() - S.t_start) / 1e6, S.group == G_OK ? "ok" : S.group == G_ASKED ? "asking" : "none",
            S.have_client ? "true" : "false", S.mode == MODE_ALL ? "all" : "best", (unsigned long long)S.dropped);
    bool first = true;
    for (int i = 0; i < MAX_LINKS; i++) {
        const link_t *l = &S.links[i];
        if (l->slot_state == L_FREE)
            continue;
        fprintf(f, "%s{\"ip\":", first ? "" : ",");
        json_str(f, l->ip);
        fprintf(f, ",\"dev\":");
        json_str(f, l->dev);
        fprintf(f, ",\"state\":\"%s\"", link_state_name(l));
        if (l->have_rtt)
            fprintf(f, ",\"rtt_ms\":%.1f,\"jitter_ms\":%.1f", l->srtt, l->jitter);
        else
            fprintf(f, ",\"rtt_ms\":null,\"jitter_ms\":null");
        fprintf(f, ",\"window\":%d,\"inflight\":%d,\"kbps\":%.0f,\"share\":%.0f,\"packets\":%llu,\"lost\":%llu,\"reordered\":%llu,\"loss_pct\":%.1f",
                (int)l->win, l->inflight, l->kbps, sum > 0 ? 100.0 * l->recent / sum : 0.0,
                (unsigned long long)l->tx_pkts, (unsigned long long)l->lost, (unsigned long long)l->reordered, l->loss * 100);
        if (l->last_errno)
            fprintf(f, ",\"error\":\"%s\"", strerror(l->last_errno));
        fprintf(f, "}");
        first = false;
    }
    fprintf(f, "]}\n");
}

static void emit_stats(void)
{
    if (!S.quiet) {
        write_stats(stdout);
        fflush(stdout);
    }
    if (!S.status_file[0])
        return;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", S.status_file);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    write_stats(f);
    if (fclose(f) == 0)
        rename(tmp, S.status_file);
}

/* ---- server address ---- */

static bool resolve_server(void)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res;
    char port[16];
    snprintf(port, sizeof port, "%d", S.server_port);
    S.t_resolved = now_us();
    int rc = getaddrinfo(S.server_host, port, &hints, &res);
    if (rc != 0) {
        event("warn", NULL, "Adresse %s nicht aufgelöst: %s", S.server_host, gai_strerror(rc));
        return false;
    }
    struct sockaddr_in a;
    memcpy(&a, res->ai_addr, sizeof a);
    freeaddrinfo(res);
    bool changed = !S.have_server || a.sin_addr.s_addr != S.server.sin_addr.s_addr || a.sin_port != S.server.sin_port;
    S.server = a;
    S.have_server = true;
    if (changed) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
        event("server", NULL, "%s:%d", ip, S.server_port);
    }
    return changed;
}

/* ---- links ---- */

static void link_close(link_t *l)
{
    if (l->fd >= 0)
        close(l->fd);
    l->fd = -1;
    if (l->slot_state != L_FREE)
        l->slot_state = L_CLOSED;
    l->probing = false;
}

static void link_reset_path(link_t *l)
{
    l->have_rtt = false;
    l->win = WIN_INIT;
    l->win_cap = WIN_MAX;
    l->used = false;
}

static void link_open(link_t *l, int64_t now)
{
    l->t_open_try = now;
    if (!S.have_server)
        return;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        l->last_errno = errno;
        return;
    }
    int buf = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    if (S.bind_dev && l->dev[0] &&
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, l->dev, (socklen_t)strlen(l->dev)) != 0)
        event("warn", l, "an Karte %s binden fehlgeschlagen: %s", l->dev, strerror(errno));
    struct sockaddr_in src = {.sin_family = AF_INET};
    if (inet_pton(AF_INET, l->ip, &src.sin_addr) != 1 || bind(fd, (struct sockaddr *)&src, sizeof src) != 0 ||
        connect(fd, (struct sockaddr *)&S.server, sizeof S.server) != 0) {
        int e = errno ? errno : EINVAL;
        if (l->last_errno != e)
            event("link_error", l, "%s", strerror(e));
        l->last_errno = e;
        close(fd);
        return;
    }
    l->fd = fd;
    l->last_errno = 0;
    l->slot_state = L_JOINING;
    l->t_join_tx = 0;
    l->t_rx = now;
    link_reset_path(l);
    event("link_open", l, NULL);
}

static void track_retire(track_t *e)
{
    if (!e->live)
        return;
    e->live = false;
    link_t *l = &S.links[e->link];
    if (l->inflight > 0)
        l->inflight--;
}

static void link_down(link_t *l, const char *why)
{
    if (l->slot_state == L_UP)
        event("link_down", l, "%s", why);
    l->slot_state = l->fd >= 0 ? L_JOINING : L_CLOSED;
    l->t_join_tx = 0;
    l->probing = false;
    link_reset_path(l);
    /* packets still counted on this link will not be confirmed any more */
    for (uint32_t i = S.head; i != S.tail; i++) {
        track_t *e = &S.track[i & TRACK_MASK];
        if (e->live && &S.links[e->link] == l)
            track_retire(e);
    }
    l->inflight = 0;
}

static int link_send(link_t *l, const void *buf, size_t len)
{
    if (l->fd < 0)
        return -1;
    ssize_t n = send(l->fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n >= 0)
        return 0;
    int e = errno;
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR || e == ECONNREFUSED || e == ENOBUFS)
        return -1;
    /* address gone, network down etc: reopen later */
    if (l->last_errno != e)
        event("link_error", l, "%s", strerror(e));
    l->last_errno = e;
    link_down(l, strerror(e));
    link_close(l);
    return -1;
}

static void send_join(link_t *l, int64_t now)
{
    uint8_t p[2 + KEY_LEN];
    wr16(p, T_JOIN);
    memcpy(p + 2, S.key, KEY_LEN);
    link_send(l, p, sizeof p);
    l->t_join_tx = now;
}

static void group_reset(const char *why)
{
    if (S.group != G_NONE)
        event("group_lost", NULL, "%s", why);
    S.group = G_NONE;
    S.t_group_tx = 0;
    for (int i = 0; i < MAX_LINKS; i++) {
        link_t *l = &S.links[i];
        if (l->slot_state == L_UP)
            link_down(l, "Gruppe neu");
    }
}

static void group_ask(int64_t now)
{
    if (now - S.t_resolved > RESOLVE_US && resolve_server()) {
        for (int i = 0; i < MAX_LINKS; i++)
            if (S.links[i].fd >= 0)
                link_close(&S.links[i]);
    }
    for (int tries = 0; tries < MAX_LINKS; tries++) {
        link_t *l = &S.links[S.ask_next++ % MAX_LINKS];
        if (l->fd < 0)
            continue;
        if (S.group == G_NONE) {
            if (getrandom(S.key, KEY_LEN, 0) != KEY_LEN) {
                for (int i = 0; i < KEY_LEN; i++)
                    S.key[i] = (uint8_t)rand();
            }
        }
        uint8_t p[2 + KEY_LEN];
        wr16(p, T_GROUP_REQ);
        memcpy(p + 2, S.key, KEY_LEN);
        if (link_send(l, p, sizeof p) == 0) {
            S.group = G_ASKED;
            S.t_group_tx = now;
            return;
        }
    }
    S.t_group_tx = now;
}

/* ---- in-flight tracking ---- */

static track_t *track_find(uint32_t seq)
{
    uint32_t idx = S.lookup[seq & TRACK_MASK];
    if (idx - S.head >= S.tail - S.head)
        return NULL;
    track_t *e = &S.track[idx & TRACK_MASK];
    return (e->live && e->seq == seq) ? e : NULL;
}

static void track_add(uint32_t seq, int li, int64_t now)
{
    track_t *old = track_find(seq);
    if (old)
        track_retire(old); /* retransmission: only the newest copy counts */
    if (S.tail - S.head == TRACK_LEN) {
        track_retire(&S.track[S.head & TRACK_MASK]);
        S.head++;
    }
    track_t *e = &S.track[S.tail & TRACK_MASK];
    *e = (track_t){.seq = seq, .t = now, .link = (int8_t)li, .live = true};
    S.lookup[seq & TRACK_MASK] = S.tail;
    S.tail++;
    S.links[li].inflight++;
}

static int64_t expire_after(const link_t *l)
{
    int64_t t = l->have_rtt ? (int64_t)(4 * (l->srtt + l->jitter) * MS) : 0;
    return t > EXPIRE_MIN_US ? t : EXPIRE_MIN_US;
}

static void track_expire(int64_t now)
{
    while (S.head != S.tail) {
        track_t *e = &S.track[S.head & TRACK_MASK];
        if (e->live && now - e->t < expire_after(&S.links[e->link]))
            break;
        track_retire(e);
        S.head++;
    }
}

static void window_grow(link_t *l)
{
    if (l->inflight + 1 < l->win / 2)
        return; /* not using the window: no reason to widen it */
    l->win += l->win < l->win_cap ? 1.0 : 3.0 / l->win;
    if (l->win > WIN_MAX)
        l->win = WIN_MAX;
}

static void window_cut(link_t *l, int64_t now)
{
    int64_t gap = l->have_rtt ? (int64_t)(l->srtt * MS) : 100 * MS;
    if (gap < 50 * MS)
        gap = 50 * MS;
    if (now - l->t_cut < gap)
        return;
    l->t_cut = now;
    l->win *= 0.9;
    if (l->win < WIN_MIN)
        l->win = WIN_MIN;
    l->win_cap = l->win;
}

/* ---- choosing a link ---- */

static double lat_score(const link_t *l) { return l->srtt + 2 * l->jitter; }

static void classify_links(void)
{
    double best = INFINITY;
    for (int i = 0; i < MAX_LINKS; i++) {
        link_t *l = &S.links[i];
        if (l->slot_state == L_UP && l->have_rtt && lat_score(l) < best)
            best = lat_score(l);
    }
    for (int i = 0; i < MAX_LINKS; i++) {
        link_t *l = &S.links[i];
        if (l->slot_state != L_UP)
            l->used = false;
        else if (!S.lat_pref || best == INFINITY)
            l->used = true;
        else
            l->used = l->have_rtt && lat_score(l) <= best + S.margin_ms;
    }
}

/* 1.0 for a clean link, down to 0.25 for one that loses a quarter of its packets or more */
static double link_quality(const link_t *l)
{
    double q = 1.0 - 3.0 * l->loss;
    return q < 0.25 ? 0.25 : q;
}

static int pick_link_fresh(void)
{
    double sum = 0;
    for (int i = 0; i < MAX_LINKS; i++)
        if (S.links[i].used)
            sum += S.links[i].recent;

    /* minimum share for every used link that has room */
    if (S.min_share > 0 && sum >= 20) {
        int pick = -1;
        double worst = 0;
        for (int i = 0; i < MAX_LINKS; i++) {
            link_t *l = &S.links[i];
            if (!l->used || l->inflight >= l->win)
                continue;
            double lack = S.min_share / 100.0 - l->recent / sum;
            if (lack > worst) {
                worst = lack;
                pick = i;
            }
        }
        if (pick >= 0)
            return pick;
    }

    /* pass 0: used links with room, pass 1: reserve links with room, pass 2: anything up */
    for (int pass = 0; pass < 3; pass++) {
        int pick = -1;
        double best = 0;
        for (int i = 0; i < MAX_LINKS; i++) {
            link_t *l = &S.links[i];
            if (l->slot_state != L_UP)
                continue;
            if (pass == 0 && !l->used)
                continue;
            double room = (l->win - l->inflight) / l->win;
            if (pass < 2 && room <= 0)
                continue;
            if (pass < 2)
                room *= link_quality(l); /* a link that really loses packets gets less */
            double v;
            if (pass == 2)
                v = room;
            else if (S.mode == MODE_BEST || pass == 1)
                v = l->have_rtt ? -lat_score(l) : -1e9;
            else
                v = room;
            if (pick < 0 || v > best) {
                best = v;
                pick = i;
            }
        }
        if (pick >= 0)
            return pick;
    }
    return -1;
}

/* Consecutive packets stay on one link for a short run: links differ in delay, and spreading every single packet
 * makes the receiver see gaps (and NAK them) that are only reordering. Runs keep the order inside the run and
 * shrink the number of switch points. */
static int pick_link(void)
{
    if (S.run_left > 0 && S.run_link >= 0) {
        const link_t *l = &S.links[S.run_link];
        if (l->slot_state == L_UP && l->used && l->inflight < l->win) {
            S.run_left--;
            return S.run_link;
        }
    }
    int li = pick_link_fresh();
    S.run_link = li;
    S.run_left = li >= 0 ? S.burst - 1 : 0;
    return li;
}

static int fastest_link(void)
{
    int pick = -1;
    for (int i = 0; i < MAX_LINKS; i++) {
        link_t *l = &S.links[i];
        if (l->slot_state != L_UP)
            continue;
        if (pick < 0 || (l->have_rtt && (!S.links[pick].have_rtt || lat_score(l) < lat_score(&S.links[pick]))))
            pick = i;
    }
    return pick;
}

/* ---- packet handling ---- */

static void from_client(const uint8_t *buf, size_t n, int64_t now)
{
    if (n < 4)
        return;
    bool data = !(buf[0] & 0x80);
    int li = data ? pick_link() : fastest_link();
    if (li < 0) {
        S.dropped++;
        return;
    }
    link_t *l = &S.links[li];
    if (link_send(l, buf, n) != 0) {
        S.dropped++;
        return;
    }
    l->tx_pkts++;
    l->tx_bytes += n;
    if (data) {
        l->recent += 1;
        track_add(rd32(buf) & 0x7fffffff, li, now);
    }
}

static void to_client(const uint8_t *buf, size_t n)
{
    if (!S.have_client)
        return;
    sendto(S.lfd, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&S.client, S.client_len);
    S.to_client++;
}

static bool ack_is_repeat(const uint8_t *buf, size_t n, int64_t now)
{
    /* the same SRT ACK may arrive on every link; pass it on only once */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ buf[i]) * 16777619u;
    for (int i = 0; i < 8; i++)
        if (S.ack_seen[i] == h && now - S.ack_seen_t[i] < 500 * MS)
            return true;
    S.ack_seen[S.ack_seen_pos] = h;
    S.ack_seen_t[S.ack_seen_pos] = now;
    S.ack_seen_pos = (S.ack_seen_pos + 1) % 8;
    return false;
}

static void handle_loss_report(const uint8_t *buf, size_t n, int64_t now)
{
    bool hit[MAX_LINKS] = {0};
    for (size_t off = 16; off + 4 <= n; off += 4) {
        uint32_t a = rd32(buf + off), b;
        if (a & 0x80000000u) {
            if (off + 8 > n)
                break;
            a &= 0x7fffffff;
            b = rd32(buf + off + 4) & 0x7fffffff;
            off += 4;
        } else {
            b = a;
        }
        uint32_t span = (b - a) & 0x7fffffff;
        if (span > 4096)
            span = 4096;
        for (uint32_t k = 0; k <= span; k++) {
            track_t *e = track_find((a + k) & 0x7fffffff);
            if (!e)
                continue;
            link_t *ol = &S.links[e->link];
            /* A packet that is younger than a one-way trip plus jitter on its own link can still be on its way:
             * the receiver saw a gap that is only reordering between links. No loss, no smaller window. */
            double young = ol->have_rtt ? ol->srtt / 2 + 2 * ol->jitter : 20;
            young = (young > REORDER_MAX_MS ? REORDER_MAX_MS : young < REORDER_MIN_MS ? REORDER_MIN_MS : young) * MS;
            if (now - e->t < (int64_t)young) {
                ol->reordered++;
                continue;
            }
            hit[e->link] = true;
            ol->lost++;
            track_retire(e);
        }
    }
    for (int i = 0; i < MAX_LINKS; i++)
        if (hit[i])
            window_cut(&S.links[i], now);
}

static void handle_rx_list(link_t *l, const uint8_t *buf, size_t n)
{
    (void)l;
    for (size_t off = 4; off + 4 <= n; off += 4) {
        track_t *e = track_find(rd32(buf + off) & 0x7fffffff);
        if (!e)
            continue;
        link_t *owner = &S.links[e->link];
        track_retire(e);
        window_grow(owner);
    }
}

static void rtt_sample(link_t *l, double ms)
{
    if (!l->have_rtt) {
        l->srtt = ms;
        l->jitter = ms / 2;
        l->have_rtt = true;
        return;
    }
    l->jitter = 0.75 * l->jitter + 0.25 * fabs(l->srtt - ms);
    l->srtt = 0.875 * l->srtt + 0.125 * ms;
}

static void from_link(link_t *l, const uint8_t *buf, size_t n, int64_t now)
{
    if (n < 2)
        return;
    l->t_rx = now;
    uint16_t type = rd16(buf);
    if (type >= 0x9000 && type <= 0x92ff) {
        switch (type) {
        case T_ALIVE:
            if (l->probing) {
                rtt_sample(l, (now - l->t_probe) / 1000.0);
                l->probing = false;
            }
            break;
        case T_RXLIST:
            handle_rx_list(l, buf, n);
            break;
        case T_JOIN:
            if (S.group == G_ASKED && n == 2 + KEY_LEN && memcmp(buf + 2, S.key, KEY_LEN / 2) == 0) {
                memcpy(S.key, buf + 2, KEY_LEN);
                S.group = G_OK;
                event("group_ok", l, NULL);
                for (int i = 0; i < MAX_LINKS; i++)
                    if (S.links[i].slot_state == L_JOINING)
                        send_join(&S.links[i], now);
            }
            break;
        case T_JOINED:
            if (S.group == G_OK && l->slot_state == L_JOINING) {
                l->slot_state = L_UP;
                l->t_probe = 0;
                event("link_up", l, NULL);
            }
            break;
        case T_JOIN_ERR:
            event("link_error", l, "Empfänger lehnt die Leitung ab");
            break;
        case T_NO_GROUP:
            if (S.group == G_OK)
                group_reset("Empfänger kennt die Gruppe nicht");
            break;
        case T_GROUP_NAK:
            if (S.group == G_ASKED && now - S.t_group_nak > NAK_RETRY_US) {
                S.t_group_nak = now;
                event("warn", l, "Empfänger lehnt neue Gruppe ab");
                S.group = G_NONE;
            }
            break;
        }
        return;
    }
    /* SRT from the server */
    if (n >= 16 && (buf[0] & 0x80)) {
        uint16_t ctl = rd16(buf) & 0x7fff;
        if (ctl == SRT_CTL_ACK && ack_is_repeat(buf, n, now))
            return;
        if (ctl == SRT_CTL_NAK)
            handle_loss_report(buf, n, now);
    }
    to_client(buf, n);
}

/* ---- links file ---- */

static void copy_str(char *dst, size_t size, const char *src)
{
    size_t n = strnlen(src, size - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void load_links(int64_t now)
{
    FILE *f = fopen(S.links_file, "r");
    if (!f) {
        event("warn", NULL, "Leitungsliste %s nicht lesbar: %s", S.links_file, strerror(errno));
        return;
    }
    for (int i = 0; i < MAX_LINKS; i++)
        S.links[i].seen = false;
    char line[256];
    int count = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char ip[64] = "", dev[64] = "";
        if (sscanf(line, "%63s %63s", ip, dev) < 1)
            continue;
        struct in_addr tmp;
        if (inet_pton(AF_INET, ip, &tmp) != 1 || strlen(dev) >= IF_NAMESIZE) {
            event("warn", NULL, "Zeile in der Leitungsliste ignoriert: %s", ip);
            continue;
        }
        link_t *l = NULL;
        for (int i = 0; i < MAX_LINKS && !l; i++)
            if (S.links[i].slot_state != L_FREE && strcmp(S.links[i].ip, ip) == 0)
                l = &S.links[i];
        if (l && strcmp(l->dev, dev) != 0) {
            link_down(l, "Karte geändert");
            link_close(l);
            copy_str(l->dev, sizeof l->dev, dev);
        }
        if (!l) {
            for (int i = 0; i < MAX_LINKS && !l; i++)
                if (S.links[i].slot_state == L_FREE)
                    l = &S.links[i];
            if (!l) {
                event("warn", NULL, "mehr als %d Leitungen, %s ignoriert", MAX_LINKS, ip);
                continue;
            }
            memset(l, 0, sizeof *l);
            l->fd = -1;
            l->slot_state = L_CLOSED;
            copy_str(l->ip, sizeof l->ip, ip);
            copy_str(l->dev, sizeof l->dev, dev);
            link_reset_path(l);
            link_open(l, now);
        }
        if (!l->seen)
            count++;
        l->seen = true;
    }
    fclose(f);
    for (int i = 0; i < MAX_LINKS; i++) {
        link_t *l = &S.links[i];
        if (l->slot_state != L_FREE && !l->seen) {
            event("link_removed", l, NULL);
            link_down(l, "aus der Liste entfernt");
            link_close(l);
            l->slot_state = L_FREE;
        }
    }
    event("links", NULL, "%d Leitung(en)", count);
    if (count == 0)
        event("warn", NULL, "keine Leitung in %s", S.links_file);
}

/* ---- timers ---- */

static void tick(int64_t now)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        link_t *l = &S.links[i];
        if (l->slot_state == L_FREE)
            continue;
        if (l->fd < 0) {
            if (now - l->t_open_try >= REOPEN_US)
                link_open(l, now);
            continue;
        }
        if (l->slot_state == L_UP && now - l->t_rx > LINK_SILENT_US)
            link_down(l, "keine Antwort");
        if (l->slot_state == L_JOINING && S.group == G_OK && now - l->t_join_tx >= RETRY_US)
            send_join(l, now);
        if (l->slot_state == L_UP) {
            if (l->probing && now - l->t_probe > PROBE_LOST_US)
                l->probing = false;
            if (!l->probing && now - l->t_probe >= PROBE_EVERY_US) {
                uint8_t p[2];
                wr16(p, T_ALIVE);
                if (link_send(l, p, 2) == 0) {
                    l->probing = true;
                    l->t_probe = now;
                }
            }
        }
    }
    if (S.group != G_OK && now - S.t_group_tx >= RETRY_US) {
        if (S.group == G_ASKED)
            S.group = G_NONE; /* no answer, start over with a fresh key */
        group_ask(now);
    }
    classify_links();
}

static void every_100ms(void)
{
    for (int i = 0; i < MAX_LINKS; i++)
        S.links[i].recent *= 0.9;
}

static void every_second(double secs)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        link_t *l = &S.links[i];
        l->kbps = (l->tx_bytes - l->last_bytes) * 8 / 1000.0 / secs;
        l->last_bytes = l->tx_bytes;
        uint64_t dp = l->tx_pkts - l->last_pkts, dl = l->lost - l->last_lost;
        l->last_pkts = l->tx_pkts;
        l->last_lost = l->lost;
        if (dp >= 20)
            l->loss += 0.3 * ((double)dl / (double)dp - l->loss);
    }
    emit_stats();
}

/* ---- setup ---- */

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        want_reload = 1;
    else
        want_stop = 1;
}

static bool split_host_port(const char *s, char *host, size_t hl, int *port)
{
    const char *c = strrchr(s, ':');
    if (!c || c == s || (size_t)(c - s) >= hl)
        return false;
    memcpy(host, s, c - s);
    host[c - s] = 0;
    char *end;
    long p = strtol(c + 1, &end, 10);
    if (*end || p < 1 || p > 65535)
        return false;
    *port = (int)p;
    return true;
}

static void usage(FILE *f)
{
    fprintf(f,
            "irl4you-srtla %s: bündelt mehrere Netzwerkleitungen zu einem SRTLA-Empfänger\n"
            "\n"
            "  irl4you-srtla -s HOST:PORT -i DATEI [Optionen]\n"
            "\n"
            "  -s, --server HOST:PORT    SRTLA-Empfänger\n"
            "  -i, --links DATEI         Leitungen, je Zeile: IP-Adresse [Netzwerkkarte]; SIGHUP liest neu\n"
            "  -l, --listen ADR:PORT     hier nimmt das Programm SRT vom Encoder an (Standard 127.0.0.1:5000)\n"
            "  -m, --mode best|all       best: schnellste Leitung zuerst, all: alle genutzten gleichmäßig (Standard best)\n"
            "      --margin MS           Toleranz zur schnellsten Leitung (Standard best 50, all 300)\n"
            "      --no-latency          Laufzeit bei der Auswahl nicht beachten\n"
            "      --min-share PROZENT   Mindestanteil je genutzter Leitung, 0-40 (Standard best 0, all 10)\n"
            "      --burst N             N Pakete in Folge auf einer Leitung, 1-64 (Standard 8)\n"
            "      --bind-device         zusätzlich an die Netzwerkkarte binden (braucht CAP_NET_RAW)\n"
            "      --status DATEI        Zustand jede Sekunde als JSON in diese Datei schreiben\n"
            "  -q, --quiet               nichts auf stdout ausgeben\n"
            "  -V, --version             Version zeigen\n",
            IRL4YOU_SRTLA_VERSION);
}

static void parse_args(int argc, char **argv)
{
    static const struct option opts[] = {
        {"server", required_argument, 0, 's'},  {"links", required_argument, 0, 'i'},
        {"listen", required_argument, 0, 'l'},  {"mode", required_argument, 0, 'm'},
        {"margin", required_argument, 0, 1},    {"no-latency", no_argument, 0, 2},
        {"min-share", required_argument, 0, 3}, {"bind-device", no_argument, 0, 4},
        {"status", required_argument, 0, 5},  {"burst", required_argument, 0, 6},    {"quiet", no_argument, 0, 'q'},
        {"version", no_argument, 0, 'V'},       {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};
    int margin = -1, share = -1, c;
    char *end;
    snprintf(S.listen_host, sizeof S.listen_host, "127.0.0.1");
    S.listen_port = 5000;
    S.lat_pref = true;
    S.burst = 8;
    while ((c = getopt_long(argc, argv, "s:i:l:m:qVh", opts, NULL)) != -1) {
        switch (c) {
        case 's':
            if (!split_host_port(optarg, S.server_host, sizeof S.server_host, &S.server_port))
                goto bad;
            break;
        case 'i':
            snprintf(S.links_file, sizeof S.links_file, "%s", optarg);
            break;
        case 'l':
            if (!split_host_port(optarg, S.listen_host, sizeof S.listen_host, &S.listen_port))
                goto bad;
            break;
        case 'm':
            if (strcmp(optarg, "best") == 0)
                S.mode = MODE_BEST;
            else if (strcmp(optarg, "all") == 0)
                S.mode = MODE_ALL;
            else
                goto bad;
            break;
        case 1:
            margin = (int)strtol(optarg, &end, 10);
            if (*end || margin < 0 || margin > 10000)
                goto bad;
            break;
        case 2:
            S.lat_pref = false;
            break;
        case 3:
            share = (int)strtol(optarg, &end, 10);
            if (*end || share < 0 || share > 40)
                goto bad;
            break;
        case 4:
            S.bind_dev = true;
            break;
        case 6:
            S.burst = (int)strtol(optarg, &end, 10);
            if (*end || S.burst < 1 || S.burst > 64)
                goto bad;
            break;
        case 5:
            snprintf(S.status_file, sizeof S.status_file, "%s", optarg);
            break;
        case 'q':
            S.quiet = true;
            break;
        case 'V':
            printf("irl4you-srtla %s\n", IRL4YOU_SRTLA_VERSION);
            exit(0);
        case 'h':
            usage(stdout);
            exit(0);
        default:
            goto bad;
        }
    }
    if (optind != argc || !S.server_host[0] || !S.links_file[0])
        goto bad;
    S.margin_ms = margin >= 0 ? margin : S.mode == MODE_ALL ? 300 : 50;
    S.min_share = share >= 0 ? share : S.mode == MODE_ALL ? 10 : 0;
    return;
bad:
    usage(stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);
    S.t_start = now_us();
    for (int i = 0; i < MAX_LINKS; i++)
        S.links[i].fd = -1;

    struct sigaction sa = {.sa_handler = on_signal};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    S.lfd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in la = {.sin_family = AF_INET, .sin_port = htons((uint16_t)S.listen_port)};
    int buf = 1 << 20;
    setsockopt(S.lfd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    setsockopt(S.lfd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    if (S.lfd < 0 || inet_pton(AF_INET, S.listen_host, &la.sin_addr) != 1 ||
        bind(S.lfd, (struct sockaddr *)&la, sizeof la) != 0) {
        event("fatal", NULL, "kann %s:%d nicht öffnen: %s", S.listen_host, S.listen_port, strerror(errno));
        return 1;
    }

    event("start", NULL, "irl4you-srtla %s, Modus %s, Toleranz %s, Mindestanteil %d%%", IRL4YOU_SRTLA_VERSION,
          S.mode == MODE_ALL ? "all" : "best", S.lat_pref ? "an" : "aus", S.min_share);
    resolve_server();
    int64_t now = now_us();
    load_links(now);

    int64_t t_100 = now, t_1s = now;
    uint8_t pkt[BUF_LEN];
    struct pollfd pfd[1 + MAX_LINKS];
    int map[1 + MAX_LINKS];

    while (!want_stop) {
        if (want_reload) {
            want_reload = 0;
            load_links(now_us());
        }
        int n = 0;
        pfd[n] = (struct pollfd){.fd = S.lfd, .events = POLLIN};
        map[n++] = -1;
        for (int i = 0; i < MAX_LINKS; i++) {
            if (S.links[i].fd >= 0) {
                pfd[n] = (struct pollfd){.fd = S.links[i].fd, .events = POLLIN};
                map[n++] = i;
            }
        }
        int rc = poll(pfd, (nfds_t)n, TICK_MS);
        now = now_us();
        if (rc < 0 && errno != EINTR) {
            event("fatal", NULL, "poll: %s", strerror(errno));
            return 1;
        }
        for (int k = 0; rc > 0 && k < n; k++) {
            if (!(pfd[k].revents & (POLLIN | POLLERR)))
                continue;
            for (int burst = 0; burst < 256; burst++) {
                if (map[k] < 0) {
                    struct sockaddr_storage from;
                    socklen_t fl = sizeof from;
                    ssize_t r = recvfrom(S.lfd, pkt, sizeof pkt, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
                    if (r < 0)
                        break;
                    if (!S.have_client || fl != S.client_len || memcmp(&from, &S.client, fl) != 0) {
                        memcpy(&S.client, &from, fl);
                        S.client_len = fl;
                        if (!S.have_client)
                            event("client", NULL, NULL);
                        S.have_client = true;
                    }
                    from_client(pkt, (size_t)r, now);
                } else {
                    link_t *l = &S.links[map[k]];
                    if (l->fd != pfd[k].fd)
                        break; /* closed while handling this round */
                    ssize_t r = recv(l->fd, pkt, sizeof pkt, MSG_DONTWAIT);
                    if (r < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNREFUSED)
                            break;
                        l->last_errno = errno;
                        link_down(l, strerror(errno));
                        link_close(l);
                        break;
                    }
                    from_link(l, pkt, (size_t)r, now);
                }
            }
        }
        if (now - t_100 >= 100 * MS) {
            track_expire(now);
            every_100ms();
            t_100 = now;
        }
        tick(now);
        if (now - t_1s >= US) {
            every_second((now - t_1s) / 1e6);
            t_1s = now;
        }
    }
    event("stop", NULL, NULL);
    return 0;
}

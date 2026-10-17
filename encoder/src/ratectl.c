/* irl4you-encoder: Bitraten-Regelung.
 *
 * Ziel: Gebündelte Mobilfunkleitungen haben eine Laufzeit, die im Normalbetrieb um 15 bis 30 ms schwankt. Darauf soll die Regelung
 * nicht reagieren. Bei echtem Stau (Laufzeit steigt deutlich, Sendepuffer läuft voll) muss sie dagegen sofort und kräftig bremsen.
 *
 * Messgrößen: Laufzeit (RTT) und gefüllter Sendepuffer in ms. Daraus:
 *   rtt_base  gleitendes Minimum der Laufzeit (wird alle BASE_WINDOW_MS behutsam angehoben, damit eine dauerhaft längere Strecke
 *             nicht als Stau gilt)
 *   rtt_dev   Schwankung: mittlere Änderung von einer Messung zur nächsten (ein dauerhafter Sprung der Laufzeit zählt
 *             dadurch nur einmal und hebt die Schwellen nicht an)
 * Drei Zustände: ruhig (langsam erhöhen), Anstieg (in kleinen Schritten senken), Stau (sofort auf einen Bruchteil senken, danach
 * eine Weile nicht erhöhen). Alle Schwellen stehen hier oben, damit sie mit Messungen nachgestellt werden können.
 */
#include "ratectl.h"

#include <math.h>
#include <stddef.h>

#define TICK_UP_MS          1000    /* frühestens so oft erhöhen */
#define TICK_DOWN_MS         500    /* frühestens so oft leicht senken */
#define HOLD_AFTER_JAM_MS   3000    /* nach Stau so lange nicht erhöhen */
#define BASE_WINDOW_MS     30000    /* Minimum der Laufzeit wird nach dieser Zeit neu bestimmt */

#define CALM_TOL_MS           15.0  /* ruhig: Laufzeit höchstens so viel über der Grundlinie … */
#define CALM_DEV_FACTOR        2.0  /* … oder so viele mittlere Abweichungen */
#define RISE_TOL_MS           40.0  /* Anstieg: mehr als so viel über der Grundlinie … */
#define RISE_DEV_FACTOR        3.0  /* … und mehr als so viele mittlere Abweichungen */
#define JAM_TOL_MS            80.0  /* Stau: mehr als so viel über der Grundlinie … */
#define JAM_LAT_FRACTION       0.2  /* … oder mehr als dieser Anteil der SRT-Latenz */
#define JAM_BUF_FRACTION       0.4  /* Stau auch, wenn der Sendepuffer mehr als diesen Anteil der Latenz hält */
#define CALM_BUF_MS          100.0  /* ruhig nur mit wenig im Puffer */
#define BUF_GROW_MS         2000    /* Puffer wächst so lange stetig … */
#define BUF_GROW_MIN_MS       50.0  /* … und dabei um mindestens so viel: wie Anstieg behandeln */
#define BUF_GROW_PAUSE_MS   1000    /* wächst er so lange nicht weiter, gilt das Wachsen als beendet */

#define UP_STEP_KBPS            50
#define UP_STEP_FRACTION      0.02
#define DOWN_STEP_FRACTION    0.05
#define JAM_FACTOR            0.70

#define EWMA_ALPHA            0.125

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static double maxd(double a, double b) { return a > b ? a : b; }

void ratectl_init(ratectl_t *rc, int min_kbps, int max_kbps, int start_kbps, int latency_ms)
{
    *rc = (ratectl_t){0};
    rc->min_kbps = min_kbps;
    rc->max_kbps = max_kbps;
    rc->latency_ms = latency_ms > 0 ? latency_ms : 2000;
    rc->target_kbps = clampi(start_kbps > 0 ? start_kbps : min_kbps, min_kbps, max_kbps);
    rc->state = RC_STEADY;
}

void ratectl_set_limits(ratectl_t *rc, int min_kbps, int max_kbps)
{
    if (min_kbps > max_kbps) { int t = min_kbps; min_kbps = max_kbps; max_kbps = t; }
    rc->min_kbps = min_kbps;
    rc->max_kbps = max_kbps;
    rc->target_kbps = clampi(rc->target_kbps, min_kbps, max_kbps);
}

const char *ratectl_state_name(rc_state_t st)
{
    switch (st) {
    case RC_STEADY: return "ruhig";
    case RC_RISING: return "anstieg";
    case RC_CONGESTED: return "stau";
    }
    return "?";
}

int ratectl_update(ratectl_t *rc, const rc_sample_t *s)
{
    if (s->rtt_ms <= 0) /* keine Messung (noch nicht verbunden) */
        return rc->target_kbps;

    if (!rc->initialized) {
        rc->rtt_base_ms = s->rtt_ms;
        rc->rtt_avg_ms = s->rtt_ms;
        rc->rtt_dev_ms = 5.0;
        rc->rtt_last_ms = s->rtt_ms;
        rc->base_reset_ms = s->now_ms;
        rc->last_buf_ms = s->sndbuf_ms;
        rc->initialized = 1;
    }

    /* Grundlinie: Minimum, nach Ablauf des Fensters behutsam an das Mittel heranführen */
    if (s->rtt_ms < rc->rtt_base_ms)
        rc->rtt_base_ms = s->rtt_ms;
    else if (s->now_ms - rc->base_reset_ms > BASE_WINDOW_MS) {
        rc->rtt_base_ms += (rc->rtt_avg_ms - rc->rtt_base_ms) * 0.25;
        rc->base_reset_ms = s->now_ms;
    }
    double diff = fabs(s->rtt_ms - rc->rtt_last_ms);
    rc->rtt_last_ms = s->rtt_ms;
    rc->rtt_avg_ms += (s->rtt_ms - rc->rtt_avg_ms) * EWMA_ALPHA;
    rc->rtt_dev_ms += (diff - rc->rtt_dev_ms) * EWMA_ALPHA;

    /* Wächst der Puffer stetig? */
    if (s->sndbuf_ms > rc->last_buf_ms + 1.0) {
        if (!rc->buf_grow_since_ms) {
            rc->buf_grow_since_ms = s->now_ms;
            rc->buf_grow_from_ms = rc->last_buf_ms;
        }
        rc->buf_last_grow_ms = s->now_ms;
    } else if (s->sndbuf_ms + 1.0 < rc->last_buf_ms || s->now_ms - rc->buf_last_grow_ms > BUF_GROW_PAUSE_MS) {
        rc->buf_grow_since_ms = 0;
    }
    rc->last_buf_ms = s->sndbuf_ms;

    double over = s->rtt_ms - rc->rtt_base_ms;          /* Einzelwert: nur für Stau (schnell reagieren) */
    double over_avg = rc->rtt_avg_ms - rc->rtt_base_ms; /* geglättet: für Anstieg/ruhig (Schwankung ausblenden) */
    double jam_rtt = maxd(JAM_TOL_MS, rc->latency_ms * JAM_LAT_FRACTION);
    double rise_rtt = maxd(RISE_TOL_MS, RISE_DEV_FACTOR * rc->rtt_dev_ms);
    double calm_rtt = maxd(CALM_TOL_MS, CALM_DEV_FACTOR * rc->rtt_dev_ms);
    int growing = rc->buf_grow_since_ms && s->now_ms - rc->buf_grow_since_ms > BUF_GROW_MS &&
                  s->sndbuf_ms - rc->buf_grow_from_ms > BUF_GROW_MIN_MS;

    int t = rc->target_kbps;
    if (over > jam_rtt || s->sndbuf_ms > rc->latency_ms * JAM_BUF_FRACTION) {
        rc->state = RC_CONGESTED;
        if (s->now_ms - rc->last_down_ms >= TICK_DOWN_MS) {
            t = (int)(t * JAM_FACTOR);
            rc->last_down_ms = s->now_ms;
        }
        rc->hold_until_ms = s->now_ms + HOLD_AFTER_JAM_MS;
    } else if (over_avg > rise_rtt || growing) {
        rc->state = RC_RISING;
        if (s->now_ms - rc->last_down_ms >= TICK_DOWN_MS) {
            int step = (int)(t * DOWN_STEP_FRACTION);
            t -= step > 10 ? step : 10;
            rc->last_down_ms = s->now_ms;
        }
    } else {
        rc->state = RC_STEADY;
        if (over_avg <= calm_rtt && s->sndbuf_ms <= CALM_BUF_MS && s->now_ms >= rc->hold_until_ms &&
            s->now_ms - rc->last_up_ms >= TICK_UP_MS) {
            int step = (int)(t * UP_STEP_FRACTION);
            t += step > UP_STEP_KBPS ? step : UP_STEP_KBPS;
            rc->last_up_ms = s->now_ms;
        }
    }
    rc->target_kbps = clampi(t, rc->min_kbps, rc->max_kbps);
    return rc->target_kbps;
}

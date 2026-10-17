/* irl4you-encoder: Bitraten-Regelung (reine Rechenlogik, ohne GStreamer/SRT, damit sie für sich testbar ist). */
#ifndef IRL4YOU_RATECTL_H
#define IRL4YOU_RATECTL_H

#include <stdint.h>

typedef enum { RC_STEADY = 0, RC_RISING, RC_CONGESTED } rc_state_t;

typedef struct {
    /* Einstellungen */
    int min_kbps, max_kbps;     /* Grenzen aus der Bitraten-Datei */
    int latency_ms;             /* SRT-Latenz */
    /* Zustand */
    int target_kbps;
    double rtt_base_ms;         /* gleitendes Minimum der Laufzeit */
    double rtt_dev_ms;          /* Schwankung: gleitendes Mittel der Änderung von Messung zu Messung */
    double rtt_last_ms;
    double rtt_avg_ms;
    int64_t base_reset_ms;      /* wann das Minimum zuletzt neu aufgesetzt wurde */
    int64_t last_down_ms, last_up_ms, hold_until_ms;
    int64_t buf_grow_since_ms, buf_last_grow_ms;
    double buf_grow_from_ms;
    double last_buf_ms;
    rc_state_t state;
    int initialized;
} ratectl_t;

typedef struct {
    double rtt_ms;      /* aktuelle Laufzeit */
    double sndbuf_ms;   /* Daten im Sendepuffer, in ms */
    int64_t now_ms;     /* monotone Uhr */
} rc_sample_t;

void ratectl_init(ratectl_t *rc, int min_kbps, int max_kbps, int start_kbps, int latency_ms);
void ratectl_set_limits(ratectl_t *rc, int min_kbps, int max_kbps);
/* Neue Messung verarbeiten; liefert die neue Zielbitrate in kbit/s. */
int ratectl_update(ratectl_t *rc, const rc_sample_t *s);
const char *ratectl_state_name(rc_state_t st);

#endif

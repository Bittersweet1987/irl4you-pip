/* Einheitstests der Bitraten-Regelung: läuft ohne GStreamer und SRT. */
#include <stdio.h>
#include <stdlib.h>

#include "../src/ratectl.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FEHLER %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* t Sekunden lang mit fester Laufzeit/Puffer laufen lassen (Messung alle 250 ms) */
static int run(ratectl_t *rc, int64_t *now, double secs, double rtt, double jitter, double buf)
{
    int k = 0;
    for (int i = 0; i < (int)(secs * 4); i++) {
        double r = rtt + ((i % 4) - 1.5) / 1.5 * jitter;
        rc_sample_t s = {.rtt_ms = r, .sndbuf_ms = buf, .now_ms = *now};
        k = ratectl_update(rc, &s);
        *now += 250;
    }
    return k;
}

int main(void)
{
    ratectl_t rc;
    int64_t now = 1000;

    /* 1. Ruhige Leitung: steigt bis zur Obergrenze */
    ratectl_init(&rc, 500, 8000, 1000, 2000);
    int k = run(&rc, &now, 120, 40, 2, 20);
    CHECK(k == 8000, "ruhig: erwartet 8000, ist %d", k);

    /* 2. Normales Schwanken um 25 ms (gebündeltes Mobilfunknetz): darf nicht in die Knie gehen */
    ratectl_init(&rc, 500, 8000, 6000, 2000);
    now = 1000;
    run(&rc, &now, 5, 60, 2, 20);
    k = run(&rc, &now, 60, 70, 25, 40);
    CHECK(k >= 5500, "Schwankung: Bitrate fiel auf %d", k);

    /* 3. Echter Stau: Laufzeit +300 ms, Puffer voll -> schnell deutlich runter */
    ratectl_init(&rc, 500, 8000, 6000, 2000);
    now = 1000;
    run(&rc, &now, 10, 50, 2, 20);
    k = run(&rc, &now, 2, 350, 5, 900);
    CHECK(k <= 3000, "Stau: erwartet <= 3000, ist %d", k);
    CHECK(rc.state == RC_CONGESTED, "Stau: Zustand %s", ratectl_state_name(rc.state));

    /* 4. Nach dem Stau erholt sie sich wieder (nicht auf dem Minimum hängen bleiben) */
    k = run(&rc, &now, 120, 50, 2, 20);
    CHECK(k >= 7000, "Erholung: nur %d", k);

    /* 5. Grenzen werden eingehalten, auch nach Änderung */
    ratectl_set_limits(&rc, 1000, 2500);
    CHECK(rc.target_kbps <= 2500 && rc.target_kbps >= 1000, "Grenzen: %d", rc.target_kbps);
    ratectl_set_limits(&rc, 9000, 3000); /* vertauscht */
    CHECK(rc.min_kbps == 3000 && rc.max_kbps == 9000, "vertauschte Grenzen");

    /* 6. Ohne Messung (nicht verbunden) bleibt die Bitrate stehen */
    ratectl_init(&rc, 500, 8000, 4000, 2000);
    rc_sample_t none = {.rtt_ms = 0, .sndbuf_ms = 0, .now_ms = 5000};
    CHECK(ratectl_update(&rc, &none) == 4000, "ohne Messung verändert");

    /* 7. Stetig wachsender Puffer bei unauffälliger Laufzeit: sanft senken */
    ratectl_init(&rc, 500, 8000, 6000, 2000);
    now = 1000;
    run(&rc, &now, 5, 40, 1, 10);
    for (int i = 0; i < 20; i++) {
        rc_sample_t s = {.rtt_ms = 42, .sndbuf_ms = 10 + i * 15, .now_ms = now};
        k = ratectl_update(&rc, &s);
        now += 250;
    }
    CHECK(k < 6000, "wachsender Puffer: keine Senkung (%d)", k);

    /* 8. Laufzeit steigt dauerhaft um 60 ms (Stau baut sich langsam auf, Puffer noch klein): senken, aber nicht als Stau */
    ratectl_init(&rc, 500, 8000, 6000, 2000);
    now = 1000;
    run(&rc, &now, 10, 40, 2, 20);
    k = run(&rc, &now, 5, 100, 2, 60);
    CHECK(k < 6000, "Anstieg: keine Senkung (%d)", k);
    CHECK(rc.state == RC_RISING, "Anstieg: Zustand %s", ratectl_state_name(rc.state));

    printf("%s (%d Fehler)\n", fails ? "FEHLGESCHLAGEN" : "OK", fails);
    return fails ? 1 : 0;
}

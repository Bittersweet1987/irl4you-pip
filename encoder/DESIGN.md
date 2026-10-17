# irl4you-encoder: Entwurf

Eigenständiger Encoder für IRL4YOU. Neu entworfen nach den Anforderungen von IRL4YOU, ohne Quelltext oder Schnittstelle von belacoder.
Sprache C (C17), GStreamer 1.28, libsrt 1.5 (Haivision). Lizenz: wie IRL4YOU (vom Projekt festzulegen).

## Aufbau

Statt einer großen Pipeline, in der ein ausgefallener Kamera-Zweig alles mitreißt, besteht der Encoder aus **getrennten Pipelines**:

```
 Kamera-Pipeline cam0 (rtmpsrc … Decoder … Skalierung … appsink)  ─┐
 Kamera-Pipeline cam1 …                                             ├─► Haupt-Pipeline (appsrc cam0_in … Mischer … Encoder venc … Muxer … appsink out)
 Kamera-Pipeline camN …                                            ─┘                                                           │
                                                                                                                                 ▼
                                                                                          SRT-Sender (libsrt, Caller) ──► lokaler Bündel-Sender / Server
```

- Jede Kamera läuft in ihrer eigenen `GstPipeline`. Fällt sie aus (Handy hört auf zu senden, Stream fehlt), wird nur diese Pipeline
  gestoppt und nach kurzer Pause neu gestartet; die Haupt-Pipeline läuft ungestört weiter.
- Die Bilder und der Ton werden im Prozess von `appsink` (Kamera) nach `appsrc camN_in` (Haupt) weitergereicht, ohne Kopie
  (DMA-Puffer des Hardware-Decoders bleiben erhalten). Beim Übergeben bekommen sie neue Zeitstempel der Haupt-Uhr.
- Der Mischer der Haupt-Pipeline arbeitet live (Aggregator mit Zeitlimit): fehlt eine Kamera, läuft das Bild mit den anderen weiter.

## Pipeline-Datei

Textdatei mit Abschnitten, jeder Abschnitt eine gst-launch-Beschreibung:

```
[main]
appsrc name=cam0_vin ! … ! mix.sink_0
…
mpph265enc name=venc … ! h265parse ! mpegtsmux name=mux ! appsink name=out

[cam 0]
rtmpsrc location=rtmp://127.0.0.1/publish/handy ! flvdemux name=d
d.video ! … ! appsink name=vout
d.audio ! … ! appsink name=aout
```

Pflicht: in `[main]` ein `appsink name=out` (MPEG-TS) und ein Video-Encoder `name=venc` mit Bitraten-Eigenschaft (`bps` bei MPP).
Zu jeder Kamera `N`: `appsink name=vout`/`aout` in `[cam N]` und `appsrc name=camN_vin`/`camN_ain` in `[main]`.
Eine Kamera ohne Abschnitt oder mit `#off` gilt als abwesend.

## Schnittstelle

Aufruf, Pipeline-Datei, Steuerbefehle, Statistik und Rückgabewerte: [SCHNITTSTELLE.md](SCHNITTSTELLE.md) (verbindlich).

## Bitraten-Regelung (eigener Entwurf)

Ziel: Mobilfunk-Bündel mit normal schwankender Laufzeit (±15–30 ms) nicht unnötig drosseln, bei echtem Stau aber schnell bremsen.
Messgrößen je 250 ms aus libsrt (`srt_bstats`): Laufzeit (RTT), Bytes im Sendepuffer, Verluste, Senderate.

- Grundlinie `rtt_base` = gleitendes Minimum der RTT über 30 s; Schwankung `rtt_dev` = mittlere Änderung von Messung zu Messung.
- Stau wird am Einzelwert erkannt, Anstieg und Ruhe am geglätteten Mittel (so lösen normale Schwankungen kein Senken aus).
- **Stau** (sofort bremsen auf 70 %): Sendepuffer > 40 % der Latenz-Laufzeit oder RTT > `rtt_base` + max(80 ms, Latenz/5).
- **Anstieg** (sanft um 5 % senken, höchstens alle 500 ms): RTT > `rtt_base` + max(40 ms, 3·`rtt_dev`) oder Puffer wächst über 2 s.
- **Ruhig** (erhöhen um max(50 kbit/s, 2 %) je Sekunde): RTT innerhalb `rtt_base` + max(15 ms, 2·`rtt_dev`) und Puffer klein.
- Grenzen aus `--bitrate-file`; nach Stau 3 s Sperre fürs Erhöhen.
- Alle Schwellen als Konstanten oben in `ratectl.c`, damit sie mit echten Messungen nachgestellt werden können.

## Wächter

- Ausgang: kommt 5 s lang nichts aus `out`, beendet sich der Encoder mit 3 (IRL4YOU startet neu).
- Kamera: liefert eine laufende Kamera 3 s lang nichts, gilt sie als ausgefallen (Statistik), nach 10 s wird ihre Pipeline neu gestartet.

## Tests

- Einheitstests der Regelung: `tests/test_ratectl.c` (`meson test`).
- Laufzeit-Test ohne Kamera und Hardware: `tests/laufzeit-test.sh` (Testbild, x264enc, SRT zu `srt-live-transmit`).
- Auf der Box: Testbild-Pipeline (`videotestsrc`) gegen einen lokalen SRT-Empfänger (`srt-live-transmit`), Kamera aus/an im Betrieb,
  Verschieben, Stau simulieren (`tc netem`), Stall auslösen. Diese Tests kommen in die Testsuite.

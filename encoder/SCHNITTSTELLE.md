# Schnittstelle von irl4you-encoder (Stand 17.10.2026, Version 1)

Verbindlich für IRL4YOU (`pipbox_send.py`, `pipbox_live.py`, `pipbox_watch.py`, `server.py`). Beide Programme sind neu entworfen, die
Schnittstelle ist eigenständig und bildet belacoder/srtla_send nicht nach. Änderungen an dieser Datei bitte nur mit der Sitzung
„Encoder und SRTLA“ abstimmen; neue Felder kommen hinzu, vorhandene ändern ihre Bedeutung nicht (`"v"` zählt hoch, wenn doch).

Sendekette:

```
Kameras (RTMP 1935) ─► irl4you-encoder ──SRT──► 127.0.0.1:9100 irl4you-srtla ══ eine UDP-Verbindung je Netz ══► SRTLA-Server ─► SRT-Ziel
```

Dateien und Pfade (IRL4YOU legt sie fest und übergibt sie als Argumente; Vorschlag):

| Was | Pfad |
|---|---|
| Programm Encoder | `/usr/bin/irl4you-encoder` (Paket `irl4you-encoder`) |
| Pipeline-Datei | `/var/tmp/pipbox/pipeline` |
| Bitraten-Datei | `/var/tmp/pipbox/bitrate` |
| Steuer-FIFO Encoder | `/run/pipbox-send/encoder.ctl` |
| Statistik Encoder | `/run/pipbox-send/encoder-stats.json` |
| lokaler SRT-Port | `9100` (UDP, nur 127.0.0.1) |

---

## 1. irl4you-encoder

### Aufruf

```
irl4you-encoder --pipeline DATEI --srt HOST:PORT [Optionen]
```

| Option | Bedeutung | Standard |
|---|---|---|
| `--pipeline DATEI` | Pipeline-Datei (siehe unten) | Pflicht |
| `--srt HOST:PORT` | SRT-Ziel (Caller). Für IPv6 `[addr]:port` | Pflicht |
| `--latency MS` | SRT-Latenz | 2000 |
| `--streamid TEXT` | SRT-Stream-ID | keine |
| `--bitrate-file DATEI` | Grenzen für die Regelung, siehe unten | 500 bis 6000 kbit/s |
| `--start-kbps N` | Bitrate beim Start (wird in die Grenzen gelegt) | Untergrenze |
| `--control FIFO` | Steuerbefehle (wird angelegt, wenn nicht vorhanden) | keiner |
| `--stats DATEI` | Statistik als JSON, jede Sekunde atomar ersetzt | keine |
| `--align MS` | Vorlauf in ms, um den die Bilder aller Kameras hinter der Uhr der Box eingeplant werden (Ausrichtung) | 0 |
| `--version` | gibt `irl4you-encoder <Version> schnittstelle=1` aus und endet | |

Signale: `SIGTERM`/`SIGINT` beenden sauber (Rückgabe 0), `SIGHUP` liest die Bitraten-Datei sofort neu.

Rückgabewerte:

| Wert | Bedeutung | Was IRL4YOU tun sollte |
|---|---|---|
| 0 | beendet auf Wunsch | nichts |
| 2 | Aufruf oder Pipeline-Datei falsch | nicht endlos neu starten, Meldung zeigen |
| 3 | Ausgang steht (5 s lang kein MPEG-TS) | neu starten |
| 4 | SRT-Ziel 30 s lang nicht erreichbar | neu starten |
| 5 | Fehler in der Haupt-Pipeline (Encoder, Mischer, Muxer) | neu starten |

### Bitraten-Datei

Zwei ganze Zahlen in **kbit/s**, getrennt durch Leerzeichen oder Zeilenumbruch: `Untergrenze Obergrenze`. Wird alle 250 ms auf Änderung
geprüft (Zeit und Größe der Datei) und bei `SIGHUP` sofort gelesen. Zum Ändern atomar ersetzen (`.tmp` schreiben, `rename`).
Weicher Start: erst `start start` schreiben, später die vollen Grenzen; die Regelung bleibt dabei in Betrieb.

### Pipeline-Datei

Textdatei mit Abschnitten, jeder Abschnitt ist eine `gst-launch`-Beschreibung (Zeilen werden zusammengefügt):

```
[main]
… mpph265enc name=venc … ! mpegtsmux name=mux ! appsink name=out
appsrc name=cam0_vin ! … ! mix.sink_0
appsrc name=cam0_ain ! … ! audiosel.sink_1

[cam 0]
#idle
rtmpsrc name=cam0_src location=rtmp://127.0.0.1:1935/publish/KEY ! flvdemux name=cam0
cam0.video ! … ! appsink name=vout
cam0.audio ! … ! appsink name=aout
```

- Steht kein Abschnittskopf in der Datei, ist der ganze Text `[main]` (einfacher Bildaufbau mit einer Pipeline, z. B. Einzelkamera).
- `[main]` braucht `appsink name=out` (liefert MPEG-TS). Ein Element `name=venc` wird geregelt: Eigenschaft `bps` (bit/s, MPP) oder
  `bitrate` (kbit/s, z. B. x264enc). Fehlt `venc`, läuft der Encoder ohne Regelung.
- `[cam N]` (N = 0 bis 7): eigene Pipeline je Kamera mit `appsink name=vout` (Bild) und optional `appsink name=aout` (Ton). Ihre Ausgaben
  gehen in `appsrc name=camN_vin` bzw. `camN_ain` der Haupt-Pipeline.
- Zeilen mit `#` sind Kommentare. Zwei Kennzeichen gelten als Anweisung:
  - `#off`: Abschnitt wird ignoriert (Kamera abwesend).
  - `#idle`: Kamera startet gestoppt (Zustand 1) und läuft erst nach `{"op":"cam","id":N,"run":true}`.
- Elementnamen sind frei, müssen aber über alle Pipelines eindeutig sein (Steuerbefehle suchen sie in allen Pipelines).

### Steuerbefehle (FIFO, eine JSON-Zeile je Befehl)

| `op` | Felder | Wirkung |
|---|---|---|
| `prop` | `el`, `name`, `value` | Eigenschaft eines Elements setzen (Zahl, Wahrheitswert oder Text; Caps als Text). Gilt auch für Elemente einer gerade gestoppten Kamera: wird gemerkt und beim Start gesetzt. |
| `pad` | `el`, `pad`, `name`, `value` | Eigenschaft eines Pads (z. B. Mischer `mix` / `sink_2` / `xpos`) |
| `audio` | `el` (Standard `audiosel`), `pad` | aktives Pad eines `input-selector` |
| `cam` | `id`, `run` (true/false), optional `pipeline` (neue Beschreibung) | Kamera-Pipeline starten oder stoppen |
| `offset` | `id`, `ms` (0 bis 10000) | Bild und Ton dieser Kamera um `ms` später einplanen (Verschiebung) |
| `batch` | `cmds` (Liste) | alle Befehle nacheinander ohne Pause, im selben Durchlauf |

Fehler (unbekanntes Element, falscher Wert) stehen als Ereignis in der Statistik und auf stderr; der Befehl wird übersprungen, der Rest
eines `batch` läuft weiter.

### Statistik (`--stats`, JSON, Version 1)

```json
{
  "v": 1, "t": 1792245937, "up_s": 812,
  "fps": 30.0, "frames": 24360,
  "rate":  {"kbps": 6500, "min": 500, "max": 12000, "state": "ruhig"},
  "srt":   {"connected": true, "rtt_ms": 48, "rtt_base_ms": 31, "send_kbps": 6610, "buf_pkts": 12, "buf_ms": 120,
            "sent": 1203345, "lost": 120, "resent": 140, "dropped": 0},
  "out":   {"age_ms": 12, "bytes": 803345920},
  "cams":  {"0": {"state": 3, "live": true, "fps": 30.0, "lag_ms": 40, "offset_ms": 0, "restarts": 0, "audio": true}},
  "events": [{"t": 1792245930, "text": "Kamera 1: Strom beendet"}]
}
```

| Feld | Bedeutung |
|---|---|
| `t` | Unix-Zeit der Datei; `up_s` Laufzeit des Encoders |
| `fps`, `frames` | Bilder je Sekunde bzw. insgesamt **am Encoder** (gemessen am Eingang von `venc`) |
| `rate.kbps` | aktuelle Zielbitrate des Video-Encoders; `min`/`max` die Grenzen; `state` `ruhig`, `anstieg` oder `stau` |
| `srt.*` | Messwerte der SRT-Verbindung: Laufzeit, Grundlinie der Laufzeit, Senderate, Sendepuffer (Pakete/ms), Pakete gesendet, verloren gemeldet, neu gesendet, verworfen (alle seit Start) |
| `out.age_ms` | ms seit dem letzten MPEG-TS-Stück (Stall-Erkennung) |
| `cams.N.state` | **1** gestoppt, **2** soll laufen, aber kein Bild in den letzten 3 s (auch: Quelle fehlt, neuer Versuch alle 3 s), **3** Bilder kommen |
| `cams.N.live` | `state == 3` |
| `cams.N.fps` | Bilder je Sekunde dieser Kamera (letzte Sekunde) |
| `cams.N.lag_ms` | Nachlauf: wie weit der Takt der Kamera seit ihrem Start hinter der Uhr der Box liegt (positiv = Kamera zu langsam) |
| `cams.N.offset_ms`, `restarts`, `audio` | eingestellte Verschiebung, automatische Neustarts der Kamera-Pipeline, Ton kommt |
| `events` | die letzten 20 Meldungen mit Zeit |

Wächter im Encoder: liefert eine laufende Kamera 10 s lang kein Bild, startet der Encoder nur ihre Pipeline neu (`restarts` + 1).

### Meldungen auf stderr

Eine Zeile je Ereignis: `irl4you-encoder: <Bereich>: <Text>`. Bereiche: `start`, `srt`, `kamera N`, `regler`, `befehl`, `pipeline`, `ende`.
Für die Auswertung in IRL4YOU gedacht sind nur Bereich und Rückgabewert; der Text ist für Menschen.

---

## 2. irl4you-srtla

Gehört nicht mehr zu dieser Datei: Aufruf, Statusdatei und Protokoll stehen im Repo `irl4you-pip` unter `srtla/README.md` und
`srtla/PROTOKOLL.md`. Für den Encoder zählt nur: er sendet SRT an die Adresse, die `irl4you-srtla -l ADR:PORT` annimmt
(IRL4YOU nutzt `127.0.0.1:9100`, also `--srt 127.0.0.1:9100` beim Encoder und `-l 127.0.0.1:9100` beim Sender).

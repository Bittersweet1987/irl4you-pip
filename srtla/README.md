# irl4you-srtla: eigener SRTLA-Sender

`irl4you-srtla` bündelt mehrere Netzwerkleitungen (LAN, WLAN, Mobilfunk) zu einem SRTLA-Empfänger, z. B. dem eines fremden
Streaming-Dienstes. Es ist neu geschrieben, ohne Quelltext anderer SRTLA-Programme als Vorlage, und ersetzt unter Ubuntu 26.04 das
bisherige `srtla_send` mit seinen zwei Patches. Was über die Leitung geht, steht in [PROTOKOLL.md](PROTOKOLL.md).

## Bauen und testen

```sh
make            # baut build/irl4you-srtla (nur libc, kein weiteres Paket nötig)
make test       # Protokolltests gegen den Test-Empfänger, alles auf diesem Rechner
sudo make install   # nach /usr/local/bin
```

Die Tests brauchen Python 3. Ist `srt-live-transmit` installiert (Paket `srt-tools`), laufen zusätzlich zwei Tests mit einer
echten SRT-Verbindung (libsrt auf beiden Seiten), eine davon mit 5 % Verlust auf einer Leitung.

## Aufruf

```sh
irl4you-srtla -s srtla.example.net:5000 -i /run/pipbox-send/links -l 127.0.0.1:5000 -m all --status /run/pipbox-send/srtla.json
```

| Option | Bedeutung |
|---|---|
| `-s HOST:PORT` | SRTLA-Empfänger (Pflicht) |
| `-i DATEI` | Leitungsliste (Pflicht), je Zeile `IP-Adresse [Netzwerkkarte]`, `#` leitet Kommentare ein. `SIGHUP` liest sie neu, ohne die Sendung zu unterbrechen. |
| `-l ADR:PORT` | dort nimmt das Programm SRT vom Encoder an (Standard `127.0.0.1:5000`) |
| `-m best` | schnellste Leitung zuerst, die anderen nur, wenn ihr Fenster voll ist (Standard) |
| `-m all` | alle genutzten Leitungen gleichzeitig, nach freiem Fensteranteil |
| `--margin MS` | Toleranz zur schnellsten Leitung; langsamere Leitungen sind Reserve (Standard: best 50, all 300) |
| `--no-latency` | Laufzeit bei der Auswahl nicht beachten |
| `--min-share P` | jede genutzte Leitung bekommt mindestens P % der Pakete, 0–40 (Standard: best 0, all 10) |
| `--burst N` | N Pakete in Folge auf einer Leitung, 1–64 (Standard 8). Weniger Umordnung beim Empfänger, also weniger NAK und Nachsendungen |
| `--bind-device` | Leitung zusätzlich an die Netzwerkkarte aus der Liste binden (braucht root bzw. `CAP_NET_RAW`) |
| `--status DATEI` | Zustand jede Sekunde als JSON in diese Datei (atomar ersetzt) |
| `-q` | nichts auf stdout ausgeben |

Rückgabewert 0 nach `SIGTERM`/`SIGINT`, 1 bei einem Fehler beim Start, 2 bei falschen Optionen. Das Programm gibt nicht auf: Fehlt der
Empfänger oder eine Adresse, versucht es es weiter, bis es beendet wird.

### Quell-Routing

Jede Leitung ist an die IP-Adresse ihrer Karte gebunden. Damit die Pakete auch über diese Karte rausgehen, braucht das System je
Quelladresse eine eigene Routing-Tabelle (`ip rule from <IP> lookup <Tabelle>`); das richten die Netzwerk-Hooks von IRL4YOU ein.
Mit `--bind-device` geht es zusätzlich ohne diese Regeln.

## Ausgabe

Eine JSON-Zeile je Ereignis auf stdout, z. B.

```json
{"ev":"link_up","t":0.021,"ip":"10.0.0.2"}
{"ev":"stats","t":12.0,"group":"ok","client":true,"mode":"all","dropped":0,"links":[
  {"ip":"10.0.0.2","dev":"eth0","state":"used","rtt_ms":38.5,"jitter_ms":2.1,"window":96,"inflight":40,
   "kbps":5200,"share":61,"packets":81234,"lost":3}]}
```

Ereignisse: `start`, `server`, `links`, `link_open`, `group_ok`, `link_up`, `link_down`, `link_error`, `link_removed`, `group_lost`,
`client`, `warn`, `stats` (jede Sekunde), `stop`, `fatal`. Zustand einer Leitung: `used` (genutzt), `reserve`, `joining` (meldet sich
an), `down` (keine Verbindung, z. B. Adresse fehlt; dann steht der Grund in `error`).

## Test-Empfänger

`testrecv/irl4you-srtla-testrecv.py` verhält sich wie ein SRTLA-Empfänger und gibt den Strom an einen SRT-Server weiter. Einzelne
Leitungen lassen sich langsam, verlustreich oder tot schalten:

```sh
testrecv/irl4you-srtla-testrecv.py --listen 0.0.0.0:5000 --target 127.0.0.1:4001 --rule 10.0.0.3:delay=150,loss=0.02
```

## Was IRL4YOU noch umstellen muss

`pipbox_send.py` startet heute `srtla_send PORT HOST PORT DATEI` mit den Umgebungsvariablen der Patches und liest `links:`-Zeilen;
`server.py` (`uplink_states`) wertet sie aus. Mit dem neuen Programm: Aufruf wie oben, `spread` → `-m best|all`, Zustand aus
`--status` statt aus den Textzeilen (Felder `state`, `rtt_ms`). Die Leitungsliste kann unverändert bleiben (nur IP-Adressen).

## Bisheriger Sender (nur noch für die 22.04-Box)

`build.sh` und die beiden Patches bauen weiter das alte `srtla_send` für die laufende 22.04-Installation (`install/install.sh`). Sie
werden entfernt, sobald IRL4YOU auf `irl4you-srtla` umgestellt ist.

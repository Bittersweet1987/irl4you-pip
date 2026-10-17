#!/bin/bash
# Laufzeit-Test von irl4you-encoder ohne Kamera und ohne Hardware-Encoder: Testbild (x264enc) -> SRT -> srt-live-transmit -> Datei.
# Aufruf im 26.04-System (oder chroot): tests/laufzeit-test.sh build/irl4you-encoder
# Prüft: Start, SRT-Verbindung, MPEG-TS kommt an, Statistik v1, Kamera-Zweige (#idle, Start per Befehl, ausgefallene Kamera),
# Steuerbefehle, Bitrate aus Datei, sauberes Ende per SIGTERM.
set -u
ENC=${1:-./b/irl4you-encoder}
W=$(mktemp -d)
PORT=${PORT:-47101}
fails=0
ok()   { echo "OK      $*"; }
fail() { echo "FEHLER  $*"; fails=$((fails + 1)); }
cleanup() { kill $RX $EP 2>/dev/null; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
J() { python3 -c "import json,sys; d=json.load(open('$W/stats.json')); print($1)" 2>/dev/null; }

cat > "$W/pipeline" <<EOF
[main]
compositor name=mix background=black latency=300000000 sink_0::zorder=1 sink_1::zorder=2 sink_1::xpos=400 sink_1::ypos=20 sink_1::width=200 sink_1::height=112 !
video/x-raw,format=I420,width=640,height=360,framerate=30/1 ! queue max-size-buffers=4 leaky=downstream !
x264enc name=venc tune=zerolatency speed-preset=ultrafast key-int-max=60 bitrate=800 ! h264parse config-interval=-1 ! queue ! mux.
input-selector name=audiosel ! audioconvert ! avenc_aac ! aacparse ! queue ! mux.
audiotestsrc is-live=true wave=silence ! audio/x-raw,rate=48000,channels=2 ! queue ! audiosel.sink_0
appsrc name=cam0_vin ! queue max-size-buffers=3 ! mix.sink_0
appsrc name=cam0_ain ! queue ! audiosel.sink_1
appsrc name=cam1_vin ! queue max-size-buffers=3 ! mix.sink_1
appsrc name=cam2_vin ! queue max-size-buffers=3 ! mix.sink_2
mpegtsmux name=mux ! appsink name=out

[cam 0]
videotestsrc name=cam0_src is-live=true pattern=ball ! video/x-raw,format=I420,width=640,height=360,framerate=30/1 ! appsink name=vout
audiotestsrc is-live=true freq=440 ! audio/x-raw,rate=48000,channels=2 ! appsink name=aout

[cam 1]
#idle
videotestsrc name=cam1_src is-live=true pattern=smpte ! video/x-raw,format=I420,width=640,height=360,framerate=30/1 ! videoscale ! capsfilter name=cam1_scc caps=video/x-raw,width=200,height=112 ! appsink name=vout

[cam 2]
rtmpsrc location=rtmp://127.0.0.1:1/gibtsnicht ! flvdemux name=d d.video ! h264parse ! avdec_h264 ! appsink name=vout
EOF
echo "800 1500" > "$W/bitrate"

srt-live-transmit -q "srt://:$PORT?mode=listener&latency=200" "file://con" > "$W/out.ts" 2>/dev/null &
RX=$!
sleep 0.5
"$ENC" --pipeline "$W/pipeline" --srt 127.0.0.1:$PORT --latency 200 --bitrate-file "$W/bitrate" \
       --control "$W/ctl" --stats "$W/stats.json" --align 200 > "$W/stderr.txt" 2>&1 &
EP=$!
sleep 6

[ -s "$W/stats.json" ] && ok "Statistik wird geschrieben" || fail "keine Statistik"
[ "$(J 'd["v"]')" = "1" ] && ok "Statistik Version 1" || fail "Statistik-Version: $(J 'd.get("v")')"
[ "$(J 'd["srt"]["connected"]')" = "True" ] && ok "SRT verbunden" || fail "SRT nicht verbunden"
fps=$(J 'int(d["fps"])'); [ "${fps:-0}" -ge 25 ] && ok "Encoder-Bildrate $fps" || fail "Encoder-Bildrate ${fps:-?}"
[ "$(J 'd["cams"]["0"]["state"]')" = "3" ] && ok "Kamera 0 liefert (Zustand 3)" || fail "Kamera 0 Zustand $(J 'd["cams"]["0"]["state"]')"
[ "$(J 'd["cams"]["0"]["audio"]')" = "True" ] && ok "Kamera 0 Ton" || fail "Kamera 0 ohne Ton"
[ "$(J 'd["cams"]["1"]["state"]')" = "1" ] && ok "Kamera 1 #idle gestoppt (Zustand 1)" || fail "Kamera 1 Zustand $(J 'd["cams"]["1"]["state"]')"
[ "$(J 'd["cams"]["2"]["state"]')" = "2" ] && ok "Kamera 2 ohne Quelle: Zustand 2" || fail "Kamera 2 Zustand $(J 'd["cams"]["2"]["state"]')"
[ "$(J 'd["rate"]["min"]'),$(J 'd["rate"]["max"]')" = "800,1500" ] && ok "Bitraten-Datei gelesen" || fail "Bitraten-Grenzen $(J 'd["rate"]')"

# Steuerbefehle: Kamera 1 starten, Eigenschaft einer gestoppten Kamera, Pad, Ton, Verschiebung, Fehler
echo '{"op":"batch","cmds":[{"op":"prop","el":"cam1_src","name":"pattern","value":"snow"},{"op":"cam","id":1,"run":true},{"op":"pad","el":"mix","pad":"sink_1","name":"alpha","value":0.8},{"op":"audio","el":"audiosel","pad":"sink_1"},{"op":"offset","id":0,"ms":250},{"op":"prop","el":"gibtsnicht","name":"x","value":1}]}' > "$W/ctl"
echo "900 1400" > "$W/bitrate.tmp" && mv "$W/bitrate.tmp" "$W/bitrate"
sleep 4
[ "$(J 'd["cams"]["1"]["state"]')" = "3" ] && ok "Kamera 1 per Befehl gestartet" || fail "Kamera 1 nach Befehl Zustand $(J 'd["cams"]["1"]["state"]')"
[ "$(J 'd["cams"]["0"]["offset_ms"]')" = "250" ] && ok "Verschiebung Kamera 0" || fail "Verschiebung $(J 'd["cams"]["0"]["offset_ms"]')"
J '" ".join(e["text"] for e in d["events"])' | grep -q "gibtsnicht" && ok "Fehler im Befehl als Ereignis" || fail "Befehlsfehler fehlt in events"
[ "$(J 'd["rate"]["min"]'),$(J 'd["rate"]["max"]')" = "900,1400" ] && ok "Bitraten-Datei neu gelesen" || fail "neue Grenzen $(J 'd["rate"]')"
echo '{"op":"cam","id":0,"run":false}' > "$W/ctl"
sleep 2
[ "$(J 'd["cams"]["0"]["state"]')" = "1" ] && ok "Kamera 0 per Befehl gestoppt" || fail "Kamera 0 nach Stop Zustand $(J 'd["cams"]["0"]["state"]')"
fps=$(J 'int(d["fps"])'); [ "${fps:-0}" -ge 25 ] && ok "Bild läuft ohne Kamera 0 weiter ($fps fps)" || fail "Bildrate ohne Kamera 0: ${fps:-?}"

kill -TERM $EP; wait $EP; rc=$?
[ $rc = 0 ] && ok "Ende per SIGTERM, Rückgabe 0" || fail "Rückgabe nach SIGTERM: $rc"
sz=$(stat -c %s "$W/out.ts" 2>/dev/null || echo 0)
python3 - "$W/out.ts" <<'PY' && ok "MPEG-TS empfangen ($sz Byte)" || fail "MPEG-TS fehlt oder kaputt ($sz Byte)"
import sys
d = open(sys.argv[1], "rb").read()
i = d.find(b"\x47")
good = i >= 0 and len(d) > 200000 and all(d[j] == 0x47 for j in range(i, min(len(d), i + 188 * 500), 188))
sys.exit(0 if good else 1)
PY
"$ENC" --pipeline /gibtsnicht --srt 127.0.0.1:1 >/dev/null 2>&1; rc=$?
[ $rc = 2 ] && ok "falsche Pipeline-Datei: Rückgabe 2" || fail "falsche Pipeline-Datei: Rückgabe $rc"

echo "--- stderr des Encoders"; cat "$W/stderr.txt"
echo "--- $fails Fehler"
exit $((fails > 0))

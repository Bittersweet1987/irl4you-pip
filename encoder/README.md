# irl4you-encoder

Eigener Encoder von IRL4YOU (ersetzt belacoder): mischt die Kameras, kodiert (MPP-Hardware oder Software), regelt die Bitrate nach der
SRT-Verbindung und sendet MPEG-TS per SRT, normalerweise an `irl4you-srtla` auf `127.0.0.1:9100`.

- Schnittstelle (Aufruf, Pipeline-Datei, Steuerbefehle, Statistik): [SCHNITTSTELLE.md](SCHNITTSTELLE.md)
- Aufbau und Regelung: [DESIGN.md](DESIGN.md)

## Bauen und testen (Ubuntu 26.04)

```sh
sudo apt install meson gcc pkgconf libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libjson-glib-dev libsrt-gnutls-dev
meson setup b --prefix=/usr && meson compile -C b
meson test -C b                          # Bitraten-Regelung
tests/laufzeit-test.sh b/irl4you-encoder # braucht srt-tools, gstreamer1.0-plugins-ugly/-bad/-libav
```

Das Paket baut `build/04-encoder.sh` im 26.04-chroot auf der Box.

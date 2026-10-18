# SRTLA auf der Leitung (Arbeitsgrundlage für irl4you-srtla)

Diese Beschreibung ist die einzige Grundlage für `irl4you-srtla` und den Test-Empfänger. Sie hält fest, was zwischen Sender und
SRTLA-Empfänger über UDP läuft, damit der Sender mit fremden SRTLA-Diensten zusammenarbeitet. Quelltext anderer SRTLA-Programme
wurde dafür nicht gelesen (Vorgabe „Eigenständig, nicht kopiert“ im Plan „nur IRL4YOU“). Alle Zahlen sind Big-Endian.

## Grundidee

Der Encoder spricht ganz normales SRT mit `irl4you-srtla` auf dem eigenen Rechner (UDP, z. B. `127.0.0.1:5000`). `irl4you-srtla`
verteilt diese UDP-Pakete auf mehrere Leitungen (eine UDP-Verbindung je Netzwerkkarte, an deren IP-Adresse gebunden) zum
SRTLA-Empfänger. Der Empfänger fasst alle Leitungen einer **Gruppe** zusammen und gibt die SRT-Pakete über eine einzige UDP-Verbindung
an den SRT-Server weiter. Antworten des SRT-Servers kommen über die Leitungen zurück; der Sender gibt sie an den Encoder weiter.

SRT-Pakete werden unverändert durchgereicht. Dazu kommen eigene SRTLA-Pakete. Ein SRTLA-Paket beginnt mit einem 16-Bit-Typ im Bereich
`0x9000`–`0x92FF` (oberstes Bit gesetzt wie bei SRT-Steuerpaketen, aber ein Steuertyp, den SRT nicht benutzt).

## Pakettypen

| Typ | Name hier | Länge | Richtung | Inhalt |
|---|---|---|---|---|
| `0x9000` | Lebenszeichen | 2 (Empfänger antwortet mit derselben Nachricht) | beide | Sender schickt, Empfänger schickt es auf derselben Leitung zurück. Dient als Lebenszeichen und zur Laufzeitmessung. |
| `0x9100` | Empfangsliste | 4 + 4·n | Empfänger → Sender | Nach dem Typ 2 Füllbytes (0), dann n SRT-Folgenummern (32 Bit), die auf **dieser** Leitung angekommen sind. Übliche Empfänger schicken die Liste nach je 10 Datenpaketen einer Leitung. |
| `0x9200` | Gruppe anfragen | 2 + 256 | Sender → Empfänger | 256 Byte Kennung; die erste Hälfte wählt der Sender zufällig. |
| `0x9201` | Gruppe / Leitung anmelden | 2 + 256 | beide | Empfänger → Sender (Antwort auf `0x9200`): vollständige Kennung, erste 128 Byte vom Sender, zweite 128 Byte vom Empfänger. Sender → Empfänger: diese vollständige Kennung, einmal je Leitung, um die Leitung der Gruppe hinzuzufügen. |
| `0x9202` | Leitung angemeldet | 2 | Empfänger → Sender | Antwort auf `0x9201` vom Sender: die Leitung gehört jetzt zur Gruppe. |
| `0x9210` | Anmeldefehler | 2 | Empfänger → Sender | Leitung konnte nicht angemeldet werden (z. B. zu viele Leitungen). |
| `0x9211` | Gruppe unbekannt | 2 | Empfänger → Sender | Antwort auf `0x9201`, wenn der Empfänger die Gruppe nicht (mehr) kennt. Der Sender fängt mit `0x9200` neu an. |
| `0x9212` | Gruppe abgelehnt | 2 | Empfänger → Sender | Antwort auf `0x9200`, wenn der Empfänger keine neue Gruppe anlegt. |

## Ablauf

1. **Gruppe anlegen:** Sender schickt `0x9200` mit neuer Zufallskennung über eine Leitung. Empfänger antwortet `0x9201` mit der
   vollständigen Kennung. Der Sender prüft, dass die ersten 128 Byte seine eigenen sind. Ohne Antwort wiederholt der Sender nach 1 s,
   jeweils über die nächste Leitung.
2. **Leitungen anmelden:** Für jede Leitung schickt der Sender `0x9201` mit der vollständigen Kennung, bis `0x9202` kommt
   (Wiederholung jede Sekunde). Die Leitung zählt beim Empfänger über ihre Absender-Adresse (IP und Port).
3. **Senden:** SRT-Datenpakete (oberstes Bit des ersten Worts 0, die unteren 31 Bit sind die Folgenummer) und SRT-Steuerpakete gehen
   über eine beliebige angemeldete Leitung. Der Empfänger meldet je Leitung per `0x9100`, welche Folgenummern dort angekommen sind.
4. **Antworten:** SRT-Antworten des Servers schickt der Empfänger über die Leitungen der Gruppe zurück. SRT-ACK (Steuertyp 2) kann
   auf **allen** Leitungen gleichzeitig ankommen; der Sender gibt jedes ACK nur einmal an den Encoder weiter. SRT-NAK (Steuertyp 3)
   enthält die verlorenen Folgenummern (Einzelwerte; Bereiche als Anfang mit gesetztem oberstem Bit, danach Ende).
5. **Lebenszeichen:** Der Sender schickt auf jeder angemeldeten Leitung regelmäßig `0x9000`. Kommt auf einer Leitung länger nichts
   zurück, gilt sie als getrennt und wird neu angemeldet (Schritt 2). Antwortet der Empfänger mit `0x9211`, beginnt alles bei Schritt 1.

## Was irl4you-srtla daraus macht (eigene Verfahren, nicht Teil des Protokolls)

- **Laufzeit je Leitung:** aus den Lebenszeichen (höchstens eins unterwegs, alle 200 ms), geglättet wie bei TCP (`srtt`, Schwankung `jitter`).
- **Sendefenster je Leitung:** Pakete unterwegs werden gezählt; `0x9100` gibt sie frei und vergrößert das Fenster, ein NAK für ein
  Paket dieser Leitung verkleinert es (höchstens einmal je Laufzeit). Pakete ohne Rückmeldung verfallen nach 4·srtt (mindestens 1,5 s).
- **Latenz-bewusste Auswahl:** Eine Leitung ist „genutzt“, wenn `srtt + 2·jitter` höchstens um die Toleranz über der besten Leitung
  liegt, sonst „Reserve“ (nur wenn alle genutzten Fenster voll sind).
- **Verteilung „best“:** unter den genutzten Leitungen zuerst die schnellste, bis ihr Fenster voll ist. **„all“:** die genutzte
  Leitung mit dem meisten freien Fensteranteil.
- **Mindestanteil:** Jede genutzte Leitung bekommt mindestens den eingestellten Anteil der Pakete der letzten Sekunde, solange ihr
  Fenster Platz hat.
- **Quell-Routing:** Jede Leitung ist an die IP-Adresse ihrer Netzwerkkarte gebunden (die Routing-Regeln je Quelladresse setzt das
  System), auf Wunsch zusätzlich an die Karte selbst (`SO_BINDTODEVICE`).
- **Läufe:** Aufeinanderfolgende Datenpakete bleiben `--burst` Pakete lang auf derselben Leitung. Leitungen haben verschiedene
  Laufzeiten; jedes einzeln verteilte Paket kommt beim Empfänger als Lücke an, die der SRT-Empfänger meldet (NAK) und die der
  Sender dann unnötig noch einmal schickt. Läufe verringern diese Umordnungs-NAKs (Test `test_bursts_reduce_reordering`).
- **Umordnung statt Verlust:** Meldet der Empfänger per NAK ein Paket, das jünger ist als eine halbe Laufzeit plus zwei Schwankungen
  seiner Leitung (mindestens 10 ms, höchstens 120 ms), zählt es als Umordnung (`reordered`): kein Verlust, kein kleineres Fenster.
  Ein Fensterschnitt bei echtem Verlust ist 10 %. Gruppenanfrage und Anmeldung werden alle 400 ms wiederholt.
- **Verlustgewicht:** Jede Leitung hat einen geglätteten Verlustanteil (`loss_pct`). Im Modus `all` zählt ihr freier Fensteranteil mit 1 − 3·Verlust (mindestens 0,25): verlustreiche Leitungen bekommen weniger Last.

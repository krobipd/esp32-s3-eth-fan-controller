# Verkabelung

> Diese Anleitung erklärt, wie du Standard-PC-Lüfter (4-Pin-PWM, auch 3-Pin) an den
> ESP32-S3-ETH anschließt. Elektrische Angaben sind gegen die **Intel „4-Wire PWM
> Controlled Fans Specification" Rev 1.3** geprüft. **Ohne Gewähr — Verdrahtung auf
> eigenes Risiko.**

## Das Grundprinzip in einem Satz

Der ESP **versorgt die Lüfter nicht** — er liefert nur ein schwaches **PWM-Steuersignal**
(3,3 V) und **liest die Tacho-Drehzahl**. Den Strom liefert ein **separates 12-V-Netzteil**.
Entscheidend ist eine **gemeinsame Masse** zwischen Netzteil und ESP.

## Was du brauchst

- 4-Pin-PWM-Lüfter (3-Pin geht eingeschränkt, siehe unten)
- ein **12-V-Netzteil** passend zur Lüfter-Summe (Dimensionierung unten)
- den ESP32-S3-ETH (per PoE oder USB-C/5 V versorgt)
- Verbindungsdraht; optional kleine Widerstände/Kondensatoren (siehe „Schutz")

## 4-Pin-Lüfter: Pinbelegung

| Pin | Funktion | Intel-Farbe (nur Referenz!) | Wohin |
|----:|----------|----------------------------|-------|
| 1 | **GND** (Masse) | Schwarz | gemeinsame Masse (Netzteil − **und** ESP-GND) |
| 2 | **+12 V** | Gelb | + des 12-V-Netzteils |
| 3 | **Sense / Tacho** | Grün | ESP-GPIO (Tacho-Eingang) |
| 4 | **Control / PWM** | Blau | ESP-GPIO (PWM-Ausgang) |

> ⚠️ **Identifiziere die Pins immer über die POSITION (mechanische Kodiernase des
> Steckers), niemals über die Drahtfarbe.** Die Farben sind in der Intel-Spec nur
> *empfohlen* und werden in der Praxis nicht eingehalten (Noctua nutzt andere Farben,
> manche Lüfter haben nur schwarze Drähte, Industrie-Standard ist oft Schwarz/Rot/Gelb).
> Pin 1 ist am Stecker durch die Kodierung markiert.

Der Stecker ist ein 4-poliger, kodierter Molex-KK-254-Typ (2,54 mm Raster); ein 3-Pin-Lüfter
passt mechanisch auf einen 4-Pin-Anschluss (Pin 4 bleibt dann frei).

## 3-Pin-Lüfter

Pin 1 = GND, Pin 2 = +12 V, Pin 3 = Tacho — **kein PWM-Draht**. Drehzahlregelung wäre nur
über die Versorgungsspannung möglich, was dieser Controller **nicht** macht. An diesem
Controller läuft ein 3-Pin-Lüfter daher **konstant auf voller Drehzahl** (Tacho wird trotzdem
gelesen). Für geregelte Drehzahl 4-Pin-Lüfter verwenden. (OEM-/Fertig-PC-Lüfter, z. B.
mancher Dell, haben proprietäre Belegungen — vor dem Anschluss prüfen.)

## Stromversorgung & Masse

- **12 V kommt aus einem separaten Netzteil**, nie aus dem ESP. Ein ESP-GPIO kann einen
  Lüfter physikalisch nicht speisen (er liefert nur ein Signal, max. ~5 mA).
- 🔌 **Gemeinsame Masse ist Pflicht:** Minus des 12-V-Netzteils **muss** mit einem GND-Pin
  des ESP verbunden sein. Ohne gemeinsame Masse haben PWM- und Tacho-Signal keine
  Bezugsspannung → der Lüfter reagiert nicht / der Tacho liefert Müll.
- Bei mehreren Lüftern: alle GND sternförmig auf die gemeinsame Masse.

### Netzteil dimensionieren
Jeder Lüfter zieht im Dauerbetrieb grob **0,05–0,25 A bei 12 V** (Datenblatt des konkreten
Lüfters beachten; große/Server-Lüfter deutlich mehr). Der **Anlaufstrom** ist kurzzeitig
höher als der Dauerstrom. Faustregel: Summe der Dauerströme + Anlauf-Reserve + Sicherheits­marge.
Beispiel 4 × 0,2 A = 0,8 A → ein 12 V / 2 A-Netzteil ist komfortabel.

## Die zwei Signalleitungen pro Lüfter

### PWM (Pin 4) → ESP-GPIO (Ausgang)
- Der ESP treibt hier 25 kHz PWM (3,3 V, push-pull aus dem LEDC). Das liegt in der
  Spec-Frequenz (Ziel 25 kHz, erlaubt 21–28 kHz; oberhalb des Hörbereichs → kein Pfeifen).
- **Kein externer Pull-up/Pull-down auf die PWM-Leitung** — der nötige Pull-up sitzt
  *im Lüfter*. Ein zusätzlicher würde die Drehzahlkurve verfälschen.
- Signal ist nicht invertiert: **100 % = volle Drehzahl, 0 % = minimal/aus** (modellabhängig,
  siehe Kalibrierung).

### Tacho / Sense (Pin 3) → ESP-GPIO (Eingang)
- Der Tacho ist ein **Open-Collector/Open-Drain-Ausgang** und liefert **2 Pulse pro
  Umdrehung**. Er zieht die Leitung nur gegen GND; den „High"-Pegel macht ein Pull-up.
- Hier übernimmt das der **interne Pull-up des ESP** (die Firmware setzt `INPUT_PULLUP` auf
  3,3 V) → die Leitung schwingt sauber zwischen 3,3 V und 0 V. **Kein externer Pull-up nötig.**
- 🛑 **NIEMALS den Tacho nach 12 V (oder 5 V) hochziehen**, wie es ein PC-Mainboard tut —
  das zerstört den 3,3-V-GPIO sofort. Am ESP ausschließlich den internen 3,3-V-Pull-up nutzen.

## ⚠️ Sicherheits-Caveats (unbedingt lesen)

1. **Nie Versorgungsspannung auf einen Signal-Pin.** 12 V (oder 5 V) auf den PWM- oder
   Tacho-GPIO des ESP zerstört den Pin. Nur Pin 2 führt 12 V — und der geht ans Netzteil,
   nicht an den ESP.
2. **Der ESP32-S3 ist NICHT 5-V-tolerant.** Im aktiven Betrieb treibt der ESP die
   PWM-Leitung selbst → unkritisch. Aber in jedem **hochohmigen Moment** (Boot, Reset, bevor
   PWM initialisiert ist, ESP stromlos) zieht ein Lüfter mit *fan-internem 5-V-Pull-up* die
   PWM-Leitung auf bis zu 5,25 V zurück in den 3,3-V-Pin. Der Strom ist auf ~5 mA begrenzt
   (kein Sofort-Tod, aber auf Dauer unzulässig). **Gegenmittel — eines genügt:**
   - Lüfter mit **3,3-V-PWM-Pull-up** wählen (Intel-Empfehlung für neue Designs), **oder**
   - einen **Serienwiderstand ~100–330 Ω** in die PWM-Leitung legen (begrenzt den
     Rückspeisestrom, billigste Versicherung).
3. **Failsafe bei Leitungsbruch:** Fällt das PWM-Signal weg oder ist Pin 4 nicht
   angeschlossen, läuft ein 4-Pin-Lüfter auf **100 %** (kühlt also weiter). Achtung: das gilt
   nur bei Bruch/stromlosem ESP — wenn der ESP aktiv 0 % treibt, bleibt der Lüfter langsam.

## Optionaler Schutz (empfohlen bei längeren Leitungen)

- **~100–330 Ω in Reihe** in jede PWM- und Tacho-Leitung: schützt den GPIO und begrenzt
  Rückspeise-/Kurzschlussströme.
- **~1 µF von Tacho nach GND** (Noctua-Empfehlung): glättet Störungen/Glitches auf langen
  Tacho-Leitungen. Die Firmware filtert zusätzlich in Software (Glitch-Filter + Median).

## GPIO-Auswahl

Welche GPIOs für PWM/Tacho erlaubt sind (und welche tabu, weil von Ethernet/USB/Boot belegt),
steht in **[HARDWARE.md](HARDWARE.md)**. Pro Lüfter brauchst du **einen** PWM- und **einen**
Tacho-GPIO. Trage die gewählten Pins anschließend im Web-UI ein (→ [USAGE.md](USAGE.md)).

> Hinweis: Dieser Controller nutzt **einen GPIO pro Lüfter** (kein gemeinsamer PWM-Bus). Nur
> wenn man *einen* Ausgang auf *mehrere* Lüfter führen wollte, bräuchte man einen
> Open-Drain-Buffer — das ist hier nicht nötig.

## Schaltbild (ein Lüfter)

```
            12-V-Netzteil
              +12V  ───────────────────────► Pin 2 (+12V, Lüfter)
               GND  ──┬────────────────────► Pin 1 (GND, Lüfter)
                      │
                      └──────────► ESP GND   (GEMEINSAME MASSE — Pflicht!)

   ESP GPIO (PWM-Ausgang) ──[~100–330 Ω]──► Pin 4 (Control/PWM, Lüfter)
   ESP GPIO (Tacho-Eingang) ◄──────────────  Pin 3 (Sense/Tacho, Lüfter)
        (interner 3,3-V-Pull-up der Firmware; KEIN externer 12-V-Pull-up!)
```

Für jeden weiteren Lüfter dasselbe mit eigenem PWM- und Tacho-GPIO; 12 V und GND laufen
parallel vom selben Netzteil (GND sternförmig).

## Nach dem Verdrahten

Lüfter im Web-UI anlegen, Pins zuweisen, und falls er erst ab einer gewissen Leistung
zuverlässig anläuft, den **Mindest-Start** kalibrieren — die Intel-Spec garantiert geregelten
Lauf nur bis herunter zu ~20–30 %, darunter ist das Verhalten lüfterabhängig. Details:
**[USAGE.md](USAGE.md)**.

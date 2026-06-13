# ESP32-S3-ETH Fan Controller — Projekt-Wissensbasis

> Diese Datei ist die zentrale Wissensbasis für das Projekt. Stand: erstellt
> während der Einarbeitungs-Session. Sprache mit dem Nutzer (krobi): **Deutsch**.

## 1. Worum geht es

Ein **netzwerkfähiger Lüfter-Controller** auf Basis des Waveshare **ESP32-S3-ETH**
Boards. Bis zu 8 PWM-Lüfter werden über eine Web-UI und/oder MQTT gesteuert, mit
Drehzahl-Rückmeldung (Tacho/RPM). Das Gerät läuft headless im Netzwerk (Ethernet,
optional PoE) — kein WLAN, kein Bluetooth.

**Aktueller Stand (vom Nutzer):**
- Auf dem Gerät läuft eine **ältere, etwas defekte Firmware**, aber es funktioniert
  „ok" — **MQTT funktioniert und ist wichtig** (nicht kaputt machen).
- Es hat viel Mühe gekostet, das Gerät überhaupt zum Laufen zu bringen.
- **Ziel:** Firmware + UI modernisieren, „endlich richtig gut" machen — ohne den
  laufenden Betrieb (v.a. MQTT) zu brechen.
- **Prioritäten (vom Nutzer):** 1) Zuverlässigkeit/Stabilität, 2) UI-Redesign,
  3) saubere Code-Architektur. Neue Features zunächst zweitrangig.
- **MQTT-Consumer: ioBroker** (kein Home Assistant → kein HA-Discovery nötig). MQTT muss
  zuverlässig funktionieren; Topics/Struktur/DeviceID dürfen geändert werden (ioBroker wird nachgezogen).
- **Flash-Politik:** Zum Testen darf Claude flashen. Kernziel ist eine *bombensichere*
  Web-Oberfläche zum Flashen (OTA muss verlässlich sein). Evtl. GitHub-Veröffentlichung.

> ## ⚠️ HARTE DEPLOYMENT-CONSTRAINT (höchste Priorität)
> Der ESP hängt **per PoE am LAN — KEIN USB**. Ausbau ist wegen der fest
> verkabelten Lüfter **sehr aufwändig**. Daraus folgt absolut bindend:
> 1. **Web-OTA MUSS funktionieren** — es ist der einzige praktikable Update-Weg.
> 2. **Der ESP muss IMMER booten können** — Bricking ist inakzeptabel (keine
>    USB-Rettung verfügbar).
> 3. Jede neue Firmware braucht **Anti-Brick-Schutz**: OTA in die inaktive
>    App-Partition + **automatisches Rollback** (`esp_ota_mark_app_valid_cancel_rollback`
>    / Bootloader-Rollback), **Watchdog aktiv** (nicht deaktivieren!), und einen
>    OTA-Pfad, der NICHT von fragilem JS abhängt (Form/curl-Fallback).
> 4. Erst-Flash der guten Firmware: bevorzugt über den **POST `/ota`-Endpoint** der
>    Altfirmware (Recovery-Hypothese). USB nur als allerletztes Mittel.

### Live-Diagnose des laufenden Geräts (IP 10.47.88.239)
Per read-only HTTP-Probe + Quellcode-Abgleich ermittelt:
- **Laufende Firmware identifiziert: `~/Documents/Arduino/luefter/luefter.ino`**
  (2216 Z.). Match über Header `<h1 style='flex:1'>ESP32-S3-ETH – Fan Controller`
  (Gedankenstrich) + `safe_mode?' (SAFE)'`. Pre-v3.1.
- **Root Cause des Defekts (am Quellcode belegt, KEIN fester Buffer):** `renderRoot`
  (und `/ota`, `/diag`) geben den kompletten Body inkl. großem Inline-`<script>` als
  **EINEN einzigen `c.print(F("..."))`** aus. Die W5500-`Ethernet`-Lib sendet bei
  *einem* großen `write()` nur so viel, wie in den Socket-TX-Puffer passt (~4249 B),
  und **wiederholt den Rest NICHT** → der String wird abgeschnitten. `uiFooter()` ist
  ein *separater* kleiner Print und erscheint deshalb *nach* dem abgeschnittenen
  Script. Pages mit vielen kleinen Prints (`/fans` 3413 B, `/mqtt` 3341 B) sind heil.
  ⇒ **Dashboard, Web-OTA-Seite und Diag sind kaputt**; `/fans`, `/mqtt`, MQTT laufen.
- **Lehre für neue Firmware:** nie einen einzelnen Riesen-`write()`; in Chunks
  ausgeben (v3.3 macht das bereits → sichere Basis) oder Sende-Schleife bis alle
  Bytes raus sind.
- ✅ **Netz-OTA bestätigt (Quellcode-Level):** Der POST-`/ota`-Handler ist intakt
  (`Update.begin/write/end`, Z. ~1515–1565; Router Z. ~1889) und unabhängig von der
  kaputten Upload-Seite. Flashen per
  `curl -X POST --data-binary @fw.bin -H 'Content-Type: application/octet-stream' http://10.47.88.239/ota`
  sollte funktionieren. Trotzdem: erster echter Flash nur mit Freigabe + Anti-Brick.
- **Konfigurierte Lüfter (Stand der Probe):** 4 Stück — `Mac` (PWM40/Tach37),
  `Unifi` (42/35), `USV` (41/36), `NAS` (47/38), je ~50–58 %, ~720–860 RPM, alle OK.
  ioBroker regelt aktiv (z.B. NAS pendelt 56/58 % — vermutlich Temperatur-gesteuert).
- `boot_count=51408` bei ~42 h Uptime, Reset=POWERON, kein safe_mode → hohe Boot-Zahl
  ist historische Crash-Akkumulation, aktueller Boot ist stabil.

## 2. Dateien im Ordner

| Datei / Ordner | Inhalt |
|---|---|
| `fan_controller_v4.0/fan_controller_v4.0.ino` | **Aktueller Entwicklungsstand** (Stufe 1: gehärtet + neues UI). Branch `v4.0-stufe1`. Noch NICHT geflasht. |
| `fan_controller_v4.0/fw_util.h` | Pure, host-getestete Logik (wrap-sichere Zeit, %-Mapping, Namens-Validierung) |
| `fan_controller_v4.0/ui_asset.h` | GENERIERT aus `ui/index.html` via `tools/build_ui.sh` (gzip, PROGMEM) |
| `ui/index.html` | Web-UI (Control-Room, Tabs; gegen `tools/mock_api.py` testbar) |
| `tools/build_ui.sh`, `tools/mock_api.py` | UI-Build + Mock-API für gerätelose Entwicklung |
| `tests/host/test_logic.cpp` | Host-Unit-Tests für `fw_util.h` |
| `docs/superpowers/specs/…`, `docs/superpowers/plans/…`, `docs/flash-checklist.md` | Spec, Implementierungsplan, Flash-Checkliste |
| `fan_controller_v3.3/fan_controller_v3.3.ino` | Referenz-Basis (1811 Z.), Vorgänger von v4.0 |
| `ESP32_FanController_v3.1_FINAL/...ino` | Älterer Stand (2049 Z.); v3.3 ist die bereinigte Weiterentwicklung |
| `ESP32-S3-ETH-details-15.jpg` | Pinout-Diagramm des Boards (sehr nützlich) |
| `ESP32-S3-ETH-Schematic.pdf` | Schaltplan |
| `W5500_ds_v110e.pdf` | Datenblatt Ethernet-Chip |
| `Esp32-s3_datasheet_en.pdf` | Chip-Datenblatt |
| `Esp32-s3_technical_reference_manual_en.pdf` | Technical Reference Manual (~14 MB) |

`.ino`-Dateien müssen in einem **gleichnamigen Ordner** liegen (Arduino-Konvention).

### Weitere Code-Stände im Arduino-Sketchbook (`~/Documents/Arduino/`)
| Sketch | Z. | Rolle |
|---|---|---|
| `luefter/luefter.ino` | 2216 | **= die aktuell laufende Firmware** (pre-v3.1, der Truncation-Bug) |
| `luefter-eth/luefter-eth.ino` | 1451 | älterer Stand (`<h1>ESP32 Fan Controller`, kein SAFE-Mode) |
| `claude/claude.ino` | 2661 | separater „v2.0"-Zweig (`🌀 ESP32-S3 Fan Controller`), experimentell |

Chronologie/Beziehung der Stände ist nicht 100 % sicher; **`luefter.ino` = Gerät**,
**`fan_controller_v3.3` = beste/sauberste Basis** für die Weiterentwicklung.

## 3. Hardware: Waveshare ESP32-S3-ETH

- ESP32-S3 (Dual-Core Xtensa LX7, 32-bit) + **W5500 Ethernet via SPI** + PoE.
- Produktseite: https://www.waveshare.com/esp32-s3-eth.htm

### Belegte / blockierte Pins (NICHT für Lüfter verwenden)
| Funktion | GPIO |
|---|---|
| W5500: MOSI / MISO / SCLK / CS / INT / RST | 11 / 12 / 13 / 14 / 10 / 9 |
| USB (D+ / D−) | 19 / 20 |
| UART0 RX / TX | 44 / 43 |
| Strapping/Boot | 0, 3, 45, 46 |
| (intern: SPI-Flash/PSRAM) | 22–32 nicht herausgeführt |

Code-`#define`s decken sich exakt mit dem Pinout-Bild. ETH_INT (GPIO10) wird im
Code nicht aktiv genutzt (Polling statt IRQ), ist aber als belegt geblockt.

### Für Lüfter erlaubte GPIOs (PWM_ALLOWED / TACH_ALLOWED)
`1, 2, 4, 5, 6, 7, 8, 15, 16, 17, 18, 21, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 47, 48`

⚠️ **Gotcha:** GPIO 4/5/6/7 sind laut Board die **SD-Karten-Pins** (CS/MISO/MOSI/CLK),
stehen aber trotzdem in der erlaubten Liste. Solange keine SD-Karte genutzt wird,
unproblematisch — sonst Konflikt.

🔴 **Gotcha 2 (✅ᴠ silicon-verifiziert):** GPIO **33–37** = Octal-MSPI-Bus (D4–D7+DQS).
Nur nutzbar, solange `PSRAM=disabled` gebaut wird (so läuft das Gerät; Tachos hängen
auf 35/36/37!). Mit `PSRAM=opi` wären sie weg → PSRAM bleibt dauerhaft disabled (§4).

### Chip-Variante & Peripherie (Schaltplan + ESP-IDF `soc_caps.h`, verifiziert)
**Bestückung (Schaltplan `ESP32-S3-ETH-Schematic.pdf`):**
- MCU **`ESP32-S3R8`** → **8 MB Octal-PSRAM (OPI, 1,8 V)** integriert; Haupt-Quarz 40 MHz (X1).
- Flash extern **`W25Q128` = 16 MB** (Quad-SPI, U4), NICHT 4 MB.
- W5500 (U7): eigener gefilterter Analog-Rail `VCC3.3_A` über Ferrit L8 (600R) + dicke
  Entkopplung; **keine Serien-Rs auf den SPI-Leitungen**; eigener **25-MHz-Quarz (X2)**;
  Magnetics T1 H1102NLT + Bob-Smith-Termination an der RJ45.
- 3,3 V über Buck **JW5060** (2,2 µH); **PoE über ein EXTERNES Modul** am Header P2
  (POE扩展口) → speist POE_5V (PoE nicht voll integriert → Power-Qualität hängt am Modul).
- Onboard zusätzlich: Kamera **OV2640** (DVP teilt sich Pins mit Lüfter-GPIOs), SD-Karte
  (GPIO4/5/6/7), WS2812B-RGB-LED, USB-C (19/20), zwei PICO-Header. Reset: EN 10K+1µF, GPIO0=Boot.

**S3-Peripherie-Grenzen (`soc_caps.h`, esp32s3):**
- **LEDC (PWM): 8 Channels, 4 Timer, max 14-bit** Timer-Breite; bei 25 kHz aus 80-MHz-APB
  praktisch **~11-bit** max. ⇒ 8 Lüfter = exakt 8 Channels (Firmware nutzt 8-bit).
- **PCNT: genau 4 Units** (1 Gruppe × 4, je 2 Channels) → max 4 Lüfter HW-gezählt, Rest per
  ISR; HW-Glitch-Filter pro GPIO vorhanden.
- **2 CPU-Cores** (LX7) — Firmware nutzt nur einen; GDMA vorhanden.
- **3 SPI-Peripherals** (SPI0/1 = Flash/PSRAM, SPI2/3 = GPSPI für W5500), DMA-fähig. 49 GPIOs (0–48).

## 4. Build & Flash

- **Arduino IDE 2.3.7**; **ESP32 Arduino Core 3.3.8 installiert** (Firmware-Header nennt 3.3.5).
- Board-Setting **„USB CDC On Boot: Enabled"** ist PFLICHT (für Serial-Log).
- Libraries: `Ethernet`, `PubSubClient`, `Preferences`, `Update`, `SPI` + ESP-IDF
  (`driver/pcnt`, `esp_wifi`, `esp_bt`, `esp_task_wdt`).
- OTA-Update ist auch über die Web-UI möglich (`.bin` hochladen unter `/ota`).
- **Arduino „autoproto" Fix:** ganz oben werden `enum FanFault`, `struct Fan`,
  `struct ApplyJob` forward-deklariert, sonst generiert die IDE kaputte Prototypen.
- **Partition-Scheme:** muss OTA-fähig sein (zwei App-Slots app0/app1). Altfirmware
  hat einen `/ota`-Handler → vermutlich bereits OTA-Schema aktiv. Beim Build prüfen.

### Umgebungs-Status (dieser Mac, Stand Einarbeitung)
- **Kein USB zum ESP** — Gerät hängt per **PoE am LAN** und ist von hier nur über's
  **Netzwerk** erreichbar (`10.47.88.239`, per Ping bestätigt). Flashen daher
  ausschließlich über Netz-OTA (siehe Deployment-Constraint).
- **Toolchain VORHANDEN** (über Arduino IDE 2.x):
  - `arduino-cli` mitgeliefert:
    `"/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"`
    (Version 1.5.0). Praktisch: Alias/Variable setzen, dann normal nutzbar.
  - **ESP32-Core 3.3.8 installiert** (`~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.8`)
    — Firmware-Header nennt 3.3.5, real ist 3.3.8 (kleine Abweichung, sollte bauen).
  - Libraries vorhanden: `Ethernet`, `Ethernet_Generic`, `EthernetWebServer`,
    `PubSubClient`. Sketchbook: `~/Documents/Arduino/`.
  - `esptool` separat nicht nötig (kein USB); `python3` vorhanden.
  - ✅ **FQBN VERIFIZIERT (Build läuft):** `esp32:esp32:esp32s3:CDCOnBoot=cdc`
    (Default-`PartitionScheme=default` = OTA-fähig, app0/app1 je 1,25 MB; FlashSize 4M;
    USBMode hwcdc — egal, kein USB). Build-Befehl:
    ```sh
    CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
    "$CLI" compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc --output-dir /tmp/out "<sketch-ordner>"
    ```
  - ✅ **Beide Firmwares kompilieren sauber** (Core 3.3.8, EXIT=0):
    `fan_controller_v3.3` → 491.857 B (37 % von 1.310.720); `luefter.ino` → 497.173 B (37 %).
    Viel OTA-Headroom (~800 KB frei im Slot). Gezogene Libs: **`Ethernet` v2.0.2**
    (Arduino-offiziell, W5500 via `utility/w5100.cpp`) + `PubSubClient` aus
    `~/Documents/Arduino/libraries`; `Update`/`Preferences`/`SPI` aus dem Core.
  - 🔴 **PSRAM MUSS `disabled` bleiben** (Korrektur 10.06.2026, ✅ᴠ silicon-verifiziert in
    `soc/spi_pins.h` des Core 3.3.8): Octal-PSRAM belegt den MSPI-Bus **GPIO33–37**
    (`MSPI_IOMUX_PIN_NUM_D4..D7`=33..36, `DQS`=37). Das kollidiert mit den **fest
    verdrahteten Tacho-Pins 35/36/37** (Unifi/USV/Mac) → `PSRAM=opi` würde 3 von 4
    RPM-Signalen töten. Waveshare-FAQ bestätigt: GPIO33–37 auf diesem Board nicht
    nutzbar. 512 KB internes SRAM reichen für den Fan-Controller locker.
    (Meine frühere Empfehlung `PSRAM=opi` war FALSCH — Board hat den PSRAM zwar
    bestückt, aber seine Pins sind durch die Lüfter-Verkabelung verbraucht.)
  - **FlashSize/FlashMode sind nur für einen USB-Erstflash relevant** (den es nicht gibt):
    Bei OTA-App-Images bestimmt der bereits geflashte **Bootloader-Header** den Flash-Modus;
    OTA fasst Bootloader + Partitionstabelle nie an. ⇒ Weiter mit Default-FQBN bauen ist ok,
    solange das Image in den App-Slot passt. OTA-fähige 16M-Schemes (nur falls je USB):
    `app3M_fat9M_16MB` (im Menü) oder `default_16MB.csv` (2×6,25 MB App, nicht im
    esp32s3-Menü, via `--build-property build.partitions=default_16MB`).
  - ⚠️ **Vor einem echten OTA-Flash noch offen:** reale Partitions-Tabelle DES GERÄTS
    (OTA schreibt nur den App-Slot, nie die Tabelle → Image muss in den Slot passen).

## 5. Firmware-Architektur (v3.3)

Single-Threaded, kooperativ: alles läuft in `loop()` (kein FreeRTOS-Task, zweiter
Core ungenutzt). Pro Durchlauf: Apply-Queue → Duty-Queue → Ethernet/Link-Watch →
HTTP → MQTT → Storm-Recovery → RPM-Messung → Persistenz. `delay(2)` am Ende.

### Subsysteme
- **Lüfter-Modell** (`struct Fan`, max 8): PWM 25 kHz/8-bit, Tacho 2 Pulse/U.
- **RPM-Messung 2-stufig:** erste 4 Lüfter über Hardware-**PCNT** (Pulse Counter,
  mit Glitch-Filter), Rest über **ISR** (`tachIsr0..7`, Glitch-Filter via minPulseUs).
  Glättung: Median-of-3 → EMA (α=0.25). „Burst"-Modus (schnelleres Sampling) nach
  Duty-Änderung.
- **Web-UI:** komplett handgebautes HTTP (kein Framework), server-seitig
  zusammengesetzte HTML-Strings, dunkles Design. Seiten: `/` (Live-Dashboard),
  `/fans`, `/fans/edit`, `/fans/calib`, `/mqtt`, `/ota`, `/diag`.
  JSON-Status unter `/api/status`, Logs unter `/log.txt` & `/prevlog.txt`.
- **MQTT** (PubSubClient), DeviceID `ws-s3eth-XXXXXX` (aus MAC), Auto-Reconnect.
  **Wichtig (per Code-Diff belegt): laufende Firmware und v3.3 nutzen UNTERSCHIEDLICHE
  Topics + Skala.** Das ist KEIN „Brechen", sondern eine bewusste Wahl beim Umstieg —
  entweder Firmware spricht die alten Topics, oder ioBroker wird umkonfiguriert. Fakten:
  - **luefter.ino (läuft):** `<prefix>/<fan>/pwm` (0..255, retained) · `<prefix>/<fan>/rpm`
    · Befehl `<prefix>/<fan>/cmd/pwm` (0..255) · Status `<prefix>/<deviceId>/status`.
  - **v3.3 (saubere, standardnähere Struktur):** `<prefix>/<deviceId>/fan/<fan>/pct`
    (0..100, retained) · `…/rpm` · Befehl `…/set` (0..100) · `<prefix>/<deviceId>/status`.
  - Deltas: Pfad flach↔verschachtelt · `/cmd/pwm`↔`/set` · `/pwm`↔`/pct` · **0..255↔0..100**.
  - **Wessen Entscheidung:** allein die des Nutzers. Kein externer Zwang; das „stabil
    halten" war eine (Über-)Interpretation von „MQTT ist wichtig", nicht eine Vorgabe.
- **OTA** über `/ota` (Update-Lib), danach Reboot.
- **Persistenz (NVS/Preferences):** Namespaces `fans` (Konfig), `state` (Duties,
  debounced), `mqtt` (Broker-Konfig), `sys` (Boot-Count, Crash-Streak, Log-Tail).
- **Robustheit:**
  - Crash-Loop-Erkennung → Safe-Mode (`crash_streak >= 3`).
  - **Storm-Shield:** zu viele Pulse → Lüfter-Messung pausieren + Cooldown.
  - W5500-Auto-Reset bei Link-Verlust > 15 s.
  - Watchdog wird in `setup()` deaktiviert (`esp_task_wdt_deinit`).
  - WiFi/BT werden aktiv abgeschaltet (`disableRadios`).
- **Entkopplung:** HTTP/MQTT schreiben nie direkt Hardware, sondern füllen Queues
  (`g_pendingDuty`, `g_apply`), die in `loop()` abgearbeitet werden. `g_stateRev`
  wird nur bei Strukturänderungen erhöht (UI-Polling nutzt das als Cache-Key).

### Im Code dokumentierte Fixes ggü. v3.1 (Marker im Code)
`[B1]` Duty-Restore nach Reboot · `[B2]` Buffer-Overflow-Fix beim Body-Lesen ·
`[B3]` rev nur bei Strukturänderung · `[B5]/[B6]` Dead-Code entfernt ·
`[B7]` W5500-Init · `[B8]` Slot-Leak-Fix bei „Neuer Lüfter" abgebrochen ·
`[F1]` XSS-`esc()` im UI · `[F2]` data-fan · `[F3]` Offline-Banner.

## 6. Bekannte Schwachstellen / Modernisierungs-Kandidaten

(Erste Einschätzung beim Lesen von v3.3 — noch nicht mit dem Nutzer priorisiert.)

1. **Blockierendes HTTP im einzigen Thread:** `readLine` mit Timeout bis 15 s pro
   Zeile; ein langsamer/böser Client kann das gesamte Gerät (inkl. MQTT, RPM)
   stallen. Kandidat: async/Task-getrennte Netzwerkbehandlung, kürzere Timeouts.
2. **Keine Authentifizierung** auf Web-UI/OTA — jeder im LAN kann flashen/steuern.
3. **Zweiter Core / FreeRTOS ungenutzt** — Netzwerk vom Steuer-/Messloop trennen.
4. **`delay()` im W5500-Reset** (~250 ms) blockiert den Loop.
5. **UI** ist server-gerendert String-Konkatenation — funktional, aber für „richtig
   gut" evtl. saubere Trennung (statische Assets / kleine SPA / SSE statt Polling).
6. **MQTT-Passwort** im Klartext im NVS und im UI-Feld sichtbar.
7. **Consumer ist ioBroker** (kein HA) → kein HA-Discovery nötig. MQTT muss zuverlässig
   laufen; Topics/Struktur dürfen geändert werden (ioBroker wird nachgezogen).
8. **Watchdog deaktiviert** statt richtig bedient — bei einem Hänger kein Auto-Reboot.
   ⚠️ Wegen der No-USB-Constraint ist das ein KO-Kriterium: Watchdog MUSS aktiv sein.
9. **millis()-Wrap-Bugs (49,7 d):** Storm-Shield (Z.662/671) und `readLine`-Timeout (Z.960f)
   nutzen das unsichere `millis() < deadline`-Muster → am Wrap Lüfter-Mess-Pause bis ~49 d
   bzw. nie feuernder Timeout (Details §12F). Fix: Delta-Arithmetik.

### Code-Audit v3.3 (10.06.2026, kompletter Read; Lib-Befunde lokal verifiziert)
10. 🔴 **Dritter millis-Wrap-Bug, trifft MQTT:** `g_nextMqttTryMs = now + backoff` +
    `if (now < g_nextMqttTryMs) return;` (Z.794/811/817/832). Wrappt millis nach gesetztem
    Deadline → Reconnect bis ~49 d blockiert = MQTT dauerhaft tot bis Reboot.
11. 🔴 **`/log.txt` hat den Altfirmware-Truncation-Bug AUCH in v3.3:** `c.print(gLogBuf)`
    = EIN write() bis 8 KB (LOG_MAX). Lib-Level bewiesen: `socketSend()` kappt auf SSIZE
    (4 KB, socket.cpp Z.428-432), `EthernetClient::write()` **gibt trotzdem `size` zurück**
    (EthernetClient.cpp Z.83-89) → stille Truncation ab ~4 KB Log. (`/prevlog.txt` ≤1,6 KB ok.
    Damit ist auch die Altfirmware-Diagnose §1 lib-seitig bestätigt.) Fix: chunked senden.
12. 🔴 **NVS-Wear durch `persistLogTail()`:** schreibt bei Log-Aktivität alle 8 s einen
    ~1,6-KB-String (≈51 NVS-Entries) in die nur 20-KB-NVS-Partition (5 Pages à 126 Entries).
    Dashboard offen = jeder 1,2-s-Poll erzeugt eine Log-Zeile (`LOGI("HTTP",…)` loggt JEDEN
    Request) → Dauerschreiben: ~10.800 Writes/Tag ≈ 4.400 Page-Erases/Tag ⇒ **NVS-100k-Limit
    nach grob ~110 Tagen Dauer-Dashboard** erreichbar. ioBroker-set-Kommandos loggen ebenfalls.
    Fix: Log-Tail nur bei Crash/Reboot persistieren (Shutdown-Handler), nicht periodisch;
    HTTP-Polling nicht loggen.
13. 🟠 **DHCP blockiert bis 60 s** (`Ethernet.begin(mac)` Default-Timeout 60000 ms, Ethernet.h
    Z.82) — in setup() UND im Retry-Pfad der loop(). Setup-Reihenfolge: Duties werden erst
    in der ersten loop()-Iteration angewendet ⇒ **nach Reboot bei Netzausfall laufen alle
    Lüfter ~60+ s mit 0 %**. Fix: `dutyProcessQueue()` direkt nach `restoreSavedDuties()` in
    setup() + `Ethernet.begin(mac, 5000, 2000)`.
14. 🟠 **Safe-Mode ist wirkungslos:** `crash_streak>=3` setzt nur Flag/NVS/UI-Badge —
    kein Verhalten ändert sich (kein Consumer im Code). Entweder echte Degradation
    (z.B. Netz-only, Fans safe-duty) oder Anzeige nicht „SAFE" nennen.
15. 🟠 **HTTP-DoS-Flächen (ohne Auth, §6.2):** a) `readHeaders` unbegrenzt (Header-Anzahl/
    Gesamtzeit; 15 s/Zeile slow-loris → Loop+MQTT stehen), b) `handleBodyToString` reserviert
    Content-Length ungeprüft (16-MB-Header → OOM-Crash), c) OTA-Trickle hält Loop bis 5 min/
    Stall. Fix: Body-Cap (~4 KB für Forms), Header-Limit, Gesamt-Request-Budget.
16. 🟡 **Stored XSS in server-gerenderten Seiten:** `f.name`/`calNote` werden in `/fans`,
    `/fans/edit` (value='…'), `/fans/calib` (textarea) UNescaped gedruckt — [F1]-esc()
    schützt nur das JS-Dashboard. Name/Note sind frei wählbar (nur Länge geprüft).
17. 🟡 Kleineres: PCNT nutzt Legacy-Treiber (`driver/pcnt.h`, deprecated in IDF5);
    Fan-Namen ohne Uniqueness → `sanitizeName`-Kollision = MQTT-Topic-Clash; beabsichtigter
    Reboot publisht kein „offline" (retained „online" bleibt bis Broker-Timeout); `/fans/new`
    ändert Zustand per GET; String-Heavy Logging/HTTP = Heap-Churn (→ §12F largest_free_block
    in /api/status aufnehmen); JSON liefert min_free_heap, aber nicht largest_block.

## 12. ESP32-S3 ⇄ W5500: Fallstricke & Best Practices (Deep-Research + verifiziert)

> Quelle: Deep-Research-Lauf (105 Agenten, 23 Quellen, 112 Claims). Der adversariale
> Verify-Pass des Laufs brach am Token-Limit ab → **nachträglich in dieser Session von
> Hand gegen Primärquellen UND den lokal installierten Core 3.3.8 verifiziert.**
> Legende: **✅ᴠ** = von mir hier gegen Primärquelle/lokalen Core geprüft (höchste
> Konfidenz, oft autoritativer als der Web-Research) · ✅ = bestätigt · 🟡 = Symptom
> belegt, aber mit Caveat · ⚠️ = Research-Behauptung **korrigiert** · ❌ = widerlegt.

### A. Hardware / SPI-Stabilität (W5500-over-SPI)
- ✅ᴠ **Pinmap** (Waveshare-Wiki): MISO=12, MOSI=11, SCLK=13, CS=14, RST=9, INT=10
  ⇒ **deckt sich exakt mit unserem Code** (§3). Kein Handlungsbedarf.
- ✅ᴠ **W5500-Datenblatt-Fakten (v1.1.0):** SPI **Mode 0 oder 3**, MSB-first, **bis 80 MHz**
  fähig (realer Wert signalintegritäts-begrenzt; Board fährt 20 MHz). **RSTn muss ≥ 500 µs
  low** sein — die Firmware (50 ms) erfüllt das weit. **PMODE[2:0] alle Pull-up → Default
  Auto-Negotiation an** (All-capable, 10/100). 8 HW-Sockets, 32 KB TX/RX-Buffer,
  **5V-tolerante I/Os**. Board-Beschaltung korrekt (Schaltplan-Match): EXRES1 12,4k,
  TOCAP 4,7µF, 1V2O 10nF, 25-MHz-Quarz.
- ✅ᴠ **Hohe SPI-Takte machen den W5500 real instabil** (Symptome bestätigt):
  - 36 MHz → `W5500 version mismatched, expected 0x04, got 0xa0/0x00`, klappt erst
    nach mehreren Reboots (esp-idf #14257).
  - 10 MHz (nativer Treiber) → `w5500.mac: received frame was truncated`,
    `invalid frame length`, TWDT-Timeout auf `w5500_tsk` (arduino-esp32 #11754 —
    Achtung: dort als *„Unable to reproduce"* geschlossen).
- ⚠️ **KORREKTUR meiner ersten Notiz:** „Maintainer empfehlen 8 MHz" steht **NICHT**
  in #14257 (dort kein Maintainer-Statement, „Won't Do" geschlossen). Realbild aus
  Querschnitt (WIZnet-DB, Foren): W5500 kann bis 80 MHz, **praktisch stabil meist
  20–26 MHz** bei sauberer Verdrahtung; **8 MHz ist der dokumentierte FALLBACK, *wenn*
  intermittierende Fehler auftreten** — keine pauschale Default-Empfehlung.
- ✅ᴠ `emac_w5500_transmit()` **hängt nach Kabel-Replug** in einer Polling-Schleife auf
  dem `W5500_SIR_SEND`-Bit endlos → Watchdog feuert in `w5500_read()` → „happens 100%
  every time" (esp-idf #6233, intern gefixt; Workaround = Loop-Counter-Limit). **Sehr
  relevant** — ein SPI-Hang MUSS per Watchdog zum Reboot führen (→ §6 Punkt 8!).

> **⇒ Revidierte, ehrliche Einschätzung zum SPI-Takt (war von mir zu scharf):** Beide
> Firmwares fahren 20 MHz (`SPI.setFrequency(20000000)`: luefter.ino Z.517+2013;
> v3.3 Z.434). **20 MHz liegt INNERHALB des üblichen Stabilbereichs**, und der Waveshare
> hat den W5500 **fest auf der Platine** (keine fliegende Jumper-Verdrahtung wie in den
> Fehler-Issues) → bessere Signalintegrität → 20 MHz ist hier **plausibel ok** (passt
> dazu, dass das Gerät stabil läuft). **⇒ NICHT proaktiv ändern.** 8 MHz ist der **erste
> Hebel, FALLS** je W5500-Instabilität auftritt (truncation/Disconnects/link-flapping).
> **Gut:** Reset-Sequenz (RST LOW 50 ms → HIGH → 200 ms) ist bereits sauber/großzügig.

### B. Treiber-/Lib-Stack-Wahl
- ✅ᴠ **Nativer `ETH.h`-Treiber** ist Espressifs offizieller Weg (`#include <ETH.h>`,
  `ETH_PHY_W5500`), **nicht** die Arduino-`Ethernet`-Lib. Signatur **lokal in 3.3.8
  verifiziert** (`.../libraries/Ethernet/examples/ETH_W5500_Arduino_SPI`):
  `SPI.begin(SCK,MISO,MOSI); ETH.begin(ETH_PHY_W5500, addr, CS, IRQ, RST, SPI, spi_freq_mhz);`
  — **`spi_freq_mhz` ist optionaler Parameter, Default = 20** (`ETH_PHY_SPI_FREQ_MHZ`,
  ETH.h Z.127). 8 MHz wäre also schlicht `ETH.begin(..., SPI, 8)`. ⚠️ Die Pins im
  Beispiel (CS15/IRQ4/RST5…) sind NICHT unsere — unsere Board-Pins einsetzen.
- ✅ᴠ **Async Link-Erkennung** via `Network.onEvent(cb)` mit
  `ARDUINO_EVENT_ETH_CONNECTED/GOT_IP/LOST_IP/DISCONNECTED` (lokal verifiziert) —
  robuster als unser jetziges blockierendes Polling im `loop()`.
- 🟡 `Ethernet.h` (Wiznet-Lib) **+ `ArduinoOTA`** crasht auf S3+W5500 mit
  lwIP-Assertion `tcpip_send_msg_wait_sem … Invalid mbox` (arduino-esp32 #9648; Ursache
  von Maintainern *nicht* bestätigt, „awaiting triage"). **Für uns vermutlich irrelevant:
  wir nutzen `Ethernet.h`, aber NICHT `ArduinoOTA`** (unser OTA läuft über die `Update`-
  Lib per HTTP-POST `/ota`). Trotzdem: Stacks nie mischen.
- ❌ „Waveshare nutzt `ETHClass`/`ETH.beginSPI()`" — veraltet; kanonisch ist `ETH.begin()`.

### C. OTA / Rollback — Mechanik für Core 3.3.8 (lokal im Core-Quellcode verifiziert)
- ✅ᴠ **sdkconfig (esp32s3/3.3.8) lokal geprüft:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`,
  `CONFIG_APP_ROLLBACK_ENABLE=y`, `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` *nicht* gesetzt
  (gut — Anti-Rollback würde Downgrades blocken), Watchdogs alle an
  (BOOTLOADER_WDT 9 s, INT_WDT 300 ms, TASK_WDT 5 s + PANIC). Braucht 2 OTA-Slots + `otadata`.
- ✅ᴠ **Mechanik (aus `cores/esp32/esp32-hal-misc.c`, `initArduino()`):** Bei einem frisch
  per OTA geschriebenen Image ist der Partition-State `ESP_OTA_IMG_PENDING_VERIFY`. Der
  Core ruft beim Boot:
  ```c
  if (!verifyRollbackLater()) {                 // default false → Block läuft
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      if (verifyOta()) esp_ota_mark_app_valid_cancel_rollback();   // valid → kein Rollback
      else             esp_ota_mark_app_invalid_rollback_and_reboot(); // → Rollback
    }
  }
  ```
- ✅ᴠ **TIMING (kritisch, lokal in `main.cpp` verifiziert):** `app_main()` → `initArduino()`
  (Z.112) → erst DANACH wird der `loopTask` erzeugt, der `setup()` (Z.67) und `loop()`
  (Z.82) ausführt. ⇒ Der Rollback-Check oben läuft **VOR `setup()`** — zu diesem Zeitpunkt
  sind **Ethernet/MQTT/Lüfter garantiert noch NICHT initialisiert.**
- 🔑 **Daraus folgt die Hook-Wahl (Hook hängt davon ab, WANN das Health-Signal verfügbar ist):**
  - **`verifyOta()`** (Default `return true`) wird *innerhalb* `initArduino()` aufgerufen →
    taugt NUR für Checks, die **schon im frühen Boot** berechenbar sind (NVS lesbar,
    Partition heil, Crash-Streak). **NICHT** für Netz/MQTT/Lüfter — die sind noch unten,
    ein solcher Check gäbe immer `false` → **jedes frische Image würde sofort
    zurückgerollt, OTA „greift nie".** (Das war mein erster Fehler — korrigiert.)
  - **`verifyRollbackLater()`** (Default `false`) ⇒ **für uns der richtige Weg.** Auf
    `true` setzen → der Boot-Check wird übersprungen, Image bleibt `PENDING_VERIFY`.
    DANN aus unserem eigenen Code **nach einem gesunden Lauf-Fenster** (Link up + MQTT
    verbunden + Lüfter messen, über N s stabil) selbst `esp_ota_mark_app_valid_cancel_rollback()`
    rufen. Crasht/hängt die FW vorher → nie valid markiert → Bootloader revertet beim
    (WDT-erzwungenen) Reboot. **Das ist die Anti-Brick-Garantie.** ⇒ Das Research-Rezept
    aus #7422 war hierfür **doch korrekt**; meine „veraltet"-Notiz war falsch.
  - Override **muss `extern "C"`** sein (weak C-Symbole in `.c`). **Konkretes Rezept für uns:**
    ```cpp
    extern "C" bool verifyRollbackLater() { return true; }  // Boot-Check aufschieben
    // ... in loop(), sobald Health-Fenster N s erfüllt und Image noch PENDING_VERIFY:
    //   esp_ota_mark_app_valid_cancel_rollback();
    ```
  - Bestätigt: **ohne irgendeinen Override wird ein Image automatisch valid** (weil
    `verifyOta()` default `true`) → das ist „notwendig ≠ hinreichend" aus §11.
  - 🔑 **v4.0-Umsetzung (13.06., NICHT „wegreparieren"!):** Das 90-s-Health-Fenster ist
    NICHT die einzige Stelle, die valid markiert. `commitIfPending()` markiert das Image
    ebenfalls bei einem **bewussten Reboot/OTA aus dem laufenden Image** (Anfang
    `handleOTA` vor `Update.begin`, sowie `prepareRestart`/`apiReboot`). Grund: sonst rollt
    ein Reboot/zweites OTA *innerhalb* der 90 s auf die alte FW zurück, und ein OTA im
    Fenster ruft `set_boot_partition()` aus `PENDING_VERIFY` heraus (unklare Ecke). Das ist
    sicher, weil ein nicht-bootendes/hängendes Image diese Pfade nie erreicht — **solange
    der Safe-Mode HTTP+OTA am Leben hält** (§6/Spec §3.7). Restrisiko: unkommandierter
    Power-Blip/Brownout im 90-s-Fenster rollt zurück (in Flash-Checkliste dokumentiert).
- 🟡 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` ist Bootloader-Compile-Zeit-Option (PlatformIO
  `build_flags` wirken nicht) — für uns irrelevant: Arduino-Core-Bootloader hat es schon.

### D. Referenz-Projekte zum Lernen (verifiziert)
- ⚠️ **`KlausMu/esp32-fan-controller`** — korrigiert: generischer ESP32, **WiFi-only (KEIN
  Ethernet/W5500)**, **EIN** 4-Pin-PWM-Lüfter + Tacho, PlatformIO, MQTT + OTA, dazu
  Fan-/Climate-Mode, TFT, BME280, HA/openHAB-Discovery (~430★). ⇒ gute Referenz für
  **Lüfter/Tacho/MQTT/OTA-Muster**, NICHT für Ethernet und nicht für Multi-Fan.
- ✅ᴠ **`jozala/ESP32_W5500_MQTT`** — plain ESP32 (nicht S3) + W5500 + MQTT (Arduino-
  Ethernet-Lib); dokumentiert **W5500-spezifische intermittierende Disconnects** (nicht
  über WiFi), nicht-deterministisch Minuten–Stunden, **ungelöst** (verweist auf
  Stoffregen-Ethernet #39). ⇒ Beleg, dass W5500-Disconnects ein bekanntes Stack-/SPI-
  Phänomen sind, kein App-Bug.
- ✅ᴠ Espressif **`ETH_W5500_Arduino_SPI.ino`** (liegt lokal im Core) — kanonisches
  nativer-Treiber-Beispiel. Weitere (ungeprüft, blog): mischianti.org, blog.hirnschall.net.

### E. Aktiv widerlegt
- ❌ „arduino-esp32 **2.0.7** brach W5500/WiFi" (0-2) — und ohnehin irrelevant: wir sind 3.3.8.

### F. 24/7-Dauerbetrieb (Research 10.06.2026; Verify-Pass am Session-Limit abgebrochen → kritische Claims von Hand lokal verifiziert)
- ✅ᴠ **Watchdog-Rezept Core 3.3.8** (sdkconfig + `esp_task_wdt.h` + `esp32-hal-misc.c` lokal geprüft):
  TWDT läuft ab Boot (**5 s, `TASK_WDT_PANIC=y` → Timeout = Reboot**, überwacht nur Idle-Task
  CPU0; Idle-CPU1 NICHT). Der `loopTask` ist **nicht** auto-abonniert → `enableLoopWDT()`
  (Core-API, macht `esp_task_wdt_add(loopTaskHandle)`) + regelmäßig `feedLoopWDT()` bzw.
  yielden. Neue 3.x-API: `esp_task_wdt_init/reconfigure(const esp_task_wdt_config_t*)` mit
  `{timeout_ms, idle_core_mask, trigger_panic}` (2.x-Signatur kompiliert nicht mehr).
  **Das `esp_task_wdt_deinit()` der aktuellen Firmware schaltet diesen Schutz ab — in der
  neuen FW ersatzlos streichen und stattdessen Loop-Task abonnieren** (Anti-Brick, §6 Pkt 8).
- ✅ᴠ **millis()** = `esp_timer_get_time()/1000` auf `unsigned long` (32 bit) gecastet →
  **Wrap nach 49,7 Tagen**. Immer Delta-Arithmetik `(uint32_t)(now - last) >= intervall`
  nutzen, nie absolute Vergleiche (v3.3 stellenweise prüfen!).
- ✅ᴠ **Brownout-Detector** ist im Core aktiv (Level 7 = höchste Schwelle) — kein Handlungsbedarf.
- ✅ᴠ **NVS hat eigenes Wear-Leveling** (Espressif-FAQ, Wortlaut nachgeprüft 10.06.: „erase-write
  balancing mechanism implemented internally"). Rechenbeispiel bestätigt: 4-Byte-Write/Minute
  über 10 Jahre = 5.256.000 Writes ≈ **42k Erase-Zyklen** (÷126 Entries/Page, 1 Sektor) <
  100k-Limit; mehrere NVS-Sektoren verteilen weiter ⇒ unsere debounced Duty-Persistenz ist
  unkritisch.
- ✅ᴠ **Heap-Fragmentierung überwachen mit `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)`**
  (lokal: `esp_heap_caps.h` Z.246; die IDF-Doku empfiehlt genau diese Funktion für „kann ich
  noch N Bytes allozieren"). Freier Gesamt-Heap allein ist irreführend (größter zusammen-
  hängender Block schrumpft zuerst) → in `/api/status`/Diag/MQTT aufnehmen. 🟡 (nur Blog-
  Heuristik): Ratio `1 − largest/free > 0,5` = kritisch. Gegenmittel: feste Puffer einmal
  beim Boot allozieren statt dynamischer Strings im Request-Pfad.
- ✅ᴠ **ESP32MQTTClient** (cyijun, GitHub nachgeprüft 10.06.): „thread-safe MQTT client",
  basiert auf offizieller **esp-mqtt**-Komponente, läuft nach `loopStart()` im eigenen
  FreeRTOS-Task (kein `client.loop()` nötig). Gepflegt: v1.1.3 vom 29.04.2026, 61★ —
  ernsthafter PubSubClient-Ersatz-Kandidat für die neue Architektur.
- ✅ᴠ **PubSubClient-Blockade-Falle (lokaler Quellcode, `PubSubClient.cpp` ~Z.256):**
  `connect()` busy-wartet auf das CONNACK bis `socketTimeout` (**Default 15 s**) — und zwar
  OHNE `yield()` (nur `readByte()` yieldet). Broker weg ⇒ Loop steht bis 15 s pro Versuch.
  **Wechselwirkung mit WDT:** Loop-WDT (5 s) + 15 s-Blockade = Reboot-Schleife bei Broker-
  Ausfall! ⇒ in neuer FW `setSocketTimeout(3..4)` < WDT-Timeout setzen + Reconnect-Backoff
  (nur alle N s versuchen). millis()-Arithmetik in der Lib selbst ist Overflow-sicher.
- 🟡 **PubSubClient + Arduino-`Ethernet` + W5500 hat dokumentierte Langzeit-Macken** (GitHub,
  nur Symptom-Ebene): knolleary#639 (sporadische Disconnects ESP32+W5500, malformed
  TCP/MQTT-Pakete), knolleary#759 (plötzlicher Socket-Verlust, W5500-Reset nötig — deckt
  sich mit unserem W5500-Auto-Reset!), arduino-libraries/Ethernet#92 (W5500 verliert
  Sockets nach zufälliger Zeit). ⇒ bestätigt die Migrationsrichtung native `ETH.h` +
  esp-mqtt (§B) für die neue FW; bis dahin trägt der vorhandene Auto-Reset.
- ✅ᴠ (Recherche ohne Gegenbefund) **LEDC + PCNT sind im Dauerlauf unauffällig:** beide
  Peripherien laufen hardware-autonom (PWM stabil unabhängig von CPU-Last; PCNT 16-bit-HW-
  Zähler, Overflow per Accumulator/Clear behandelbar — macht unsere FW per Poll+Clear).
  Keine bekannten 24/7-Issues gefunden.
- ✅ᴠ **F4R8 (Quad-Flash + Octal-PSRAM) ist offiziell supported** (IDF-Doku
  flash_psram_config; `ESPTOOLPY_OCT_FLASH` aus, PSRAM-Mode separat octal) — für uns nur
  akademisch, da PSRAM disabled bleibt (§4). **`qio120`/120-MHz NIE wählen:** offiziell
  experimentell, crasht bei >20 °C Chiptemperatur-Drift nach Power-on („accesses will
  crash randomly") — Ausschlusskriterium für 24/7-headless.
- 🟡 PoE-Modul-Detail (Waveshare-Wiki, Seite blockt Fetch mit 403): „PoE Module (B)",
  IEEE 802.3af ⇒ ~12,95 W PD-Budget. Extern-Modul-Architektur selbst ist ✅ per Schaltplan
  (§3, Header P2) belegt.
- ✅ᴠ (esptool-Doku, Wortlaut nachgeprüft) Flash-Mode-Optionen „are only consulted when
  flashing a bootable image at offset 0x0" → für OTA-App-Images faktisch egal (Basis
  der §4-Korrektur).
- ✅ᴠ **millis()-Audit v3.3 (10.06.):** 13/15 Stellen Overflow-sicher; **2 unsichere
  Deadline-Muster** (`millis() < deadline`): Z.662/671 Storm-Shield-Cooldown (am 49,7-d-Wrap
  kann ein Lüfter bis ~49 d in Mess-Pause hängen) und Z.960f `readLine`-Timeout (Wrap
  während des Wartens ⇒ Timeout feuert nie, Loop hängt an langsamem Client bis WDT/ewig).
  ⇒ in neuer FW auf `(uint32_t)(millis() - t0) >= dauer` umstellen.
- ℹ️ Recherche-Lauf 10.06.: 23 Quellen / 108 Claims; adversarialer Verify-Pass fiel dem
  Session-Limit zum Opfer (alle Votes 0-0 = ungeprüft, nicht widerlegt) → die Top-Claims
  wurden stattdessen oben von Hand gegen Primärquellen + lokalen Core verifiziert.

## 7. Geklärt / Noch offen

**Geklärt** (siehe Live-Diagnose oben): laufende Firmware = pre-v3.1 mit
~4250-B-Puffer-Bug; Consumer = ioBroker; Prioritäten = Zuverlässigkeit > UI > Architektur;
Defekt = kaputtes Dashboard/OTA/Diag durch Seiten-Truncation.

**Noch offen:**
- Soll die neue Firmware auf **v3.3 aufsetzen** (klar beste Basis) oder soll vorher
  noch eine größere Architektur-Überarbeitung (FreeRTOS/2. Core, Auth) rein?
- Build-Setup: **arduino-cli** lokal einrichten, damit Claude kompilieren kann?
  (Toolchain ESP32 Core 3.3.5; Flashen per USB oder via OTA-Endpoint.)
- Erst-Flash-Weg: USB (sicher) vs. direkter OTA-POST (Hypothese, ungetestet).
- ioBroker-Anbindung: ob Topic-Struktur beibehalten oder modernisiert wird, ist eine
  offene Entscheidung des Nutzers (laufend: `<prefix>/<fan>/pwm|rpm|cmd/pwm`, 0..255).

## 8. Arbeitsweise / Konventionen

- Nutzer schreibt Deutsch → Antworten auf Deutsch.
- Kein Git-Repo (Stand jetzt). Versionierung läuft über Ordnernamen (`v3.x`).
- MQTT muss zuverlässig funktionieren. Topics/Struktur/DeviceID dürfen geändert werden
  (ioBroker wird nachgezogen) — die Anforderung ist nur „MQTT bleibt funktionsfähig".
- **Flashen ist die einzige unumkehrbare Risiko-Aktion** → immer erst Freigabe holen,
  nie ungetestet, immer mit Anti-Brick-Netz (Rollback/Watchdog).

## 11. Brick-Risiko — gesicherte Fakten & offene Risiken

**Entlastend (geprüft):**
- OTA schreibt NUR in eine App-Partition (die inaktive). **Bootloader & Partitions-
  tabelle werden NIE angefasst** → der monatelang erarbeitete Boot/HW-Bring-up ist
  durch OTA nicht gefährdet. Ein fehlgeschlagener *Upload* kann nicht bricken (alte
  App bleibt aktiv bis Reboot).
- Core 3.3.8 hat `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, `BOOTLOADER_WDT` (9 s),
  `ESP_TASK_WDT` (5 s, PANIC), `ESP_INT_WDT` (300 ms) — die Bausteine für
  unbrickbares OTA sind da.
- **NVS-Schema luefter.ino ↔ v3.3 identisch** (Namespaces fans/mqtt/state/sys, gleiche
  Keys) → Lüfter-/MQTT-Config überlebt einen Versionswechsel.

**NICHT überschätzen / offene Risiken (ehrlich):**
- ROLLBACK_ENABLE ist NOTWENDIG, nicht HINREICHEND. Ob ein schlechtes Image wirklich
  zurückfällt, hängt davon ab, was das Image „valid" markiert (Mechanik §12C).
- Rollback fängt **Crash/Hang** ab, NICHT „bootet sauber, steuert Lüfter falsch".
- Rollback braucht eine intakte, valide Vorgänger-Partition als Ziel.
- Beim Reboot nach Flash: kurzes Fenster (~Sekunden), in dem PWM resettet, bevor die
  Firmware die Duties wiederherstellt → Lüfter kurz ungeregelt (thermisch trivial).

## 10. Verfügbare Tools / Plugins

- **`frontend-design`-Skill** (Plugin installiert) → für das UI-Redesign:
  erzeugt distinktive, produktionsreife Frontends statt generischer AI-Optik. Wichtig:
  Die ESP-UI ist server-gerendert in C++-`F()`-Strings mit engem RAM/Flash-Budget —
  Design-Output muss schlank/embedded-tauglich umgesetzt werden (kein schweres
  Framework, am besten eine kleine statische Seite + `/api/status`).
- Weitere installierte Skills (superpowers, code-review, verify, run …) nach Bedarf.

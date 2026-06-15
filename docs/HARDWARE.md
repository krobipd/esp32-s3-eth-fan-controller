# Hardware & Build-Settings

Board: **Waveshare ESP32-S3-ETH** (ESP32-S3R8 · 16 MB Flash W25Q128 · 8 MB OPI-PSRAM ·
W5500-Ethernet über SPI · PoE-Header).
Produktseite: <https://www.waveshare.com/esp32-s3-eth.htm>

## Arduino-IDE-Einstellungen (Tools-Menü)

| Einstellung | Wert | Hinweis |
|---|---|---|
| Board | **ESP32S3 Dev Module** | ESP32 Arduino Core 3.3.x |
| USB CDC On Boot | **Enabled** | Pflicht (Serial-Log über USB-CDC) |
| **PSRAM** | **Disabled** | ⚠️ **Kritisch** — siehe unten |
| Flash Size | 16MB (128Mb) | entspricht dem W25Q128 |
| Flash Mode | QIO | für OTA-Images faktisch egal (siehe unten) |
| Partition Scheme | OTA-fähig (zwei App-Slots) | Default lässt `app0`/`app1` zu; Image (~0,48 MB) passt locker |
| Upload | **per Netzwerk-OTA** | das Gerät hängt headless am LAN, kein USB |

### Build per `arduino-cli`
```sh
# 1) UI-Asset erzeugen (ui/index.html -> fan_controller/ui_asset.h)
tools/build_ui.sh

# 2) Firmware kompilieren
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc \
  --output-dir dist fan_controller
# -> dist/fan_controller.ino.bin
```

## ⚠️ Warum PSRAM = Disabled (Pflicht)

Der Octal-PSRAM des ESP32-S3R8 belegt den MSPI-Bus auf **GPIO 33–37**. Genau diese Pins
werden hier als **Tacho-Eingänge** genutzt. Mit `PSRAM=opi` wären 3 der 4 Drehzahl-
Signale tot. Das Board hat den PSRAM zwar bestückt, aber seine Pins sind durch die
Lüfter-Verkabelung belegt → **PSRAM bleibt dauerhaft aus.** Die 512 KB internes SRAM
genügen dem Controller mit großem Abstand.

## Pinbelegung

**W5500 (SPI, fest verdrahtet):**

| Signal | GPIO |
|---|---|
| MISO | 12 |
| MOSI | 11 |
| SCLK | 13 |
| CS | 14 |
| RST | 9 |
| INT | 10 (im Code nicht genutzt, geblockt) |

SPI-Takt: 20 MHz (innerhalb des stabilen W5500-Bereichs bei fester Verdrahtung).

**Für Lüfter erlaubte GPIOs (PWM & Tacho):**
`1, 2, 4, 5, 6, 7, 8, 15, 16, 17, 18, 21, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 47, 48`

**Geblockt (nicht für Lüfter):** W5500 9–14 · USB-CDC 19/20 · UART0 43/44 ·
Strapping/Boot 0/3/45/46 · intern (Flash/PSRAM-Bus) 33–37 nur bei `PSRAM=disabled` frei.

> Gotcha: GPIO 4/5/6/7 sind die SD-Karten-Pins — unproblematisch, solange keine SD genutzt wird.

## Peripherie-Grenzen (ESP32-S3)

- **LEDC (PWM):** 8 Kanäle → exakt 8 Lüfter möglich. PWM 25 kHz / 8-bit.
- **PCNT (Hardware-Pulszähler):** 4 Units → die ersten 4 Lüfter werden HW-gezählt,
  der Rest per ISR (mit Glitch-Filter). Tacho: 2 Pulse/Umdrehung.
- 2 CPU-Cores: Netz/HTTP/MQTT laufen auf Core 0, der Steuer-/Mess-Loop (RPM/PWM/Persistenz)
  auf Core 1 (Dual-Core-Split mit FreeRTOS-Queues + Mutex).

## Libraries

nativer `ETH.h` (W5500 via esp-netif/lwIP) · `ESP32MQTTClient` / esp-mqtt (thread-safe, eigener Task) ·
`Update` (OTA) · `Preferences` (NVS) · `SPI` + ESP-IDF (`driver/pcnt`, `esp_ota_ops`, `esp_task_wdt`).

## OTA & Anti-Brick (Kurzfassung)

Das Gerät ist nur per Netzwerk erreichbar (kein USB) → **Bricking ist inakzeptabel.**
Schutzmechanik:

- OTA schreibt nur in die inaktive App-Partition; Bootloader/Partitionstabelle bleiben unangetastet.
- Frisch geflashte Images booten in `PENDING_VERIFY` und müssen ein **90-s-Health-Window**
  überleben, sonst rollt der Bootloader automatisch auf die letzte gute Firmware zurück
  (`verifyRollbackLater()` + `commitIfPending()`).
- **Watchdog aktiv** (8 s, Panic → Reboot); kein Code-Pfad blockiert länger.
- Ein *bewusster* Reboot/zweites OTA aus dem laufenden Image markiert es vor dem Neustart
  als gültig (`commitIfPending`) — ein Reboot im 90-s-Fenster rollt also nicht versehentlich zurück.

<div align="center">

# 🌀 ESP32-S3-ETH Fan Controller

**Bis zu 8 PWM-Lüfter mit Tacho-Rückmeldung · Web-UI + MQTT · gehärtet für den 24/7-Dauerbetrieb**

[![Build](https://github.com/krobipd/esp32-s3-eth-fan-controller/actions/workflows/build.yml/badge.svg)](https://github.com/krobipd/esp32-s3-eth-fan-controller/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/krobipd/esp32-s3-eth-fan-controller?sort=semver&label=Release&color=2ea44f)](https://github.com/krobipd/esp32-s3-eth-fan-controller/releases/latest)
[![License: MIT](https://img.shields.io/github/license/krobipd/esp32-s3-eth-fan-controller?color=blue)](LICENSE)
![Platform](https://img.shields.io/badge/Platform-ESP32--S3-E7352C?logo=espressif&logoColor=white)
![Arduino](https://img.shields.io/badge/Arduino-Core_3.3.8-00979D?logo=arduino&logoColor=white)
![MQTT](https://img.shields.io/badge/MQTT-ready-660099?logo=mqtt&logoColor=white)
![Ethernet](https://img.shields.io/badge/Ethernet-W5500%20%2F%20PoE-555555)

</div>

---

Netzwerkfähiger PWM-Lüfter-Controller für das **Waveshare ESP32-S3-ETH** (W5500-Ethernet,
PoE). Bis zu **8 Lüfter** mit Drehzahl-Rückmeldung (Tacho), Steuerung über eine schlanke
**Web-UI** und **MQTT**. Headless im 24/7-Dauerbetrieb — kein WLAN, kein Bluetooth.

Typischer Einsatz: temperaturgesteuerte Kühlung von Netzwerk-/Server-Hardware (NAS, USV,
Switches) über eine zentrale Steuerung; die eigentliche Regel-Logik kann eine
Home-Automation (z. B. ioBroker) per MQTT übernehmen.

## ✨ Features

- **Web-UI** — eine statische, gzip-komprimierte Seite (Tabs: Dashboard / System /
  Einstellungen / Firmware) + JSON-API. Live-RPM, Schieberegler, Lüfter anlegen/
  kalibrieren, MQTT-Konfig, OTA-Upload, Logs. Hell/Dunkel-Modus.
- **MQTT** — flaches Topic-Schema, Last-Will, retained Ist-Werte, optionale
  Home-Assistant-Discovery. Consumer z. B. ioBroker.
- **Auf 24/7 ausgelegt** — Dual-Core (Netzwerk/Steuerung getrennt), aktiver Watchdog,
  wrap-sichere Zeitarithmetik (kein 49-Tage-Bug), NVS-schonende Persistenz, gehärtetes
  HTTP, Safe-Mode mit Failsafe-Duty.
- **Bombensicheres OTA** — Update in die inaktive Partition + automatisches
  Bootloader-Rollback, falls ein frisches Image das Health-Window nicht übersteht.
  (Auf der Zielhardware end-to-end verifiziert.)

## 📖 Dokumentation

| Doku | Inhalt |
|---|---|
| **[docs/HARDWARE.md](docs/HARDWARE.md)** | Board, Arduino-Einstellungen, Pinbelegung, Peripherie-Grenzen |
| **[docs/WIRING.md](docs/WIRING.md)** | **Verkabelung**: 4-Pin-Lüfter, Strom/Masse, PWM- & Tacho-Leitung, Schaltbild |
| **[docs/BUILD.md](docs/BUILD.md)** | Kompilieren (IDE/CLI), UI einbetten, OTA-Flash, Anti-Brick |
| **[docs/USAGE.md](docs/USAGE.md)** | Web-UI, Lüfter einrichten & kalibrieren, Robustheits-Features |
| **[docs/MQTT.md](docs/MQTT.md)** | Topic-Schema + ioBroker-Anbindung |
| **[docs/API.md](docs/API.md)** | HTTP-/JSON-Schnittstelle |

## 🚀 Schnellstart

1. **Verkabeln** — Lüfter (12 V extern), PWM- & Tacho-Leitung an erlaubte GPIOs, gemeinsame
   Masse. Schritt für Schritt: **[WIRING.md](docs/WIRING.md)**.
2. **Bauen & Flashen** — `tools/build_ui.sh`, dann kompilieren und per OTA flashen:
   **[BUILD.md](docs/BUILD.md)**.
   > ⚠️ **PSRAM = `Disabled`** bauen — der OPI-Bus belegt GPIO 33–37, die hier Tacho-Pins
   > sind. Begründung in [HARDWARE.md](docs/HARDWARE.md).
3. **Einrichten** — Web-UI unter `http://<geräte-ip>/` öffnen, Lüfter anlegen, Pins
   zuweisen, ggf. Mindest-Start kalibrieren: **[USAGE.md](docs/USAGE.md)**.
4. **Anbinden (optional)** — MQTT-Broker eintragen, in der Home-Automation steuern:
   **[MQTT.md](docs/MQTT.md)**.

## 📡 MQTT in Kürze

```
<prefix>/<deviceId>/info/status   online/offline   (retained, Last-Will)
<prefix>/<deviceId>/info/version  Firmware-Version (retained)
<prefix>/<deviceId>/info/ip       aktuelle IP      (retained)
<prefix>/<deviceId>/<fan>/speed    Ist-Stellwert 0–100 %   (retained)
<prefix>/<deviceId>/<fan>/set      Befehl 0–100
<prefix>/<deviceId>/<fan>/rpm      Drehzahl
```
Beispiel: `esp/ws-s3eth-1A2B3C/nas/set` = `60`. Details: [MQTT.md](docs/MQTT.md).

## 🗂️ Repo-Struktur

| Pfad | Inhalt |
|---|---|
| `fan_controller/` | Firmware (`.ino`, `fw_util.h`, `net_eth.h`, `concurrency.h`, generiertes `ui_asset.h`) |
| `ui/index.html` | Web-UI (Quelle) |
| `tools/build_ui.sh` | erzeugt `ui_asset.h` aus der UI |
| `tools/mock_api.py` | Mock-API für UI-Entwicklung ohne Gerät |
| `tests/host/` | Host-Unittests (Logik ohne Hardware) |
| `docs/` | Dokumentation (siehe oben) |

## 📄 Lizenz

Veröffentlicht unter der **MIT-Lizenz** — Firmware für eigene Hardware, Nutzung auf eigenes
Risiko, ohne Gewähr.

```
MIT License

Copyright (c) 2026 krobi

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

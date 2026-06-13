# ESP32-S3-ETH Fan Controller

Netzwerkfähiger PWM-Lüfter-Controller für das **Waveshare ESP32-S3-ETH** (W5500-Ethernet,
PoE). Bis zu **8 Lüfter** mit Drehzahl-Rückmeldung (Tacho), Steuerung über eine schlanke
**Web-UI** und **MQTT**. Headless im 24/7-Dauerbetrieb — kein WLAN, kein Bluetooth.

Typischer Einsatz: temperaturgesteuerte Kühlung von Netzwerk-/Server-Hardware (NAS, USV,
Switches) über eine zentrale Steuerung; die eigentliche Regel-Logik kann eine
Home-Automation (z. B. ioBroker) per MQTT übernehmen.

## Features

- **Web-UI** — eine statische, gzip-komprimierte Seite (Tabs: Dashboard / System /
  Einstellungen / Firmware) + JSON-API. Live-RPM, Schieberegler, Lüfter anlegen/
  kalibrieren, MQTT-Konfig, OTA-Upload, Logs.
- **MQTT** — flaches Topic-Schema, Last-Will, retained Ist-Werte. Consumer z. B. ioBroker.
- **Auf 24/7 ausgelegt** — aktiver Watchdog, wrap-sichere Zeitarithmetik (kein
  49-Tage-Bug), NVS-schonende Persistenz, gehärtetes HTTP, Safe-Mode mit Failsafe-Duty.
- **Bombensicheres OTA** — Update in die inaktive Partition + automatisches
  Bootloader-Rollback, falls ein frisches Image das 90-s-Health-Window nicht übersteht.
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

## MQTT in Kürze

```
<prefix>/<deviceId>/status        online/offline   (retained, Last-Will)
<prefix>/<deviceId>/<fan>/speed    Ist-Stellwert 0–100 %   (retained)
<prefix>/<deviceId>/<fan>/set      Befehl 0–100
<prefix>/<deviceId>/<fan>/rpm      Drehzahl
```
Beispiel: `esp/ws-s3eth-1A2B3C/nas/set` = `60`. Details: [MQTT.md](docs/MQTT.md).

## Repo-Struktur

| Pfad | Inhalt |
|---|---|
| `fan_controller/` | Firmware (`.ino`, `fw_util.h`, generiertes `ui_asset.h`) |
| `ui/index.html` | Web-UI (Quelle) |
| `tools/build_ui.sh` | erzeugt `ui_asset.h` aus der UI |
| `tools/mock_api.py` | Mock-API für UI-Entwicklung ohne Gerät |
| `tests/host/` | Host-Unittests (Logik ohne Hardware) |
| `docs/` | Dokumentation (siehe oben) |

## Lizenz

[MIT](LICENSE) — ohne Gewähr. Firmware für eigene Hardware, Nutzung auf eigenes Risiko.

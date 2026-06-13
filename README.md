# ESP32-S3-ETH Fan Controller

Netzwerkfähiger PWM-Lüfter-Controller auf Basis des **Waveshare ESP32-S3-ETH**
(ESP32-S3R8, W5500-Ethernet via SPI, PoE). Bis zu 8 Lüfter mit Drehzahl-Rückmeldung
(Tacho), Steuerung über eine schlanke Web-UI und MQTT. Läuft headless im 24/7-Betrieb —
kein WLAN, kein Bluetooth.

> **Status:** `v4.0` (Stufe 1) ist code-fertig — gehärtet für Dauerbetrieb + neues
> Web-UI. Die On-Device-Verifikation (Flash, Rollback-Test) ist der nächste Schritt,
> siehe [`docs/flash-checklist.md`](docs/flash-checklist.md).

## Highlights (v4.0 Stufe 1)

- **Bombensicheres OTA:** Update in die inaktive App-Partition + automatisches
  Bootloader-Rollback. Frische Images müssen ein 90-s-Health-Window überleben, sonst
  fällt das Gerät auf die letzte gute Firmware zurück (`verifyRollbackLater` +
  `commitIfPending`).
- **Aktiver Watchdog (8 s, Panic→Reboot):** kein Pfad blockiert den kooperativen
  `loop()` über das WDT-Budget; HTTP hat ein 10-s-Gesamtbudget.
- **24/7-Härtung:** wrap-sichere Zeitarithmetik (kein 49,7-Tage-millis()-Bug),
  NVS-schonende Log-Persistenz, chunked HTTP (kein W5500-Truncation), Safe-Mode mit
  Failsafe-Duty bei Crash-Loop.
- **Web-UI:** eine statische, gzip-komprimierte Seite (Control-Room-Stil, Tabs) +
  JSON-API (`/api/*`). Getrennt vom C++-Code, lokal gegen einen Mock-Server testbar.
- **MQTT (flaches Schema):** `<prefix>/<deviceId>/<name>/speed|set|rpm` + `…/status`
  (LWT). Consumer: ioBroker.

## Hardware

- Waveshare **ESP32-S3-ETH** (ESP32-S3R8 · 16 MB Flash W25Q128 · 8 MB OPI-PSRAM · W5500 · PoE-Header).
- ⚠️ **PSRAM muss `disabled` gebaut werden:** der Octal-PSRAM-Bus belegt GPIO 33–37, die
  hier als Tacho-Eingänge dienen. Siehe [`CLAUDE.md`](CLAUDE.md) §3/§4.

## Build

```sh
# UI-Asset erzeugen (ui/index.html -> fan_controller_v4.0/ui_asset.h)
tools/build_ui.sh

# Firmware kompilieren (Arduino ESP32 Core 3.3.x)
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc \
  --output-dir /tmp/out fan_controller_v4.0
```

Host-Unit-Tests der Pure-Logik (ohne Hardware):

```sh
c++ -std=c++17 tests/host/test_logic.cpp -o /tmp/t && /tmp/t   # -> OK
```

UI ohne Gerät entwickeln:

```sh
python3 tools/mock_api.py   # http://127.0.0.1:8077
```

## Flashen

Das Zielgerät hängt per PoE am LAN (kein USB). Update läuft per HTTP-POST:

```sh
curl -X POST --data-binary @/tmp/out/fan_controller_v4.0.ino.bin \
  -H 'Content-Type: application/octet-stream' http://<device-ip>/ota
```

**Vorher unbedingt [`docs/flash-checklist.md`](docs/flash-checklist.md) lesen** —
inkl. Rollback-Test und „kein Power-Cycle in den ersten 90 s".

## Repo-Aufbau

| Pfad | Inhalt |
|---|---|
| `fan_controller_v4.0/` | Aktuelle Firmware (`.ino`, `fw_util.h`, generiertes `ui_asset.h`) |
| `ui/index.html` | Web-UI (Quelle) |
| `tools/` | UI-Build-Script + Mock-API |
| `tests/host/` | Host-Unit-Tests |
| `docs/` | Spec, Implementierungsplan, Flash-Checkliste |
| `CLAUDE.md` | Projekt-Wissensbasis (Hardware-Details, Fallstricke, Entscheidungen) |
| `fan_controller_v3.3/`, `ESP32_FanController_v3.1_FINAL/` | Vorgänger-Stände |

## Roadmap

**Stufe 2** (späteres eigenes Design): Dual-Core/FreeRTOS-Trennung von Netz und
Steuer-/Messloop, nativer `ETH.h`-W5500-Treiber, thread-sicherer MQTT-Client,
optional Authentifizierung.

## Lizenz

[MIT](LICENSE) — ohne Gewähr. Firmware für eigene Hardware; Nutzung auf eigenes Risiko.

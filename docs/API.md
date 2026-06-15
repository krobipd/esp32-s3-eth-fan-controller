# HTTP-API

Die Web-UI ist eine statische Seite; alle Daten laufen über diese JSON-API. Schreibende
Endpunkte erwarten `application/x-www-form-urlencoded` und antworten mit
`{"ok":true}` bzw. `{"ok":false,"error":"…"}`. Schreibzugriffe füllen intern nur Queues
und werden im Hauptloop abgearbeitet (entkoppelt von der Hardware).

> **Keine Authentifizierung.** Das Gerät gehört in ein vertrauenswürdiges LAN — wer es erreicht,
> kann es steuern und flashen. (Ein Passwort-Lockout wäre bei einem Gerät ohne USB-Rettung
> selbst ein Risiko, daher bewusst nicht standardmäßig aktiv.)

## GET

| Pfad | Antwort |
|---|---|
| `/` | Web-UI (gzip, `text/html`) |
| `/api/status` | kompletter Status als JSON (siehe unten) |
| `/log.txt` | Log des aktuellen Boots (Text, chunked) |
| `/prevlog.txt` | Log-Tail des vorherigen Boots |

### `GET /api/status` (Auszug der Felder)

```json
{
  "rev": 3,
  "fw_version": "5.2.0",
  "device": "ws-s3eth-1A2B3C",
  "ip": "10.0.0.42",
  "mqtt_connected": true,
  "boot_count": 12,
  "safe_mode": false,
  "reset_reason": "SW",
  "crash_streak": 0,
  "wdt": true,
  "ota_pending": false,
  "min_free_heap": 201000,
  "largest_block": 198000,
  "uptime_s": 3600,
  "core1_loops": 1234567,
  "net_loops": 1234890,
  "net_stack_hwm": 4304,
  "mqtt": { "enabled": true, "host": "10.0.0.5", "port": 1883, "user": "iob", "prefix": "esp" },
  "free_pwm":  [1,2,8,15,16,17,18,21,33,34,39,48],
  "free_tach": [1,2,8,15,16,17,18,21,33,34,39,48],
  "fans": [
    { "index":0, "name":"nas", "present":true, "pwm":148, "pct":58, "rpm":864,
      "pwmPin":47, "tachPin":38, "fault":0, "validated":true, "inv":false,
      "cmin":15, "cnote":"be quiet" }
  ]
}
```

- `fw_version` = laufende Firmware-Version (Semver, einzige Quelle im Code).
- `core1_loops`/`net_loops` = Heartbeats der beiden Cores (Control-Loop bzw. Netz-Task) — beide müssen
  steigen; `net_stack_hwm` = freier Netz-Task-Stack in Bytes (Liveness/Diagnose des Dual-Core-Betriebs).
- `pwm` = roher 8-bit-Duty (0–255), `pct` = davon abgeleitete Prozent (0–100).
- `present` = vollständig konfiguriert (Name + beide Pins). Unkonfigurierte, aber angelegte
  Slots erscheinen mit `present:false` (zum Bearbeiten).
- Das MQTT-**Passwort** wird nie ausgegeben.
- `free_pwm`/`free_tach` = aktuell freie, geeignete GPIOs (für die Pin-Auswahl im UI).

## POST

| Pfad | Felder | Wirkung |
|---|---|---|
| `/api/fan/set` | `idx`, `pct` (0–100) | Leistung setzen |
| `/api/fan/save` | `idx`, `name`, `pwm`, `tach`, `inv` (0/1) | Lüfter ändern; **`idx=-1` legt einen neuen an** (belegt ersten freien Slot, `name`+Pins Pflicht) |
| `/api/fan/delete` | `idx` | Lüfter löschen (jeder belegte Slot) |
| `/api/calib` | `idx`, `cmin` (0–100), `cnote` | Kalibrierung speichern |
| `/api/mqtt` | `enabled`, `host`, `port`, `user`, `pass`, `prefix` | MQTT-Konfig (leeres `pass` lässt das alte stehen) |
| `/api/safemode/reset` | – | Crash-Streak/Safe-Mode zurücksetzen (wirkt nach Reboot) |
| `/api/reboot` | – | Neustart (markiert ein PENDING-Image vorher gültig) |
| `/ota` | Body = `.bin` (`application/octet-stream`) | Firmware-Update, danach Reboot |

### Beispiele (curl)

```sh
# Lüfter 0 auf 60 %
curl -X POST -d 'idx=0&pct=60' http://<ip>/api/fan/set

# Status abfragen
curl http://<ip>/api/status

# Firmware flashen (siehe BUILD.md für das Anti-Brick-Verhalten)
curl -X POST --data-binary @dist/fan_controller.ino.bin \
  -H 'Content-Type: application/octet-stream' http://<ip>/ota
```

Härtung: Header-Anzahl und Body-Größe sind begrenzt, jeder Nicht-OTA-Request hat ein
10-s-Gesamtbudget (ein langsamer Client kann den Loop nicht blockieren), Antworten werden
gechunkt gesendet.

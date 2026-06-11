# Design: Fan Controller v4.0 — Stufe 1 (Härtung + neues Web-UI)

Datum: 2026-06-10 · Status: vom Nutzer freigegeben (Design-Gespräch)
Basis: `fan_controller_v3.3` · Ziel-Gerät: Waveshare ESP32-S3-ETH @ 10.47.88.239 (PoE, **kein USB**)

## 1. Ziel

Die Firmware muss **immer booten** und **24/7 stabil laufen**; das Webinterface wird
modern (Control-Room-Stil), vollständig und voll funktionsfähig. Stufe 1 härtet die
bewährte v3.3-Single-Loop-Architektur und ersetzt das UI; der größere Architektur-Umbau
(Dual-Core, nativer ETH.h-Treiber, esp-mqtt) ist **Stufe 2** mit eigenem Design und
läuft später über das dann abgesicherte OTA.

## 2. Getroffene Entscheidungen (Nutzer)

| Thema | Entscheidung |
|---|---|
| Vorgehen | **Zweistufig:** erst härten (diese Spec), dann Architektur-Umbau (Stufe 2) |
| UI-Umfang | Heutige Funktionen modernisiert **+ System-Monitor** (keine Graphen, keine Automatik/Kurven) |
| MQTT-Topics | **Sofort neues, flaches Schema** `<prefix>/<deviceId>/<name>/speed|set|rpm` (0..100, Details §4); ioBroker wird nach dem Flash nachgezogen |
| Design-Richtung | **A: Control-Room / Industrial** (Monospace-Zahlen, Status-LEDs, dichte Raster, dunkel) |
| Navigation | **B: Tab-Leiste** — Dashboard / System / Einstellungen / Firmware |
| UI-Technik | **Statische App + JSON-API:** eine HTML/CSS/JS-Datei, gzip im Flash, Daten nur über `/api/*`, Polling (kein SSE) |
| Auth | **Keine** in Stufe 1 (Lockout-Risiko ohne USB > LAN-Bedrohung; Stufe-2-Thema) |
| Versionierung | `fan_controller_v4.0/`-Ordner + **git init** im Projektordner |

## 3. Zuverlässigkeits-Kern

Alle Punkte referenzieren das Code-Audit (CLAUDE.md §6 Pkt. 8–17, §12C, §12F).

### 3.1 Watchdog (statt `esp_task_wdt_deinit()`)
- Core-TWDT bleibt aktiv (5 s, `trigger_panic` ⇒ Reboot), `loopTask` wird per
  `enableLoopWDT()` abonniert; Fütterung implizit pro Loop-Durchlauf.
- Konsequenz: **kein Codepfad darf > ~3 s blocken.** Dafür:
  - `mqtt.setSocketTimeout(3)` (PubSubClient-CONNACK-Busy-Wait, §12F),
  - `Ethernet.begin(mac, 5000, 2000)` statt 60-s-Default,
  - HTTP-Gesamtbudget 10 s pro Request (Ausnahme: OTA-Upload, der füttert den WDT
    im Empfangs-Loop explizit via `feedLoopWDT()`).

### 3.2 OTA-Anti-Brick (Rollback + Health-Window)
- `extern "C" bool verifyRollbackLater() { return true; }` — Boot-Auto-Validierung aus.
- Health-Window: Läuft das frische Image **90 s** ohne Crash/WDT (Kriterium: Loop
  erreicht den 90-s-Punkt), ruft es `esp_ota_mark_app_valid_cancel_rollback()`.
  Bewusst **nicht** an Link/MQTT geknüpft — ein Broker-/Switch-Ausfall darf kein
  gutes Image zurückrollen.
- Crash/Hänger vor Ablauf ⇒ WDT-Reboot ⇒ Bootloader-Rollback aufs vorherige Image
  (Mechanik §12C, Core 3.3.8 verifiziert).

### 3.3 millis()-Wrap-Fixes (49,7-Tage-Bugs)
Alle Deadline-Vergleiche auf Delta-Arithmetik `(uint32_t)(now - t0) >= dauer`:
- Storm-Shield (v3.3 Z.662/671), `readLine`-Timeout (Z.960f), MQTT-Backoff (Z.794/817/832)
- plus Review **aller** verbleibenden Zeitvergleiche im Code.

### 3.4 Boot-Reihenfolge (Lüfter vor Netz)
`restoreSavedDuties()` + `dutyProcessQueue()` laufen in `setup()` **vor** der
W5500-/DHCP-Initialisierung. Nach Reboot drehen die Lüfter sofort mit den
gespeicherten Werten, auch wenn das Netz minutenlang fehlt (Stromausfall-Szenario).

### 3.5 NVS-Schonung (110-Tage-Verschleißpfad schließen)
- `persistLogTail()` nicht mehr periodisch (8 s), sondern nur noch: einmal nach Boot
  (Marker) + im Panic-/Shutdown-Pfad (`esp_register_shutdown_handler`) + vor
  beabsichtigtem Reboot/OTA.
- `LOGI("HTTP", …)` pro Request entfällt; geloggt werden nur Fehler und Zustandswechsel.
- Duty-Persistenz bleibt debounced (2,5 s) — unkritisch (§12F NVS-Rechnung).

### 3.6 HTTP-Härtung
- Limits: max. 32 Header-Zeilen, Form-Body-Cap 4 KB (Content-Length validiert,
  kein blindes `reserve()`), Gesamt-Request-Budget 10 s.
- `/log.txt` und alle Antworten > 1 KB werden **chunked** gesendet (max. 1-KB-Writes)
  — Fix der Lib-Truncation (`EthernetClient::write()` meldet Erfolg trotz 4-KB-Kappung).
- Server-seitige Namens-Whitelist (`[a-zA-Z0-9 _-]`, ≤19 Zeichen) für Fan-Namen und
  Kalibrier-Notiz; UI escaped zusätzlich client-seitig (XSS-Fix §6.16).
- `404` statt `400` für unbekannte Pfade; `/fans/new`-Äquivalent nur noch per POST.

### 3.7 Safe-Mode mit echter Wirkung
Bei `crash_streak >= 3`:
- alle konfigurierten Lüfter fest auf **70 % Failsafe-Duty** (keine Regelung),
- HTTP + OTA aktiv (Rettungspfad), **MQTT deaktiviert**, RPM-Messung passiv,
- UI zeigt unübersehbaren Banner („SAFE MODE — crash loop erkannt").
Verlassen durch erfolgreichen OTA-Flash oder manuellen Reset des Streaks im UI.

### 3.8 Telemetrie
`/api/status` zusätzlich: `largest_free_block` (heap_caps), `uptime_s`, `wdt_armed`,
`ota_pending_verify`, `crash_streak`, `safe_mode`. Sichtbar im System-Tab.

### 3.9 Kleinere Fixes aus dem Audit
- Fan-Namen-Uniqueness nach `sanitizeName()` erzwingen (MQTT-Topic-Kollision §6.17).
- Beabsichtigter Reboot/OTA publisht vorher `status=offline` + `mqtt.disconnect()`.
- Beibehaltene Bewährtes: Queues (`g_apply`, `g_pendingDuty`), PCNT/ISR-Zweistufigkeit,
  Storm-Shield, Median+EMA, W5500-Auto-Reset, debounced State-Writes — unverändert.

## 4. MQTT (neues Schema, sofort — Nutzer-Revision 11.06.: flach + sprechende Namen)

- Topics (KEINE `fan/`-Zwischenebene, pro Lüfter genau drei Datenpunkte):
  - `<prefix>/<deviceId>/status` — `online`/`offline` (retained, LWT)
  - `<prefix>/<deviceId>/<name>/speed` — Ist-Stellwert **0..100 %** (retained)
  - `<prefix>/<deviceId>/<name>/set` — Befehl 0..100 (kein Retain; einziger Schreib-Datenpunkt)
  - `<prefix>/<deviceId>/<name>/rpm` — Drehzahl
- Reservierte Lüfternamen: `status`, `sys` (Kollision mit Geräte-Datenpunkten) —
  Validierung lehnt sie ab.
- Stack bleibt PubSubClient + Ethernet.h (Stufe 1), Backoff-Reconnect (wrap-sicher),
  `setSocketTimeout(3)`.
- ioBroker wird **nach** dem Erst-Flash vom Nutzer umkonfiguriert; bis dahin halten
  die Lüfter ihre restaurierten Duties (thermisch unkritisch).

## 5. Web-UI

### 5.1 Auslieferung
- `ui/index.html` (eine Datei, Vanilla JS/CSS, Control-Room-Design) wird per
  Build-Script (`tools/build_ui.sh`: gzip → `xxd -i` → `ui_asset.h`) in die Firmware
  eingebettet und chunked mit `Content-Encoding: gzip` ausgeliefert. Ziel ≤ 15 KB gzip.
- UI ist getrennt entwickelbar/testbar: `tools/mock_api.py` serviert `ui/index.html`
  + Fake-`/api/*` (kein Gerät nötig).

### 5.2 Tabs & Funktionen
- **Dashboard:** Lüfter-Grid (Karte je Lüfter: Name, RPM groß, Status-LED, PWM-Slider
  mit Debounce wie heute, Fault-Badge), Status-Leiste (ETH/MQTT/WDT/Safe-Mode).
- **System:** Heap + largest_free_block, Uptime, Boot-Count, Reset-Grund, Crash-Streak,
  OTA-Partition-Status, Log-Viewer (aktuell + prev, chunked geladen), Reboot-Button
  (mit Confirm), Safe-Mode-Reset.
- **Einstellungen:** Lüfter anlegen/bearbeiten/löschen (Pins aus erlaubter Liste,
  belegte ausgegraut), Kalibrierung (Min-Start % + Live-Test), MQTT-Konfig
  (Passwortfeld type=password, wird nie im JSON zurückgegeben).
- **Firmware:** OTA-Upload mit Fortschrittsbalken, Validierungs-Status
  (PENDING_VERIFY/valid), Reboot-Watch; Hinweis auf curl-Fallback.

### 5.3 API (JSON, schreibend → Queues)
- `GET /api/status` (wie heute + §3.8-Felder)
- `POST /api/fan/set` `{idx,pct}` · `POST /api/fan/save` `{idx,name,pwm,tach,inv}` ·
  `POST /api/fan/delete` `{idx}` · `POST /api/fan/new`
- `POST /api/calib` `{idx,cminPct,note}` · `POST /api/mqtt` `{enabled,host,port,user,pass,prefix}`
- `POST /api/safemode/reset` · `POST /api/reboot`
- `POST /ota` (application/octet-stream, **unverändert kompatibel zu curl** — Rettungsleine)
- Fehler immer als `{ok:false,error:"…"}` mit passendem HTTP-Code.

## 6. Erst-Flash & Risiko (ehrlich dokumentiert)

1. Build v4.0 (FQBN `esp32:esp32:esp32s3:CDCOnBoot=cdc`, **PSRAM=disabled** — Pflicht,
   §4 CLAUDE.md: OPI-Bus kollidiert mit Tacho 35/36/37; FlashSize-Option irrelevant für OTA).
2. **Nur mit expliziter Nutzer-Freigabe:** `curl -X POST --data-binary @fw.bin
   -H 'Content-Type: application/octet-stream' http://10.47.88.239/ota`
3. **Der Erst-Flash ist der eine ungeschützte Schritt** (Altfirmware hat kein
   Health-Window). Risikominimierung: kompletter HW-Init-Pfad (SPI/W5500/LEDC/PCNT/NVS)
   wird 1:1 aus v3.3 übernommen; alle Änderungen sind additiv/chirurgisch; NVS-Schema
   ist identisch (Konfig überlebt, §11).
4. Nach erfolgreichem v4.0-Lauf: **Rollback-Kette einmal absichtlich validieren** —
   Test-Image flashen, das nach Boot kontrolliert crasht; es MUSS automatisch auf
   v4.0 zurückfallen. Erst danach gilt OTA als „bombensicher".

## 7. Verifikation

- Build: arduino-cli, EXIT=0, Image-Größe < App-Slot (~1,25 MB konservativ angenommen).
- UI: Browser-Test gegen Mock-API (alle Tabs, Fehlerfälle: Gerät offline, 400er).
- Nach Flash (Checkliste): Boot < 10 s · IP per DHCP · `/api/status` ok · MQTT
  verbunden, neue Topics in ioBroker sichtbar · RPM aller 4 Lüfter plausibel ·
  Slider-Roundtrip · OTA-Roundtrip + Rollback-Test (§6.4) · 24-h-Beobachtung
  `largest_free_block` (darf nicht monoton fallen).

## 8. Nicht-Ziele (Stufe 2 / bewusst raus)

Dual-Core/FreeRTOS-Aufteilung, nativer `ETH.h`-W5500-Treiber, esp-mqtt/ESP32MQTTClient,
SSE-Push, Authentifizierung, Lüfterkurven/Automatik im Gerät, Verlaufs-Graphen,
HA-Discovery, PSRAM-Nutzung (dauerhaft disabled wegen Pin-Konflikt).

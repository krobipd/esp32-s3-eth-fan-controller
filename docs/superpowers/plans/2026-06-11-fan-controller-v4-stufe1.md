# Fan Controller v4.0 (Stufe 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Gehärtete v3.3-Firmware (immer bootfähig, 24/7-stabil, OTA-Anti-Brick) mit neuem Control-Room-Web-UI (statische App + JSON-API) und flachem MQTT-Schema.

**Architecture:** Single-Loop-Architektur aus v3.3 bleibt; alle Audit-Fixes additiv (WDT, OTA-Health-Window, Wrap-Fixes, Boot-Reihenfolge, NVS-Schonung, HTTP-Härtung, Safe-Mode). UI ist EINE gzip-HTML-Datei im Flash, Daten ausschließlich über `/api/*`. Schreibpfade füllen weiterhin nur Queues.

**Tech Stack:** Arduino ESP32 Core 3.3.8, Ethernet v2.0.2, PubSubClient, Preferences, Update; arduino-cli Build; Vanilla-JS-UI; Host-Tests mit plain `c++`.

**Spec:** `docs/superpowers/specs/2026-06-10-fan-controller-v4-stufe1-design.md`

**Referenz-Quelle:** `fan_controller_v3.3/fan_controller_v3.3.ino` (Zeilenangaben = v3.3-Stand; nach Kopie identisch)

**Build-Kommando (überall identisch verwendet):**
```bash
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$CLI" compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc --output-dir /tmp/out-v4 /Volumes/ssd/claude/esp32/fan_controller_v4.0
```
Erwartet: `EXIT=0`. ⚠️ Niemals `PSRAM=opi` (GPIO-33-37-Konflikt, CLAUDE.md §4). **Es wird in diesem Plan NICHT geflasht** — Flash nur mit expliziter Nutzer-Freigabe (Spec §6).

## Datei-Struktur

| Pfad | Verantwortung |
|---|---|
| `fan_controller_v4.0/fan_controller_v4.0.ino` | Firmware (v3.3-Kopie + chirurgische Fixes) |
| `fan_controller_v4.0/fw_util.h` | Pure Logik (Zeit-Deltas, %-Mapping, Namens-Validierung) — host-testbar |
| `fan_controller_v4.0/ui_asset.h` | GENERIERT aus `ui/index.html` (gzip, PROGMEM) |
| `ui/index.html` | Web-UI (eine Datei, Control-Room, Tabs) |
| `tools/build_ui.sh` | gzip → C-Header |
| `tools/mock_api.py` | Mock-API für UI-Entwicklung ohne Gerät |
| `tests/host/test_logic.cpp` | Host-Unit-Tests für `fw_util.h` |

---

### Task 1: Scaffold — v3.3 nach v4.0 kopieren

**Files:**
- Create: `fan_controller_v4.0/fan_controller_v4.0.ino` (Kopie)

- [ ] **Step 1: Kopieren + Versions-Strings**

```bash
cd /Volumes/ssd/claude/esp32
mkdir -p fan_controller_v4.0
cp fan_controller_v3.3/fan_controller_v3.3.ino fan_controller_v4.0/fan_controller_v4.0.ino
```

Im neuen File (Kopf-Kommentar Z.2 und Serial-Banner in `setup()`):
- `* WAVESHARE ESP32-S3-ETH Fan Controller v3.3` → `… v4.0 (Stufe 1: Haertung + neues UI)`
- `Serial.println(F("== ESP32-S3-ETH Fan Controller v3.3 =="));` → `…v4.0==`

- [ ] **Step 2: Build-Check** — Build-Kommando oben, Erwartet: EXIT=0, ~492 KB.

- [ ] **Step 3: Commit**

```bash
git add fan_controller_v4.0 && git commit -m "v4.0 scaffold: Kopie von v3.3"
```

---

### Task 2: fw_util.h + Host-Tests (TDD)

**Files:**
- Create: `fan_controller_v4.0/fw_util.h`
- Test: `tests/host/test_logic.cpp`

- [ ] **Step 1: Failing Test schreiben** — `tests/host/test_logic.cpp`:

```cpp
#include <cassert>
#include <cstdio>
#include "../../fan_controller_v4.0/fw_util.h"

int main() {
  // elapsed(): normal
  assert(!elapsed(1000, 500, 600));
  assert( elapsed(1100, 500, 600));
  // elapsed(): ueber den 32-bit-Wrap (DER 49,7-Tage-Bug)
  uint32_t since = 0xFFFFFF00u;
  assert(!elapsed(0x00000010u, since, 0x200));  // 0x110 ms vergangen < 0x200
  assert( elapsed(0x00000150u, since, 0x200));  // 0x250 ms vergangen
  // pct<->duty Roundtrip muss fuer alle 0..100 stabil sein
  for (int p = 0; p <= 100; p++) assert(pctFromDuty(dutyFromPct((uint8_t)p)) == p);
  // Namen: nur a-z 0-9 _ - , 1..19 Zeichen, reservierte verboten
  assert(fanNameValid("nas"));  assert(fanNameValid("fan_2-x"));
  assert(!fanNameValid(""));    assert(!fanNameValid("NAS"));
  assert(!fanNameValid("status")); assert(!fanNameValid("sys"));
  assert(!fanNameValid("zu-lang-1234567890123"));
  puts("OK");
  return 0;
}
```

- [ ] **Step 2: Test laufen lassen — muss FEHLSCHLAGEN**

```bash
mkdir -p tests/host
c++ -std=c++17 tests/host/test_logic.cpp -o /tmp/test_logic && /tmp/test_logic
```
Erwartet: Compile-Fehler `fw_util.h: No such file`.

- [ ] **Step 3: `fan_controller_v4.0/fw_util.h` implementieren**

```cpp
#pragma once
#include <stdint.h>
#include <string.h>

// Wrap-sicherer Zeitvergleich: true sobald intervalMs seit 'since' vergangen.
// IMMER statt `millis() < deadline` verwenden (49,7-Tage-Wrap!).
static inline bool elapsed(uint32_t now, uint32_t since, uint32_t intervalMs) {
  return (uint32_t)(now - since) >= intervalMs;
}

static inline uint8_t pctFromDuty(uint8_t duty) {
  return (uint8_t)((duty * 100U + 127) / 255U);
}
static inline uint8_t dutyFromPct(uint8_t pct) {
  if (pct > 100) pct = 100;
  return (uint8_t)((pct * 255U + 50) / 100U);
}

// Gueltig nach sanitizeName(): a-z 0-9 _ - ; 1..19 Zeichen; nicht reserviert.
static inline bool fanNameValid(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n > 19) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  }
  if (strcmp(s, "status") == 0 || strcmp(s, "sys") == 0) return false;
  return true;
}
```

- [ ] **Step 4: Test laufen lassen — PASS** (Ausgabe `OK`).

- [ ] **Step 5: .ino auf fw_util.h umstellen** — in `fan_controller_v4.0.ino`:
  - Nach `#include <Preferences.h>` einfügen: `#include "fw_util.h"`
  - Die beiden Inline-Definitionen löschen (Z.379-380 v3.3):
    `static inline uint8_t pctFromDuty(…)` und `static inline uint8_t dutyFromPct(…)`
    (Signaturen identisch — alle Aufrufer bleiben unverändert; das `constrain` in der
    alten dutyFromPct ist durch das `if (pct > 100)` ersetzt).

- [ ] **Step 6: Build-Check** (EXIT=0) **+ Commit**

```bash
git add fan_controller_v4.0 tests && git commit -m "fw_util.h: wrap-sichere Zeit, pct-Mapping, Namens-Validierung + Host-Tests"
```

---

### Task 3: millis()-Wrap-Fixes (Storm, readLine, MQTT-Backoff)

**Files:**
- Modify: `fan_controller_v4.0/fan_controller_v4.0.ino` (Fan-Struct, stormTrip/stormTryRecover, readLine, mqttEnsureConnected, loop)

- [ ] **Step 1: Storm-Shield auf Flag+Since umstellen**

In `struct Fan`: `uint32_t stormUntilMs;` → `uint32_t stormSinceMs; bool stormActive;`
In `fanInitDefaults()`: `f.stormUntilMs = 0;` → `f.stormSinceMs = 0; f.stormActive = false;`

`stormTrip()` (v3.3 Z.657): Zeile `f.stormUntilMs = millis() + STORM_COOLDOWN_MS;` →
```cpp
  f.stormActive = true;
  f.stormSinceMs = millis();
```

`stormTryRecover()` (Z.668): die ersten drei Zeilen ersetzen durch
```cpp
  Fan &f = fans[i];
  if (!f.stormActive) return;
  if (!elapsed(millis(), f.stormSinceMs, STORM_COOLDOWN_MS)) return;
  f.stormActive = false;
```
(die Zeile `f.stormUntilMs = 0;` entfällt)

In `loop()` RPM-Block (Z.1736): `if (f.stormUntilMs && now < f.stormUntilMs) {` →
`if (f.stormActive) {`

- [ ] **Step 2: readLine() Deadline-Fix** (Z.958-972) — Funktion ersetzen:

```cpp
static bool readLine(EthernetClient &c, String &out, unsigned long timeoutMs) {
  out = "";
  unsigned long t0 = millis();
  while (!elapsed(millis(), t0, timeoutMs)) {
    while (c.available()) {
      char ch = (char)c.read();
      if (ch == '\r') continue;
      if (ch == '\n') return true;
      out += ch;
      if (out.length() > 4096) return true;
    }
    delay(1);
  }
  return (out.length() > 0);
}
```

- [ ] **Step 3: MQTT-Backoff-Fix** — Globals (Z.794):
`static uint32_t g_nextMqttTryMs = 0, g_mqttBackoffMs = 3000;` →
`static uint32_t g_lastMqttTryMs = 0; static uint32_t g_mqttBackoffMs = 3000;`

In `mqttEnsureConnected()` connected-Zweig (Z.810-811):
```cpp
      g_mqttBackoffMs = 3000;
      g_lastMqttTryMs = now;
```
Retry-Gate (Z.817): `if (now < g_nextMqttTryMs) return;` →
`if (!elapsed(now, g_lastMqttTryMs, g_mqttBackoffMs)) return;`
direkt danach (vor `mqtt.setServer`): `g_lastMqttTryMs = now;`
Fail-Zweig (Z.831-832): die Zeile `g_nextMqttTryMs = now + g_mqttBackoffMs;` löschen
(Backoff-Verdopplung `g_mqttBackoffMs = min<uint32_t>(g_mqttBackoffMs * 2, 60000);` bleibt).

- [ ] **Step 4: Build-Check** (EXIT=0) **+ Commit** `git commit -am "Wrap-sichere Zeitarithmetik: Storm-Shield, readLine, MQTT-Backoff"`

---

### Task 4: Watchdog aktiv + kurze Timeouts überall

**Files:**
- Modify: `fan_controller_v4.0/fan_controller_v4.0.ino` (Konstanten, setup, readLine, handleBodyToString, handleOTA, loop)

- [ ] **Step 1: WDT statt Deinit** — in `setup()` die Zeile `esp_task_wdt_deinit();` ersetzen:

```cpp
  // Anti-Brick: Task-WDT bleibt AKTIV und ueberwacht den Loop-Task.
  // 8 s Budget; Panic => Reboot => ggf. Bootloader-Rollback (Spec §3.1/3.2).
  {
    esp_task_wdt_config_t wcfg = {};
    wcfg.timeout_ms    = 8000;
    wcfg.idle_core_mask = (1 << 0);
    wcfg.trigger_panic = true;
    esp_task_wdt_reconfigure(&wcfg);
  }
  enableLoopWDT();
```

- [ ] **Step 2: Timeouts kürzen** (Konstanten Z.58-59):
`CLIENT_RD_TIMEOUT = 15000` → `3000` · `BODY_RD_TIMEOUT = 300000` → `30000`
(beides sind Stall-Budgets: `t0` wird bei Fortschritt zurückgesetzt).

- [ ] **Step 3: DHCP nicht-blockierend genug** — beide Aufrufe `Ethernet.begin(g_mac)`
(setup Z.1633, loop Z.1673) → `Ethernet.begin(g_mac, 4000, 1200)` (4 s Gesamt, 1,2 s Response — bleibt unter dem 8-s-WDT).

- [ ] **Step 4: PubSubClient-CONNACK-Falle** — in `setup()` nach `loadMQTTConfig();`:
```cpp
  mqtt.setSocketTimeout(3);  // CONNACK-Busy-Wait < WDT-Budget (Spec §3.1)
```

- [ ] **Step 5: WDT in Warteschleifen füttern** — `feedLoopWDT();` einfügen:
  - `readLine()`: vor `delay(1);`
  - `handleBodyToString()`: im `else`-Zweig vor `delay(1);`
  - `handleOTA()`: im Empfangs-Loop nach `received += n; t0 = millis();` UND im `else`-Zweig vor `delay(1);` — außerdem Schleifenkopf auf Delta umstellen:
    `while (received < contentLength && millis() - t0 < BODY_RD_TIMEOUT)` →
    `while (received < contentLength && !elapsed(millis(), t0, BODY_RD_TIMEOUT))`
  - gleiche Umstellung in `handleBodyToString()`: `millis() - t0 < BODY_RD_TIMEOUT` → `!elapsed(millis(), t0, BODY_RD_TIMEOUT)`
  - `loop()` HTTP-Wartezeile (Z.1705): `while (c.connected() && !c.available() && (millis() - t0 < 400)) delay(1);` → `while (c.connected() && !c.available() && !elapsed(millis(), t0, 400)) { feedLoopWDT(); delay(1); }`

- [ ] **Step 6: Build-Check** (EXIT=0) **+ Commit** `git commit -am "WDT aktiv (8s, panic) + alle Block-Pfade unter WDT-Budget"`

---

### Task 5: Boot-Reihenfolge (Lüfter vor Netz) + Safe-Mode mit Wirkung

**Files:**
- Modify: `fan_controller_v4.0/fan_controller_v4.0.ino` (setup, loop, stateFlushIfNeeded)

- [ ] **Step 1: setup() umordnen** — den Block ab `restoreSavedDuties();` (Z.1628) bis vor `resetW5500();` ersetzen durch:

```cpp
  // [Spec §3.4/3.7] Luefter drehen BEVOR das Netz initialisiert wird.
  if (g_crashLoopDetected) {
    // SAFE MODE: feste 70 % Failsafe, kein Restore, MQTT bleibt aus (loop).
    for (uint8_t i = 0; i < MAX_FANS; i++)
      if (fanPresentIdx(i)) dutyEnqueue(i, dutyFromPct(70));
    LOGW("SAFE", "crash loop erkannt -> Failsafe 70%, MQTT aus");
  } else {
    restoreSavedDuties();  // [B1]
  }
  dutyProcessQueue();
```

- [ ] **Step 2: MQTT im Safe-Mode aus** — `loop()` (Z.1712):
`if (mqttConfig.enabled && ethHasIP) {` → `if (mqttConfig.enabled && ethHasIP && !g_crashLoopDetected) {`

- [ ] **Step 3: Failsafe-Duty nicht persistieren** — `stateFlushIfNeeded()` erste Zeile:
```cpp
  if (g_crashLoopDetected) return;  // Safe-Mode-Duties nie in NVS schreiben
```

- [ ] **Step 4: Build-Check + Commit** `git commit -am "Boot: Duties vor Netz; Safe-Mode mit echter Wirkung (70% Failsafe, MQTT aus)"`

---

### Task 6: OTA-Anti-Brick (Rollback-Hook + Health-Window) + sauberer Disconnect

**Files:**
- Modify: `fan_controller_v4.0/fan_controller_v4.0.ino` (Includes, Globals, setup, loop, handleOTA, /reboot)

- [ ] **Step 1: Hook + Erkennung** — nach den bestehenden Includes (`esp_task_wdt.h`):

```cpp
#include "esp_ota_ops.h"

// [Spec §3.2] Boot-Auto-Validierung aufschieben: WIR markieren das Image erst
// nach gesundem Lauf-Fenster als gueltig. Crash/WDT vorher => Bootloader-Rollback.
extern "C" bool verifyRollbackLater() { return true; }

static bool     g_otaPendingVerify = false;
static const uint32_t OTA_HEALTH_MS = 90000;
```

In `setup()` direkt nach dem WDT-Block (Task 4):
```cpp
  {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
      g_otaPendingVerify = true;
      LOGW("OTA", "Image PENDING_VERIFY - Health-Window 90s laeuft");
    }
  }
```

- [ ] **Step 2: Health-Window in loop()** — direkt nach `const uint32_t now = millis();`:

```cpp
  if (g_otaPendingVerify && now > OTA_HEALTH_MS) {
    g_otaPendingVerify = false;
    esp_ota_mark_app_valid_cancel_rollback();
    LOGI("OTA", "Image nach 90s als VALID markiert (Rollback abgewaehlt)");
  }
```
(millis() ist ab Boot monoton; in den ersten 90 s ist kein Wrap möglich.)

- [ ] **Step 3: Sauberer MQTT-Abgang vor Restarts** — neue Funktion vor `handleOTA()`:

```cpp
static void prepareRestart() {
  if (mqtt.connected()) {
    String t = topicDev() + "/status";
    mqtt.publish(t.c_str(), "offline", true);
    mqtt.disconnect();
  }
  persistLogTail();
}
```
In `handleOTA()` Erfolgspfad: `c.flush(); delay(300); ESP.restart();` →
`c.flush(); prepareRestart(); delay(300); ESP.restart();`
Im `/reboot`-Handler ebenso `prepareRestart();` vor `ESP.restart();` einfügen.

- [ ] **Step 4: Build-Check + Commit** `git commit -am "OTA-Anti-Brick: verifyRollbackLater + 90s-Health-Window; offline+disconnect vor Restart"`

---

### Task 7: NVS-Schonung (Log-Persistenz) + Log-Diät

**Files:**
- Modify: `fan_controller_v4.0/fan_controller_v4.0.ino` (Konstante, setup, handleClient)

- [ ] **Step 1: Cadence 8 s → 10 min** (Z.61): `LOG_FLUSH_MS = 8000` → `600000`.

- [ ] **Step 2: Shutdown-Handler** — in `setup()` nach `bootTrackInit();`:

```cpp
  // Log-Tail bei jedem geordneten Reboot sichern (ESP.restart ruft Handler).
  static auto logShutdown = []() { persistLogTail(); };
  esp_register_shutdown_handler((shutdown_handler_t)+logShutdown);
```

- [ ] **Step 3: HTTP-Request-Logging entfernen** — in `handleClient()` die Zeile
`LOGI("HTTP", method + " " + path + (query.length() ? "?" + query : ""));` ersatzlos streichen (Spec §3.5: pro-Request-Logs = NVS-Wear + Heap-Churn).

- [ ] **Step 4: Build-Check + Commit** `git commit -am "NVS-Schonung: Log-Tail 10min+Shutdown-Handler, kein Request-Logging"`

---

### Task 8: HTTP-Härtung (Limits, Chunked-Writer, 404)

**Files:**
- Modify: `fan_controller_v4.0/fan_controller_v4.0.ino` (readHeaders, handleFormPost, handleOTA, handleLogTxt, Router)

- [ ] **Step 1: Header-Limit** — `readHeaders()` (Z.974) Signatur + Schleife:

```cpp
static bool readHeaders(EthernetClient &c, size_t &contentLength, String &contentType) {
  contentLength = 0;
  contentType   = "";
  bool teChunked = false;
  String h;
  uint8_t nHdr = 0;
  bool terminated = false;
  while (nHdr++ < 32 && readLine(c, h, CLIENT_RD_TIMEOUT)) {
    if (h.length() == 0) { terminated = true; break; }
    String hl = h; hl.toLowerCase();
    if (hl.startsWith(F("content-length:"))) {
      String s = h.substring(h.indexOf(':') + 1); s.trim();
      contentLength = s.toInt();
    } else if (hl.startsWith(F("content-type:"))) {
      String s = h.substring(h.indexOf(':') + 1); s.trim();
      contentType = s;
    } else if (hl.startsWith(F("transfer-encoding:")) && hl.indexOf(F("chunked")) >= 0) {
      teChunked = true;
    }
  }
  if (teChunked) contentLength = 0;
  return terminated;
}
```
Aufrufer in `handleClient()`: `readHeaders(c, contentLength, contentType);` →
```cpp
  if (!readHeaders(c, contentLength, contentType)) { httpSend400(c, "bad headers"); return; }
```

- [ ] **Step 2: Body-Cap für Formulare** — `handleFormPost()` als erste Zeile:

```cpp
  if (contentLength == 0 || contentLength > 4096) { httpSend400(c, "bad length"); return false; }
```
`handleOTA()` nach dem `contentLength == 0`-Check zusätzlich:
```cpp
  if (contentLength > 0x200000) {  // > 2 MB passt in keinen App-Slot
    httpSendHeaderOK(c, "application/json; charset=UTF-8");
    c.print(F("{\"ok\":false,\"error\":\"too large\"}"));
    return false;
  }
```

- [ ] **Step 3: Chunked-Writer** (Lib-Truncation-Fix, CLAUDE.md §6.11) — vor `handleLogTxt()`:

```cpp
// EthernetClient::write() kappt still bei Socket-Puffergroesse (4KB) und meldet
// trotzdem Erfolg -> NIE mehr als 1KB pro write() schicken.
static void printChunked(EthernetClient &c, const char *s, size_t len) {
  while (len > 0) {
    size_t n = len < 1024 ? len : 1024;
    if (c.write((const uint8_t *)s, n) == 0) return;
    s += n; len -= n;
    feedLoopWDT();
  }
}
```
`handleLogTxt`/`handlePrevLogTxt` umstellen:
```cpp
static void handleLogTxt(EthernetClient &c)     { httpSendHeaderOK(c, "text/plain; charset=UTF-8"); printChunked(c, gLogBuf.c_str(), gLogBuf.length()); }
static void handlePrevLogTxt(EthernetClient &c) { httpSendHeaderOK(c, "text/plain; charset=UTF-8"); printChunked(c, gPrevLogTail.c_str(), gPrevLogTail.length()); }
```

- [ ] **Step 4: 404** — neue Funktion neben `httpSend400()`:
```cpp
static void httpSend404(EthernetClient &c) {
  c.println(F("HTTP/1.1 404 Not Found"));
  c.println(F("Content-Type: text/plain; charset=UTF-8"));
  c.println(F("Connection: close"));
  c.println(); c.println(F("Not found"));
}
```
Letzte Zeile von `handleClient()`: `httpSend400(c, "Not found");` → `httpSend404(c);`

- [ ] **Step 5: Build-Check + Commit** `git commit -am "HTTP-Haertung: Header-Limit, Body-Caps, Chunked-Writer (Truncation-Fix), 404"`

---

### Task 9: MQTT — flaches Schema (speed / set / rpm)

**Files:**
- Modify: `fan_controller_v4.0/fan_controller_v4.0.ino` (topicFan, mqttPublishPWM→mqttPublishSpeed, mqttCallback, applyDo)

- [ ] **Step 1: Topics** — `topicFan()` (Z.377):
```cpp
static String topicFan(uint8_t i) { return topicDev() + "/" + sanitizeName(fans[i].name); }
```

- [ ] **Step 2: speed statt pct** — `mqttPublishPWM()` umbenennen in `mqttPublishSpeed()` (auch Prototyp Z.383 und Aufrufer: `onDutyChanged`, `mqttEnsureConnected`, `applyDo` Name-Change); Topic-Zeile:
`String t = topicFan(i) + "/pct";` → `String t = topicFan(i) + "/speed";`

- [ ] **Step 3: Callback ans flache Schema anpassen** — `mqttCallback()` komplett ersetzen:

```cpp
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String t(topic);
  String pre = topicDev() + "/";
  if (!t.startsWith(pre) || !t.endsWith(F("/set"))) return;
  String fanKey = t.substring(pre.length(), t.length() - 4);  // ".../<name>/set"

  char buf[8];
  unsigned int n = min(length, (unsigned)sizeof(buf) - 1), j = 0;
  for (unsigned int i = 0; i < n; i++) if (payload[i] >= 32) buf[j++] = (char)payload[i];
  buf[j] = 0;
  int pct = constrain(atoi(buf), 0, 100);

  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i)) continue;
    if (sanitizeName(fans[i].name) == fanKey) {
      dutyEnqueue(i, dutyFromPct((uint8_t)pct));
      LOGI("MQTT", String("set ") + fans[i].name + " -> " + pct + "%");
      return;
    }
  }
  LOGW("MQTT", "set fuer unbekannten Luefter");
}
```

- [ ] **Step 4: Delete/Rename-Aufräumpfade** — in `applyDo()` beide `baseOld`/`oldBase`-Konstruktionen:
`… + "/fan/" + sanitizeName(…)` → `… + "/" + sanitizeName(…)` und
`mqtt.publish((baseOld + "/pct").c_str(), "", true);` → `…"/speed"…` (beide Stellen).

- [ ] **Step 5: Build-Check + Commit** `git commit -am "MQTT flach: <prefix>/<dev>/<fan>/{speed,set,rpm}"`

---

### Task 10: Web-UI (ui/index.html) + Build-Tooling + Mock-API

**Files:**
- Create: `ui/index.html`, `tools/build_ui.sh`, `tools/mock_api.py`
- Create (generiert): `fan_controller_v4.0/ui_asset.h`

- [ ] **Step 1: `tools/build_ui.sh`**

```bash
#!/bin/sh
# ui/index.html -> gzip -> fan_controller_v4.0/ui_asset.h
set -e
cd "$(dirname "$0")/.."
gzip -9 -n -c ui/index.html > /tmp/ui.gz
{
  echo "// GENERIERT von tools/build_ui.sh - nicht von Hand editieren"
  echo "#pragma once"
  echo "#include <pgmspace.h>"
  echo "static const uint8_t UI_ASSET[] PROGMEM = {"
  xxd -i < /tmp/ui.gz
  echo "};"
  echo "static const unsigned int UI_ASSET_LEN = sizeof(UI_ASSET);"
} > fan_controller_v4.0/ui_asset.h
echo "ui_asset.h erzeugt: $(wc -c < /tmp/ui.gz) Bytes gzip"
```
`chmod +x tools/build_ui.sh`

- [ ] **Step 2: `tools/mock_api.py`**

```python
#!/usr/bin/env python3
"""UI-Entwicklung ohne Geraet: python3 tools/mock_api.py -> http://127.0.0.1:8077"""
import json, random, http.server

FANS = [
  {"index":0,"name":"mac","pwm":133,"pct":52,"rpm":724,"pwmPin":40,"tachPin":37,"fault":0,"validated":True},
  {"index":1,"name":"unifi","pwm":140,"pct":55,"rpm":811,"pwmPin":42,"tachPin":35,"fault":0,"validated":True},
  {"index":2,"name":"usv","pwm":128,"pct":50,"rpm":747,"pwmPin":41,"tachPin":36,"fault":3,"validated":True},
  {"index":3,"name":"nas","pwm":148,"pct":58,"rpm":858,"pwmPin":47,"tachPin":38,"fault":0,"validated":True},
]

class H(http.server.BaseHTTPRequestHandler):
    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.end_headers()
        self.wfile.write(body.encode())
    def do_GET(self):
        if self.path.startswith("/api/status"):
            for f in FANS:
                f["rpm"] = max(0, f["rpm"] + random.randint(-15, 15))
            self._send(200, "application/json", json.dumps({
                "rev":1,"device":"ws-s3eth-MOCK","ip":"10.47.88.239","mqtt_connected":True,
                "boot_count":42,"safe_mode":False,"reset_reason":"POWERON",
                "min_free_heap":201000,"largest_block":198000,"uptime_s":361445,
                "wdt":True,"ota_pending":False,"crash_streak":0,
                "free_pwm":[1,2,8,15,16,17,18,21,33,34,39,48],
                "free_tach":[1,2,8,15,16,17,18,21,33,34,39,48],
                "fans":FANS}))
        elif self.path == "/log.txt":
            self._send(200, "text/plain", "[T+0001.000s #42] [I] BOOT: mock log\n" * 30)
        elif self.path == "/prevlog.txt":
            self._send(200, "text/plain", "[prev boot] mock tail\n")
        else:
            self._send(200, "text/html", open("ui/index.html").read())
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self._send(200, "application/json", '{"ok":true,"idx":4}')
    def log_message(self, *a): pass

print("Mock-API: http://127.0.0.1:8077")
http.server.HTTPServer(("127.0.0.1", 8077), H).serve_forever()
```

- [ ] **Step 3: `ui/index.html`** — komplette Datei (Control-Room-Stil A, Tab-Layout B):

```html
<!doctype html><html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FAN-CTRL</title>
<style>
:root{--bg:#0a0e12;--panel:#0f141b;--bd:#1f2937;--fg:#e2e8f0;--mut:#64748b;--dim:#475569;
--ok:#22c55e;--warn:#f59e0b;--err:#ef4444;--acc:#38bdf8}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:14px}
.wrap{max-width:1000px;margin:0 auto;padding:14px}
header{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;
border-bottom:1px solid var(--bd);padding-bottom:10px;margin-bottom:2px}
h1{font-size:15px;margin:0;letter-spacing:.1em}h1 small{color:var(--dim);font-weight:400}
.leds{display:flex;gap:12px;font-size:11px;color:var(--mut)}
.led::before{content:"●";margin-right:4px;color:var(--err)}.led.on::before{color:var(--ok)}
#safebanner{display:none;background:#3a1313;border:1px solid var(--err);color:#fecaca;
padding:10px;border-radius:6px;margin:10px 0;font-weight:700}
nav{display:flex;gap:2px;margin-top:12px}
nav button{font:inherit;font-size:12px;letter-spacing:.08em;padding:7px 14px;cursor:pointer;
background:none;border:1px solid var(--bd);border-bottom:none;border-radius:6px 6px 0 0;color:var(--dim)}
nav button.act{background:var(--panel);color:var(--fg)}
main{background:var(--panel);border:1px solid var(--bd);border-radius:0 6px 6px 6px;padding:14px;min-height:300px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:10px}
.fan{background:var(--bg);border:1px solid var(--bd);border-left:3px solid var(--ok);border-radius:6px;padding:10px}
.fan.f3{border-left-color:var(--warn)}.fan.f4,.fan.bad{border-left-color:var(--err)}
.fan .top{display:flex;justify-content:space-between;font-size:11px;color:#94a3b8}
.fan .rpm{font-size:26px;font-weight:700;margin:4px 0}.fan .rpm small{font-size:11px;color:var(--dim)}
.fan input[type=range]{width:100%;accent-color:var(--acc)}
.fan .pct{font-size:11px;color:var(--mut)}
.badge{font-size:10px;padding:1px 7px;border-radius:8px;border:1px solid var(--bd)}
.b-ok{color:#a7f3d0}.b-warn{color:#fde68a}.b-err{color:#fecaca}
.kv{display:grid;grid-template-columns:max-content 1fr;gap:6px 18px;font-size:13px}
.kv b{color:var(--mut);font-weight:400}
pre{background:var(--bg);border:1px solid var(--bd);border-radius:6px;padding:10px;
white-space:pre-wrap;max-height:45vh;overflow:auto;font-size:11px}
label{display:block;color:var(--mut);font-size:11px;margin:10px 0 3px;letter-spacing:.05em}
input[type=text],input[type=password],input[type=number],select{width:100%;max-width:340px;
font:inherit;padding:7px;background:var(--bg);border:1px solid var(--bd);border-radius:6px;color:var(--fg)}
.btn{font:inherit;font-size:12px;padding:7px 14px;border:1px solid var(--bd);border-radius:6px;
background:#16202e;color:var(--fg);cursor:pointer;margin:10px 6px 0 0}
.btn.danger{background:#2b1515;border-color:#4b1b1b}
.row{display:flex;gap:16px;flex-wrap:wrap}.col{flex:1;min-width:260px}
.list{border:1px solid var(--bd);border-radius:6px;margin-top:8px}
.list>div{display:flex;justify-content:space-between;align-items:center;padding:8px 10px;border-bottom:1px solid var(--bd)}
.list>div:last-child{border-bottom:none}
.muted{color:var(--mut)}.small{font-size:11px}
progress{width:100%;accent-color:var(--acc)}
hr{border:0;border-top:1px solid var(--bd);margin:16px 0}
#toast{position:fixed;bottom:16px;right:16px;background:#16202e;border:1px solid var(--bd);
border-radius:6px;padding:10px 14px;display:none;font-size:12px}
</style></head><body><div class="wrap">
<header>
  <h1>FAN-CTRL <small id="dev">—</small></h1>
  <div class="leds">
    <span class="led" id="l-eth">ETH</span><span class="led" id="l-mqtt">MQTT</span>
    <span class="led" id="l-wdt">WDT</span><span id="l-ip" class="muted"></span>
  </div>
</header>
<div id="safebanner">⚠ SAFE MODE — Crash-Loop erkannt. Lüfter laufen mit 70 % Failsafe, MQTT ist aus.</div>
<nav>
  <button data-t="dash" class="act" onclick="tab('dash')">DASHBOARD</button>
  <button data-t="sys" onclick="tab('sys')">SYSTEM</button>
  <button data-t="cfg" onclick="tab('cfg')">EINSTELLUNGEN</button>
  <button data-t="fw" onclick="tab('fw')">FIRMWARE</button>
</nav>
<main>
  <section id="t-dash"><div class="grid" id="fangrid"></div>
    <p class="muted small" id="nofans" style="display:none">Keine Lüfter konfiguriert — Tab EINSTELLUNGEN.</p></section>

  <section id="t-sys" style="display:none">
    <div class="kv" id="syskv"></div>
    <div><button class="btn" onclick="loadLogs()">Logs laden</button>
      <button class="btn" onclick="api('/api/reboot',{},()=>toast('Reboot…'))">Reboot</button>
      <button class="btn danger" id="srbtn" style="display:none"
        onclick="api('/api/safemode/reset',{},()=>toast('Safe-Mode zurückgesetzt — Reboot empfohlen'))">Safe-Mode-Reset</button></div>
    <label>LOG (aktueller Boot)</label><pre id="log">—</pre>
    <label>LOG (vorheriger Boot)</label><pre id="prevlog">—</pre></section>

  <section id="t-cfg" style="display:none"><div class="row">
    <div class="col"><h3 class="small" style="letter-spacing:.1em">LÜFTER</h3>
      <div class="list" id="fanlist"></div>
      <button class="btn" onclick="api('/api/fan/new',{},j=>{poll(true);toast('Slot '+j.idx+' angelegt')})">+ Neuer Lüfter</button>
      <div id="editbox" style="display:none"><hr>
        <h3 class="small">LÜFTER #<span id="e-idx"></span> BEARBEITEN</h3>
        <label>NAME (a-z 0-9 _ -)</label><input type="text" id="e-name" maxlength="19">
        <label>PWM-PIN</label><select id="e-pwm"></select>
        <label>TACHO-PIN</label><select id="e-tach"></select>
        <label><input type="checkbox" id="e-inv" style="width:auto"> PWM invertieren</label>
        <button class="btn" onclick="saveFan()">Speichern</button>
        <button class="btn danger" onclick="delFan()">Löschen</button>
        <label>KALIBRIERUNG: MIN-START (%)</label><input type="number" id="e-cmin" min="0" max="100" value="0">
        <label>NOTIZ</label><input type="text" id="e-note" maxlength="39">
        <button class="btn" onclick="saveCalib()">Kalibrierung speichern</button></div></div>
    <div class="col"><h3 class="small" style="letter-spacing:.1em">MQTT</h3>
      <label><input type="checkbox" id="m-en" style="width:auto"> aktiv</label>
      <label>HOST</label><input type="text" id="m-host">
      <label>PORT</label><input type="number" id="m-port" value="1883">
      <label>USER</label><input type="text" id="m-user">
      <label>PASSWORT</label><input type="password" id="m-pass" placeholder="(unverändert lassen = leer)">
      <label>PREFIX</label><input type="text" id="m-prefix" maxlength="15">
      <button class="btn" onclick="saveMqtt()">MQTT speichern</button>
      <p class="muted small">Topics: prefix/&lt;device&gt;/&lt;name&gt;/speed·set·rpm + …/status</p></div>
  </div></section>

  <section id="t-fw" style="display:none">
    <p class="muted small">.bin wählen — Update läuft in den inaktiven Slot; das neue Image
    muss 90 s gesund laufen, sonst Rollback. Notfall: curl POST /ota.</p>
    <input type="file" id="fwfile" accept=".bin">
    <button class="btn" id="fwbtn" onclick="flash()">Upload &amp; Flash</button>
    <div id="fwbox" style="display:none"><progress id="fwpr" max="100" value="0"></progress>
      <div class="muted small" id="fwst">Bereit…</div></div>
    <div class="kv" style="margin-top:12px"><b>OTA-Status</b><span id="otastate">—</span></div></section>
</main>
<div id="toast"></div>
<script>
"use strict";
const $=id=>document.getElementById(id);
const esc=s=>String(s??"").replace(/[&<>"']/g,m=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[m]));
let J=null,editIdx=-1,fail=0,mqttLoaded=false;
function tab(t){document.querySelectorAll("nav button").forEach(b=>b.classList.toggle("act",b.dataset.t===t));
 ["dash","sys","cfg","fw"].forEach(x=>$("t-"+x).style.display=x===t?"":"none");}
function toast(m){const t=$("toast");t.textContent=m;t.style.display="block";setTimeout(()=>t.style.display="none",2500);}
function api(url,data,okCb){const body=Object.entries(data).map(([k,v])=>k+"="+encodeURIComponent(v)).join("&");
 fetch(url,{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body})
 .then(r=>r.json()).then(j=>{if(j.ok)okCb&&okCb(j);else toast("Fehler: "+(j.error||"?"));})
 .catch(()=>toast("Keine Verbindung"));}
const sliders={t:{},send(i,pct){clearTimeout(this.t[i]);this.t[i]=setTimeout(()=>api("/api/fan/set",{idx:i,pct}),150);}};
function badge(f){if(!f.validated)return'<span class="badge b-err">CONFIG</span>';
 if(f.fault===0)return'<span class="badge b-ok">RUN</span>';
 if(f.fault===3)return'<span class="badge b-warn">NO TACH</span>';
 if(f.fault===4)return'<span class="badge b-warn">STORM</span>';
 return'<span class="badge b-err">FAULT '+f.fault+"</span>";}
function renderDash(){const g=$("fangrid");g.innerHTML="";const fans=J.fans||[];
 $("nofans").style.display=fans.length?"none":"";
 fans.forEach(f=>{const d=document.createElement("div");
  d.className="fan f"+f.fault+(f.validated?"":" bad");
  d.innerHTML=`<div class="top"><span>${String(f.index+1).padStart(2,"0")} ${esc(f.name).toUpperCase()}</span>${badge(f)}</div>`+
   `<div class="rpm">${f.rpm}<small> rpm</small></div>`+
   `<input type="range" min="0" max="100" value="${f.pct}" oninput="this.nextElementSibling.firstChild.textContent=this.value;sliders.send(${f.index},this.value)">`+
   `<div class="pct"><span>${f.pct}</span>% PWM · Pin ${f.pwmPin}/${f.tachPin}</div>`;
  g.appendChild(d);});}
function renderSys(){const fmt=s=>{const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  return(d?d+"d ":"")+h+"h "+m+"m"};
 $("syskv").innerHTML=[["Device",esc(J.device)],["IP",esc(J.ip)],["Uptime",fmt(J.uptime_s||0)],
  ["Boot-Count",J.boot_count],["Reset-Grund",esc(J.reset_reason)],["Crash-Streak",J.crash_streak],
  ["Watchdog",J.wdt?"AKTIV":"AUS!"],["OTA",J.ota_pending?"PENDING_VERIFY (Health-Window)":"validiert"],
  ["Min Free Heap",J.min_free_heap],["Largest Block",J.largest_block]]
  .map(([k,v])=>`<b>${k}</b><span>${v}</span>`).join("");
 $("srbtn").style.display=J.safe_mode?"":"none";
 $("otastate").textContent=J.ota_pending?"PENDING_VERIFY — wird nach 90 s gesundem Lauf validiert":"Image validiert";}
function renderCfg(){const l=$("fanlist");l.innerHTML="";(J.fans||[]).forEach(f=>{const d=document.createElement("div");
  d.innerHTML=`<span>${String(f.index+1).padStart(2,"0")} ${esc(f.name)} <span class="muted small">pwm${f.pwmPin} tach${f.tachPin}</span></span>`+
   `<button class="btn" style="margin:0" onclick="editFan(${f.index})">edit</button>`;
  l.appendChild(d);});
 if(!mqttLoaded&&J.mqtt){mqttLoaded=true;$("m-en").checked=!!J.mqtt.enabled;$("m-host").value=J.mqtt.host||"";
  $("m-port").value=J.mqtt.port||1883;$("m-user").value=J.mqtt.user||"";$("m-prefix").value=J.mqtt.prefix||"esp";}}
function pinOpts(sel,list,current){sel.innerHTML="";[...new Set([current,...list])].filter(p=>p!==undefined&&p!==255)
 .forEach(p=>{const o=document.createElement("option");o.value=p;o.textContent="GPIO "+p;
  if(p===current)o.selected=true;sel.appendChild(o);});}
function editFan(i){editIdx=i;const f=(J.fans||[]).find(x=>x.index===i);if(!f)return;
 $("editbox").style.display="";$("e-idx").textContent=i;$("e-name").value=f.name;
 pinOpts($("e-pwm"),J.free_pwm||[],f.pwmPin);pinOpts($("e-tach"),J.free_tach||[],f.tachPin);
 $("e-inv").checked=false;$("e-cmin").value=f.cmin??0;$("e-note").value=f.cnote??"";}
function saveFan(){api("/api/fan/save",{idx:editIdx,name:$("e-name").value,pwm:$("e-pwm").value,
 tach:$("e-tach").value,inv:$("e-inv").checked?1:0},()=>{toast("Gespeichert");poll(true);});}
function delFan(){if(confirm("Lüfter wirklich löschen?"))
 api("/api/fan/delete",{idx:editIdx},()=>{$("editbox").style.display="none";toast("Gelöscht");poll(true);});}
function saveCalib(){api("/api/calib",{idx:editIdx,cmin:$("e-cmin").value,cnote:$("e-note").value},()=>toast("Kalibrierung gespeichert"));}
function saveMqtt(){api("/api/mqtt",{enabled:$("m-en").checked?1:0,host:$("m-host").value,port:$("m-port").value,
 user:$("m-user").value,pass:$("m-pass").value,prefix:$("m-prefix").value},()=>toast("MQTT gespeichert"));}
function loadLogs(){fetch("/log.txt").then(r=>r.text()).then(t=>$("log").textContent=t.split("\n").slice(-300).join("\n"));
 fetch("/prevlog.txt").then(r=>r.text()).then(t=>$("prevlog").textContent=t);}
function flash(){const f=$("fwfile").files[0];if(!f){toast("Bitte .bin wählen");return;}
 $("fwbtn").disabled=true;$("fwbox").style.display="";
 const x=new XMLHttpRequest();x.open("POST","/ota",true);
 x.setRequestHeader("Content-Type","application/octet-stream");
 x.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);
  $("fwpr").value=p;$("fwst").textContent="Upload "+p+"%";}};
 x.onerror=()=>{$("fwst").textContent="Netzwerkfehler";$("fwbtn").disabled=false;};
 x.onload=()=>{try{const j=JSON.parse(x.responseText||"{}");
  if(j.ok){$("fwst").textContent="Flash OK — Reboot, warte auf Gerät…";watchReboot();}
  else{$("fwst").textContent="Fehler: "+(j.error||"?");$("fwbtn").disabled=false;}}
  catch(e){$("fwst").textContent="Antwort unlesbar";$("fwbtn").disabled=false;}};
 x.send(f);}
function watchReboot(){const b0=J?J.boot_count:0,t0=Date.now();
 const iv=setInterval(()=>{fetch("/api/status",{cache:"no-store"}).then(r=>r.json()).then(j=>{
  if(j.boot_count!==b0){clearInterval(iv);$("fwst").textContent="Online: Boot#"+j.boot_count+
   (j.ota_pending?" — Health-Window läuft (90 s)":"");$("fwbtn").disabled=false;}})
  .catch(()=>{if(Date.now()-t0>120000){clearInterval(iv);$("fwst").textContent="Gerät meldet sich nicht.";}});},1500);}
function poll(force){fetch("/api/status",{cache:"no-store"}).then(r=>r.json()).then(j=>{
  J=j;fail=0;$("l-eth").classList.add("on");
  $("l-mqtt").classList.toggle("on",!!j.mqtt_connected);$("l-wdt").classList.toggle("on",!!j.wdt);
  $("dev").textContent=j.device||"—";$("l-ip").textContent=j.ip||"";
  $("safebanner").style.display=j.safe_mode?"":"none";
  renderDash();renderSys();if(force||$("t-cfg").style.display==="")renderCfg();})
 .catch(()=>{if(++fail>=3){$("l-eth").classList.remove("on");$("l-mqtt").classList.remove("on");}});}
setInterval(poll,1500);poll(true);
</script></div></body></html>
```

- [ ] **Step 4: Gegen Mock testen**

```bash
python3 tools/mock_api.py &
open http://127.0.0.1:8077
```
Prüfen: alle 4 Tabs rendern; Fan-Karten zeigen RPM-Live-Wackeln; Slider sendet POST; USV zeigt „NO TACH"-Badge; Logs laden; Mock killen (`kill %1`).

- [ ] **Step 5: Asset bauen** — `tools/build_ui.sh`, Erwartet: „ui_asset.h erzeugt: N Bytes gzip" mit N ≤ 15000.

- [ ] **Step 6: Commit** `git add ui tools fan_controller_v4.0/ui_asset.h && git commit -m "Web-UI: Control-Room-App (Tabs), Build-Script, Mock-API"`

---

### Task 11: JSON-API + Router-Umbau (alte HTML-Seiten raus)

**Files:**
- Modify: `fan_controller_v4.0/fan_controller_v4.0.ino`

- [ ] **Step 1: UI-Asset einbinden** — bei den Includes: `#include "ui_asset.h"`; vor dem Router:

```cpp
static void sendUiAsset(EthernetClient &c) {
  c.println(F("HTTP/1.1 200 OK"));
  c.println(F("Content-Type: text/html; charset=UTF-8"));
  c.println(F("Content-Encoding: gzip"));
  c.print(F("Content-Length: ")); c.println(UI_ASSET_LEN);
  c.println(F("Cache-Control: no-cache"));
  c.println(F("Connection: close"));
  c.println();
  size_t off = 0;
  while (off < UI_ASSET_LEN) {
    size_t n = (UI_ASSET_LEN - off) < 1024 ? (UI_ASSET_LEN - off) : 1024;
    c.write(UI_ASSET + off, n);
    off += n;
    feedLoopWDT();
  }
}
```

- [ ] **Step 2: JSON-Antwort-Helfer** — neben `httpSend400()`:

```cpp
static void apiOk(EthernetClient &c) {
  httpSendHeaderOK(c, "application/json; charset=UTF-8");
  c.print(F("{\"ok\":true}"));
}
static void apiErr(EthernetClient &c, const char *msg) {  // nur statische msg!
  c.println(F("HTTP/1.1 400 Bad Request"));
  c.println(F("Content-Type: application/json; charset=UTF-8"));
  c.println(F("Connection: close"));
  c.println();
  c.print(F("{\"ok\":false,\"error\":\"")); c.print(msg); c.print(F("\"}"));
}
```

- [ ] **Step 3: API-Handler** — die drei alten Save-Handler (`handleMQTTConfigSave`, `handleFanSave`, `handleCalibSave`) ersetzen durch:

```cpp
static void apiFanSet(EthernetClient &c, const String &body) {
  String s; long idx = -1, pct = -1;
  if (formGet(body, F("idx"), s)) idx = s.toInt();
  if (formGet(body, F("pct"), s)) pct = s.toInt();
  if (idx < 0 || idx >= MAX_FANS || !fanPresentIdx((uint8_t)idx) || pct < 0 || pct > 100) {
    apiErr(c, "bad idx/pct"); return;
  }
  dutyEnqueue((uint8_t)idx, dutyFromPct((uint8_t)pct));
  apiOk(c);
}

static void apiFanSave(EthernetClient &c, const String &body) {
  String s;
  if (!formGet(body, F("fan"), s) && !formGet(body, F("idx"), s)) { apiErr(c, "missing idx"); return; }
  int idx = s.toInt();
  if (idx < 0 || idx >= MAX_FANS) { apiErr(c, "bad idx"); return; }
  Fan &f = fans[idx];

  String newName = String(f.name);
  if (formGet(body, F("name"), s)) newName = s;
  String cleanName = sanitizeName(newName);
  if (!fanNameValid(cleanName.c_str())) { apiErr(c, "invalid name"); return; }
  for (uint8_t i = 0; i < MAX_FANS; i++) {  // Topic-Kollision (Spec §4)
    if ((int)i == idx || !fanPresentIdx(i)) continue;
    if (sanitizeName(fans[i].name) == cleanName) { apiErr(c, "name in use"); return; }
  }

  bool inv = false;
  if (formGet(body, F("inv"), s)) inv = (s.toInt() == 1);
  uint8_t newPwm = f.pwmPin, newTac = f.tachPin;
  if (formGet(body, F("pwm"),  s)) newPwm = (uint8_t)constrain(s.toInt(), 0, 255);
  if (formGet(body, F("tach"), s)) newTac = (uint8_t)constrain(s.toInt(), 0, 255);
  if (newPwm != f.pwmPin && !validPwmForFan(newPwm,  (int8_t)idx)) { apiErr(c, "pwm pin invalid/busy"); return; }
  if (newTac != f.tachPin && !validTachForFan(newTac, (int8_t)idx)) { apiErr(c, "tach pin invalid/busy"); return; }

  ApplyJob j; j.idx = (uint8_t)idx;
  if (sanitizeName(String(f.name)) != cleanName) { safeStrcpy(j.name, sizeof(j.name), cleanName); j.nameChanged = true; }
  if (inv != f.invertPwm) { j.invert = inv; j.invChanged = true; }
  if (newPwm != f.pwmPin || newTac != f.tachPin) { j.pwmPin = newPwm; j.tachPin = newTac; j.pinsChanged = true; }
  if (j.nameChanged || j.invChanged || j.pinsChanged) applyQueue((uint8_t)idx, j);
  apiOk(c);
}

static void apiFanDelete(EthernetClient &c, const String &body) {
  String s;
  if (!formGet(body, F("idx"), s)) { apiErr(c, "missing idx"); return; }
  int idx = s.toInt();
  if (idx < 0 || idx >= MAX_FANS || !fanPresentIdx((uint8_t)idx)) { apiErr(c, "bad idx"); return; }
  ApplyJob j; j.idx = (uint8_t)idx; j.deleteFan = true;
  applyQueue((uint8_t)idx, j);
  apiOk(c);
}

static void apiFanNew(EthernetClient &c, const String &body) {
  (void)body;
  int idx = -1;  // [B8] freie Slots an pwmPin==0xFF erkennen
  for (uint8_t i = 0; i < MAX_FANS; i++) if (fans[i].pwmPin == 0xFF) { idx = i; break; }
  if (idx < 0) { apiErr(c, "no free slot"); return; }
  fanInitDefaults((uint8_t)idx);
  clearFanNVS((uint8_t)idx);
  safeStrcpy(fans[idx].name, sizeof(fans[idx].name), String("fan") + String(idx + 1));
  g_stateRev++;
  httpSendHeaderOK(c, "application/json; charset=UTF-8");
  c.print(F("{\"ok\":true,\"idx\":")); c.print(idx); c.print(F("}"));
}

static void apiCalib(EthernetClient &c, const String &body) {
  String s;
  if (!formGet(body, F("idx"), s)) { apiErr(c, "missing idx"); return; }
  int idx = s.toInt();
  if (idx < 0 || idx >= MAX_FANS || !fanPresentIdx((uint8_t)idx)) { apiErr(c, "bad idx"); return; }
  Fan &f = fans[idx];
  if (formGet(body, F("cmin"), s)) f.calMinStart = dutyFromPct((uint8_t)constrain(s.toInt(), 0, 100));
  if (formGet(body, F("cnote"), s)) safeStrcpy(f.calNote, sizeof(f.calNote), s);
  Preferences p; p.begin("fans", false);
  String k = "f" + String(idx) + "_";
  p.putUChar((k + "cmin").c_str(), f.calMinStart);
  p.putString((k + "cnote").c_str(), f.calNote);
  p.end();
  apiOk(c);
}

static void apiMqttSave(EthernetClient &c, const String &body) {
  String s;
  mqttConfig.enabled = body.indexOf(F("enabled=1")) >= 0;
  if (formGet(body, F("host"),   s)) safeStrcpy(mqttConfig.host,   sizeof(mqttConfig.host),   s);
  if (formGet(body, F("port"),   s)) mqttConfig.port = (uint16_t)constrain(s.toInt(), 1, 65535);
  if (formGet(body, F("user"),   s)) safeStrcpy(mqttConfig.user,   sizeof(mqttConfig.user),   s);
  if (formGet(body, F("pass"),   s) && s.length() > 0) safeStrcpy(mqttConfig.pass, sizeof(mqttConfig.pass), s);
  if (formGet(body, F("prefix"), s)) safeStrcpy(mqttConfig.prefix, sizeof(mqttConfig.prefix), s);
  Preferences p; p.begin("mqtt", false);
  p.putBool("enabled", mqttConfig.enabled);
  p.putString("host",   mqttConfig.host);
  p.putUShort("port",   mqttConfig.port);
  p.putString("user",   mqttConfig.user);
  p.putString("pass",   mqttConfig.pass);
  p.putString("prefix", mqttConfig.prefix);
  p.end();
  mqtt.disconnect();  // Reconnect mit neuer Konfig via Backoff
  apiOk(c);
}

static void apiSafeModeReset(EthernetClient &c, const String &body) {
  (void)body;
  Preferences p; p.begin("sys", false);
  p.putUChar("crash_streak", 0);
  p.putUChar("safe_mode", 0);
  p.end();
  apiOk(c);  // wirkt vollstaendig nach dem naechsten Reboot
}

static void apiReboot(EthernetClient &c, const String &body) {
  (void)body;
  apiOk(c);
  c.flush();
  prepareRestart();
  delay(200);
  ESP.restart();
}
```

- [ ] **Step 4: sendJsonStatus erweitern** — Includes oben ergänzen: `#include "esp_heap_caps.h"` und `#include "esp_timer.h"`. In `bootTrackInit()` den Streak global merken: vor der Funktion `static uint8_t g_crashStreak = 0;`, in der Funktion nach der Streak-Berechnung `g_crashStreak = crashStreak;`. In `sendJsonStatus()` nach der `min_free_heap`-Zeile einfügen:

```cpp
  c.print(F(",\"largest_block\":")); c.print(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  c.print(F(",\"uptime_s\":")); c.print((uint32_t)(esp_timer_get_time() / 1000000ULL));
  c.print(F(",\"wdt\":true"));
  c.print(F(",\"ota_pending\":")); c.print(g_otaPendingVerify ? "true" : "false");
  c.print(F(",\"crash_streak\":")); c.print((int)g_crashStreak);
  c.print(F(",\"mqtt\":{\"enabled\":")); c.print(mqttConfig.enabled ? "true" : "false");
  c.print(F(",\"host\":")); jsonPrintEscaped(c, mqttConfig.host);
  c.print(F(",\"port\":")); c.print(mqttConfig.port);
  c.print(F(",\"user\":")); jsonPrintEscaped(c, mqttConfig.user);
  c.print(F(",\"prefix\":")); jsonPrintEscaped(c, mqttConfig.prefix);
  c.print(F("}"));
  c.print(F(",\"free_pwm\":["));
  { bool fp = true;
    for (size_t k = 0; k < COUNT_OF(PWM_ALLOWED); k++) {
      uint8_t p = PWM_ALLOWED[k];
      if (pinInUse(p)) continue;
      if (!fp) c.print(','); fp = false; c.print(p);
    } }
  c.print(F("],\"free_tach\":["));
  { bool ft = true;
    for (size_t k = 0; k < COUNT_OF(TACH_ALLOWED); k++) {
      uint8_t p = TACH_ALLOWED[k];
      if (pinInUse(p) || !canAttachInterruptPin(p)) continue;
      if (!ft) c.print(','); ft = false; c.print(p);
    } }
  c.print(F("]"));
```
Und im Fan-Objekt (nach `"validated"`): 
```cpp
    c.print(F(",\"cmin\":")); c.print((int)pctFromDuty(fans[i].calMinStart));
    c.print(F(",\"cnote\":")); jsonPrintEscaped(c, fans[i].calNote);
```
(Das MQTT-**Passwort** wird bewusst NIE ausgegeben.)

- [ ] **Step 5: Router neu** — in `handleClient()` GET-/POST-Blöcke ersetzen:

```cpp
  // --- GET ---
  if (method == "GET") {
    if (path == "/")            { sendUiAsset(c); return; }
    if (path == "/api/status")  { sendJsonStatus(c); return; }
    if (path == "/log.txt")     { handleLogTxt(c); return; }
    if (path == "/prevlog.txt") { handlePrevLogTxt(c); return; }
  }

  // --- POST ---
  if (method == "POST") {
    if (path == "/api/fan/set")        { handleFormPost(c, contentLength, contentType, apiFanSet); return; }
    if (path == "/api/fan/save")       { handleFormPost(c, contentLength, contentType, apiFanSave); return; }
    if (path == "/api/fan/delete")     { handleFormPost(c, contentLength, contentType, apiFanDelete); return; }
    if (path == "/api/fan/new")        { apiFanNew(c, ""); return; }
    if (path == "/api/calib")          { handleFormPost(c, contentLength, contentType, apiCalib); return; }
    if (path == "/api/mqtt")           { handleFormPost(c, contentLength, contentType, apiMqttSave); return; }
    if (path == "/api/safemode/reset") { apiSafeModeReset(c, ""); return; }
    if (path == "/api/reboot")         { apiReboot(c, ""); return; }
    if (path == "/ota") {
      if (contentType.startsWith(F("application/octet-stream"))) (void)handleOTA(c, contentLength);
      else httpSend400(c, "Use application/octet-stream");
      return;
    }
  }

  httpSend404(c);
```
Hinweis: `apiFanNew("")`-Aufrufe brauchen einen `String`-Temp: `apiFanNew(c, String(""));` — Signaturen nehmen `const String&`.

- [ ] **Step 6: Tote UI-Funktionen löschen** — komplette Funktionen entfernen:
`uiHeader`, `uiFooter`, `renderRoot`, `renderFanManager`, `renderFanEdit`, `renderCalib`,
`renderMQTTConfig`, `renderOTA`, `renderDiag`, `renderPinDropdown`, `httpSendRedirect`
sowie den alten `/fans/new`-, `/fans/set`-, `/fans/edit`-, `/fans/calib`-, `/reboot`-GET-Code
(Reboot läuft jetzt über `POST /api/reboot`).

- [ ] **Step 7: Build-Check** (EXIT=0; Größe notieren — UI-Asset rein, HTML-Strings raus → netto sollte das Image *kleiner* werden) **+ Commit** `git commit -am "JSON-API + Router-Umbau, alte HTML-Seiten entfernt, Status-Telemetrie"`

---

### Task 12: Endabnahme (ohne Flash!) + Doku

**Files:**
- Modify: `CLAUDE.md` (§2 Dateien-Tabelle: v4.0-Zeile ergänzen; §7 „Noch offen" aktualisieren)
- Create: `docs/flash-checklist.md`

- [ ] **Step 1: Alle Checks am Stück**

```bash
c++ -std=c++17 tests/host/test_logic.cpp -o /tmp/test_logic && /tmp/test_logic   # OK
tools/build_ui.sh                                                                 # <= 15000 Bytes
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$CLI" compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc --output-dir /tmp/out-v4 fan_controller_v4.0
ls -la /tmp/out-v4/*.bin    # Image < 1.250.000 Bytes (konservativer Slot)
grep -c "esp_task_wdt_deinit" fan_controller_v4.0/fan_controller_v4.0.ino        # MUSS 0 sein
grep -c "stormUntilMs\|g_nextMqttTryMs" fan_controller_v4.0/fan_controller_v4.0.ino  # MUSS 0 sein
```

- [ ] **Step 2: `docs/flash-checklist.md`** — Inhalt:

```markdown
# Flash-Checkliste v4.0 (NUR mit expliziter Freigabe von krobi!)

## Vorher
- [ ] ioBroker: neue Topics vorbereitet (`<prefix>/<deviceId>/<fan>/speed|set|rpm`)
- [ ] Build frisch: tools/build_ui.sh && arduino-cli compile (EXIT=0)
- [ ] Freigabe von krobi liegt vor

## Flash (Risiko-Fenster: Altfirmware hat KEIN Health-Window)
curl -X POST --data-binary @/tmp/out-v4/fan_controller_v4.0.ino.bin \
  -H 'Content-Type: application/octet-stream' http://10.47.88.239/ota

## Nachher (Reihenfolge!)
- [ ] Boot < 10 s, ping ok, http://10.47.88.239/ laedt neues UI
- [ ] /api/status: wdt=true, ota_pending=false (Altfirmware-Pfad markiert sofort valid)
- [ ] 4 Luefter drehen mit restaurierten Duties, RPM plausibel
- [ ] MQTT verbunden, ioBroker auf neue Topics umgestellt, set-Roundtrip ok
- [ ] Slider-Roundtrip im UI ok; Logs im System-Tab lesbar
- [ ] ROLLBACK-TEST (Spec §6.4): Test-Image mit absichtlichem Crash nach Boot
      flashen -> Geraet MUSS automatisch auf v4.0 zurueckfallen.
      Falls NICHT (alter Bootloader ohne Rollback-Support): dokumentieren —
      dann ist das Health-Window wirkungslos und Updates bleiben "Hoffnung+Test".
- [ ] 24 h beobachten: largest_block darf nicht monoton fallen; boot_count stabil
```

- [ ] **Step 3: CLAUDE.md ergänzen** — §2-Tabelle neue Zeile
`| fan_controller_v4.0/… | Stufe-1-Firmware (gehärtet + neues UI), Stand siehe Plan |`
und in §7 „Noch offen": Erst-Flash-Freigabe + ioBroker-Umstellung als offene Punkte.

- [ ] **Step 4: Commit** `git add -A && git commit -m "v4.0 Endabnahme: Checks gruen, Flash-Checkliste, Doku"`

---

## Self-Review (gegen Spec geprüft)

- §3.1 WDT → Task 4 · §3.2 OTA → Task 6 · §3.3 Wrap → Task 3 · §3.4 Boot-Reihenfolge → Task 5 · §3.5 NVS → Task 7 · §3.6 HTTP → Task 8 · §3.7 Safe-Mode → Task 5 (+Reset-API Task 11) · §3.8 Telemetrie → Task 11 Step 4 · §3.9 Uniqueness/offline → Tasks 11/6 · §4 MQTT → Task 9 · §5 UI/API → Tasks 10/11 · §6 Erst-Flash → Task 12 (Checkliste, kein Flash) · §7 Verifikation → Tasks 2/10/12.
- Typkonsistenz: `elapsed/fanNameValid/dutyFromPct/pctFromDuty` (Task 2) werden in Tasks 3-11 mit identischer Signatur verwendet; `prepareRestart` (Task 6) wird in Task 11 (`apiReboot`) verwendet; `printChunked`/`httpSend404` (Task 8) in Task 11; `mqttPublishSpeed`-Umbenennung (Task 9) betrifft nur dort genannte Aufrufer.
- Bekannte bewusste Lücke: `inv`-Checkbox im UI spiegelt nicht den Ist-Zustand (Status-JSON enthält kein `invertPwm`) — bewusst minimal; Invert wird nur gesetzt, wenn der Haken geändert wird. Falls störend: Folge-PR.

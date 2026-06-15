/**************************************************************
 * WAVESHARE ESP32-S3-ETH Fan Controller v5.0 (Dual-Core: native ETH.h + esp-mqtt + Core-Split)
 * Board: Waveshare ESP32-S3-ETH (W5500 via SPI2, PoE)
 * Arduino IDE 2.3.7 | ESP32 Arduino Core 3.3.8
 * W5500 Pins: MISO=12, MOSI=11, SCLK=13, CS=14, RST=9
 * Board-Setting: "USB CDC On Boot: Enabled" ist PFLICHT
 **************************************************************/

// --- Arduino autoproto fix ---
#include <stdint.h>
#include <atomic>   // §4.5: std::atomic fuer Cross-Core-Skalare (C11 _Atomic compiliert in C++/GCC nicht)
enum FanFault : uint8_t;
struct Fan;
struct ApplyJob;

// ==== Includes ====
#include <Arduino.h>
#include <SPI.h>
#include <Update.h>
#include "ESP32MQTTClient.h"   // thread-safe esp-mqtt (eigener Task); ersetzt PubSubClient
#include <Preferences.h>

#include "fw_util.h"
#include "concurrency.h"   // Stufe 3: Cross-Core-Queues (Telemetrie/Duty/...)
#include "net_eth.h"   // nativer W5500-Treiber (ETH.h) + Link-Events; ersetzt Ethernet.h

#include "esp_wifi.h"
#include "esp_bt.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "ui_asset.h"

extern "C" {
#include "driver/pcnt.h"
#include "esp_idf_version.h"
}

// [Spec §3.2] Boot-Auto-Validierung aufschieben: WIR markieren das Image erst
// nach gesundem Lauf-Fenster als gueltig. Crash/WDT vorher => Bootloader-Rollback.
// (Definition NACH allen Includes — Arduino-Autoproto, siehe CLAUDE.md §4.)
extern "C" bool verifyRollbackLater() { return true; }

static std::atomic<bool> g_otaPendingVerify{false};
static const uint32_t OTA_HEALTH_MS = 90000;

// W5500-Board-Pins liegen jetzt in net_eth.h (ETH_PIN_*/ETH_SPI_*).

// ==== Limits & Timing ====
static const uint8_t  MAX_FANS              = 8;
// §4.7: Arduino-loopTask laeuft auf Core 1 = Control-Core. ISR-/PCNT-/LEDC-Registrierung MUSS
// von hier erfolgen (Affinitaet folgt dem Registrar), damit tachEdgeCore + Zaehler auf demselben
// Core wie ihre Schreiber (fanSetDuty) liegen -> dann genuegt volatile (kein Mutex noetig).
#define CONTROL_CORE 1
static const uint32_t PWM_FREQ_HZ           = 25000;
static const uint8_t  PWM_BITS              = 8;
static const uint8_t  PULSES_PER_REV        = 2;
static const uint16_t RPM_HARD_CAP          = 3000;
static const uint16_t MAX_EXPECTED_RPM      = 2200;

static const uint32_t RPM_BASE_INTERVAL_MS  = 2500;
static const uint32_t RPM_BURST_INTERVAL_MS = 500;
static const uint8_t  RPM_BURST_SAMPLES     = 3;
static const float    EMA_ALPHA             = 0.25f;

// MQTT
static const uint32_t MQTT_PUB_MS_KEEPALIVE = 60000;
static const uint16_t MQTT_RPM_ABS_DELTA    = 50;

// HTTP & Buffers
static const unsigned long CLIENT_RD_TIMEOUT = 3000;   // Stall-Budget pro Zeile (< WDT 8s)
static const unsigned long BODY_RD_TIMEOUT   = 30000;  // Stall-Budget Body/OTA (t0 reset bei Fortschritt)
static const unsigned long HTTP_REQ_BUDGET_MS = 10000; // Gesamtbudget Nicht-OTA-Request (Spec §3.1)
static const size_t        LOG_MAX           = 8192;
static const uint32_t      LOG_FLUSH_MS      = 600000;  // 10 min — NVS-Wear (Spec §3.5)
static const size_t        LOG_NVS_MAX       = 1600;

// Storm-Shield & Filter
static const uint16_t ISR_STORM_BUDGET   = 9000;
static const uint16_t PCNT_STORM_BUDGET  = 16000;
static const uint32_t STORM_COOLDOWN_MS  = 5000;
static const uint32_t MIN_PULSE_US_FLOOR = 400;
static const uint16_t PCNT_FILTER_CYCLES = 800;

// Boot/State
static const uint8_t  CRASH_LOOP_LIMIT       = 3;
static const uint32_t STATE_WRITE_DEBOUNCE_MS = 2500;

// Ethernet (nativer ETH.h-Treiber; DHCP/Lease-Erneuerung macht esp-netif selbst)
static const uint32_t      ETH_REINIT_COOLDOWN_MS  = 5000;
static const uint32_t      ETH_LINK_LOST_RESET_MS  = 15000;

// ==== Pin-Listen (Waveshare ESP32-S3-ETH) ====
// Geblockt: W5500 (9-14), USB CDC (19-20), Strapping (0,3,43-46)
static const uint8_t PWM_ALLOWED[] = {
  1, 2, 4, 5, 6, 7, 8, 15, 16, 17, 18, 21,
  33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 47, 48
};
static const uint8_t TACH_ALLOWED[] = {
  1, 2, 4, 5, 6, 7, 8, 15, 16, 17, 18, 21,
  33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 47, 48
};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

// ==== Pin-Validierung ====
static inline bool isPinBlocked(uint8_t pin) {
  if (pin >= 9 && pin <= 14) return true;   // W5500
  if (pin == 19 || pin == 20) return true;  // USB CDC
  if (pin == 0 || pin == 3) return true;    // Boot/JTAG
  if (pin >= 43 && pin <= 46) return true;  // Strapping
  if (pin == 0xFF) return true;
  return false;
}
static inline bool inList(uint8_t pin, const uint8_t *lst, size_t n) {
  for (size_t i = 0; i < n; i++) if (lst[i] == pin) return true;
  return false;
}
static inline bool isPwmAllowed(uint8_t pin)  { return pin != 0xFF && !isPinBlocked(pin) && inList(pin, PWM_ALLOWED, COUNT_OF(PWM_ALLOWED)); }
static inline bool isTachAllowed(uint8_t pin) { return pin != 0xFF && !isPinBlocked(pin) && inList(pin, TACH_ALLOWED, COUNT_OF(TACH_ALLOWED)); }
static inline bool canAttachInterruptPin(uint8_t pin) {
  if (pin == 0xFF || isPinBlocked(pin)) return false;
  int irq = digitalPinToInterrupt(pin);
  return (irq != -1 && irq != NOT_AN_INTERRUPT);
}

// ==== MQTT Config ====
struct MQTTConfig {
  bool     enabled = false;
  char     host[64] = "";
  uint16_t port = 1883;
  char     user[32] = "";
  char     pass[32] = "";
  char     prefix[16] = "esp";
} mqttConfig;

// ==== System State ====
static String   gLogBuf, gPrevLogTail;
static uint32_t g_bootCount = 0;
static bool     g_crashLoopDetected = false;
static String   g_resetReasonStr = "OTHER";
static std::atomic<uint32_t> g_minFreeHeap{UINT32_MAX};
static String   deviceId;
static uint8_t  g_mac[6];
static uint8_t  g_crashStreak = 0;

// ==== Logger ====
static inline const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "OTHER";
  }
}

// §4.6: gLogBuf (Heap-String) wird von beiden Cores beruehrt -> Mutex. Producer (LOG-Makros)
// posten in die Log-Queue (thread-safe, nie blockierend); EIN Consumer (logDrain) haengt an.
static SemaphoreHandle_t logMutex = nullptr;
static inline void logLock()   { if (logMutex) xSemaphoreTake(logMutex, portMAX_DELAY); }
static inline void logUnlock() { if (logMutex) xSemaphoreGive(logMutex); }

// Consumer-Seite: haengt eine Zeile an gLogBuf (mit Ringkuerzung). NUR via logDrain (ein Task).
static void logLine(const char *s) {
  size_t slen = strlen(s);
  logLock();
  if (gLogBuf.length() + slen + 1 > LOG_MAX) {
    int cut = (gLogBuf.length() + slen + 1) - LOG_MAX + 256;
    if (cut < (int)gLogBuf.length()) gLogBuf.remove(0, cut);
    else gLogBuf = "";
  }
  gLogBuf += s; gLogBuf += '\n';
  logUnlock();
}

// Producer-Seite (beide Cores): sofort auf Serial (Debug) + in die Log-Queue (gLogBuf via Consumer).
// Lokaler Puffer statt geteiltem Static -> thread-safe ohne Lock im Hot-Path.
static void logFmt(char level, const char *tag, const char *msg) {
  char line[128];
  uint32_t ms = millis();
  snprintf(line, sizeof(line),
           "[T+%04lu.%03lus #%lu] [%c] %s: %s",
           (unsigned long)(ms / 1000UL), (unsigned long)(ms % 1000UL),
           (unsigned long)g_bootCount, level, tag, msg);
  Serial.println(line);
  logPost(line);
}

// Leert die Log-Queue in gLogBuf. NUR aus EINEM Task (Loop bis Task 8, dann networkTask).
static void logDrain() {
  if (!g_logQ) return;
  LogLine l;
  while (xQueueReceive(g_logQ, &l, 0) == pdTRUE) logLine(l.text);
}

#define LOGI(t, m) logFmt('I', t, (String(m)).c_str())
#define LOGW(t, m) logFmt('W', t, (String(m)).c_str())
#define LOGE(t, m) logFmt('E', t, (String(m)).c_str())

static void persistLogTail() {
  logLock();                          // §4.6: gLogBuf-Snapshot unter Lock (Consumer haengt evtl. parallel an)
  if (gLogBuf.isEmpty()) { logUnlock(); return; }
  String tail = gLogBuf;
  logUnlock();
  if (tail.length() > (int)LOG_NVS_MAX) tail.remove(0, tail.length() - LOG_NVS_MAX);
  Preferences p; p.begin("sys", false);
  p.putString("log_tail", tail);
  p.end();
}
static void loadPrevLogTail() {
  Preferences p; p.begin("sys", true);
  gPrevLogTail = p.getString("log_tail", "");
  p.end();
}
static void bootTrackInit() {
  Preferences p; p.begin("sys", false);
  uint32_t prevBoots = p.getUInt("boots", 0);
  uint8_t crashStreak = p.getUChar("crash_streak", 0);

  esp_reset_reason_t rr = esp_reset_reason();
  g_resetReasonStr = resetReasonStr(rr);

  bool isCrash = (rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT || rr == ESP_RST_PANIC);
  if (isCrash) crashStreak = (uint8_t)min<int>(crashStreak + 1, 255);
  else crashStreak = 0;

  p.putUChar("crash_streak", crashStreak);
  g_bootCount = prevBoots + 1;
  p.putUInt("boots", g_bootCount);

  g_crashStreak = crashStreak;
  g_crashLoopDetected = (crashStreak >= CRASH_LOOP_LIMIT);
  p.putUChar("safe_mode", g_crashLoopDetected ? 1 : 0);
  p.putString("last_reset", g_resetReasonStr);
  p.end();

  LOGI("BOOT", String("Reset: ") + g_resetReasonStr + " | boot=" + g_bootCount +
       " | crash_streak=" + (int)crashStreak + (g_crashLoopDetected ? " | CRASH-LOOP" : ""));
}

// ==== URL/Form Helpers ====
static char fromHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}
static String urlDecode(const String &s) {
  String o; o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') o += ' ';
    else if (c == '%' && i + 2 < s.length()) { char h = (fromHex(s[i+1]) << 4) | fromHex(s[i+2]); o += h; i += 2; }
    else o += c;
  }
  return o;
}
static bool formGet(const String &body, const String &key, String &out) {
  String k = key + "="; int p = body.indexOf(k); if (p < 0) return false;
  int s = p + k.length(); int e = body.indexOf('&', s);
  out = urlDecode((e < 0) ? body.substring(s) : body.substring(s, e)); return true;
}
static inline void safeStrcpy(char *dst, size_t cap, const String &src) {
  size_t n = min(cap - 1, (size_t)src.length()); memcpy(dst, src.c_str(), n); dst[n] = 0;
}
static String sanitizeName(const String &in) {
  String s = in; s.trim(); String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i]; if (c == ' ') c = '_';
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') out += c;
  }
  if (out.isEmpty()) out = "fan";
  return out;
}

// ==== Fan Faults & Datenmodell ====
enum FanFault : uint8_t {
  FF_OK = 0,
  FF_PIN_FORBIDDEN,
  FF_NO_IRQ,
  FF_NO_TACH_PULSES,
  FF_PULSE_STORM
};

// [B6] lastValidateMs entfernt (Dead Field)
struct Fan {
  char    name[20];
  uint8_t pwmPin;
  uint8_t tachPin;
  bool    invertPwm;

  volatile uint32_t pulseCount;
  uint32_t lastPulseCount;
  uint16_t rpmShown;
  uint16_t rpmRawA, rpmRawB, rpmRawC;
  float    rpmEma;
  uint8_t  duty;
  uint8_t  calMinStart;
  char     calNote[40];

  volatile uint32_t lastEdgeUs;
  uint32_t minPulseUs;

  uint32_t lastRpmPubMs;

  bool        pcntEnabled;
  pcnt_unit_t pcntUnit;

  bool     validated;
  FanFault fault;
  uint8_t  faultCount;
  uint32_t stormSinceMs;
  bool     stormActive;

  uint8_t  burstLeft;
  bool     measureSuspended;
};

static Fan fans[MAX_FANS] = { 0 };

// ==== DRY: Fan-Defaults & NVS-Helpers ====
// [B6] lastValidateMs entfernt
static void fanInitDefaults(uint8_t idx) {
  Fan &f = fans[idx];
  f.pwmPin = 0xFF; f.tachPin = 0xFF; f.invertPwm = false;
  f.duty = 0; f.calMinStart = 0; f.calNote[0] = 0;
  f.pulseCount = 0; f.lastPulseCount = 0;
  f.rpmShown = 0; f.rpmRawA = 0; f.rpmRawB = 0; f.rpmRawC = 0; f.rpmEma = 0;
  f.lastRpmPubMs = 0;
  f.pcntEnabled = false; f.pcntUnit = PCNT_UNIT_0;
  f.validated = false; f.fault = FF_OK; f.faultCount = 0;
  f.stormSinceMs = 0; f.stormActive = false; f.burstLeft = 0; f.measureSuspended = false;
  f.lastEdgeUs = 0;
  uint32_t minPulse = (uint32_t)((60UL * 1000000UL) / max<uint32_t>(1, (uint32_t)MAX_EXPECTED_RPM * PULSES_PER_REV));
  f.minPulseUs = max<uint32_t>(MIN_PULSE_US_FLOOR, minPulse / 3);
}

// [B5] saveFanToNVS entfernt (Dead Code)

static void clearFanNVS(uint8_t idx) {
  Preferences p; p.begin("fans", false);
  String k = "f" + String(idx) + "_";
  p.putUChar((k + "pwm").c_str(),  0xFF);
  p.putUChar((k + "tac").c_str(),  0xFF);
  p.putUChar((k + "inv").c_str(),  0);
  p.putUChar((k + "cmin").c_str(), 0);
  p.putString((k + "cnote").c_str(), "");
  p.putString((k + "name").c_str(), "");
  p.end();
}

// ==== Präsenz & Pin-Nutzung ====
static inline bool fanPresentIdx(uint8_t i) {
  const Fan &f = fans[i];
  return f.name[0] != 0 && f.pwmPin != 0xFF && f.tachPin != 0xFF;
}
static bool pinInUse(uint8_t pin, int8_t ignoreIndex = -1) {
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if ((int8_t)i == ignoreIndex) continue;
    if (!fanPresentIdx(i)) continue;
    if (fans[i].pwmPin == pin || fans[i].tachPin == pin) return true;
  }
  return false;
}
static inline bool validPwmForFan(uint8_t p, int8_t ignoreIndex) {
  return isPwmAllowed(p) && !pinInUse(p, ignoreIndex);
}
static inline bool validTachForFan(uint8_t p, int8_t ignoreIndex) {
  return isTachAllowed(p) && !pinInUse(p, ignoreIndex) && canAttachInterruptPin(p);
}
static void fanMarkFault(uint8_t idx, FanFault ff) {
  Fan &f = fans[idx];
  if (f.fault != ff) f.faultCount = 0;
  f.fault = ff; f.faultCount++;
}
static void fanAutoValidate(uint8_t idx) {
  Fan &f = fans[idx];
  if (!fanPresentIdx(idx))                                    { f.validated = false; fanMarkFault(idx, FF_OK); return; }
  if (!isPwmAllowed(f.pwmPin) || !isTachAllowed(f.tachPin))  { f.validated = false; fanMarkFault(idx, FF_PIN_FORBIDDEN); return; }
  if (!canAttachInterruptPin(f.tachPin))                      { f.validated = false; fanMarkFault(idx, FF_NO_IRQ); return; }
  f.validated = true; fanMarkFault(idx, FF_OK);
}

// ==== Globaler State ====
static std::atomic<uint32_t> g_stateRev{0};

// §4.3 (Spec): EIN Mutex schuetzt fans[]-Felder, die der esp-mqtt-Task liest, waehrend der
// Loop-Task sie schreibt — plus die Pending-Cleanup-Liste. Disziplin: NIE ueber Socket-I/O
// (mqtt.publish) halten -> unter Lock snapshotten, Lock freigeben, DANN senden. Fundament fuer
// den Dual-Core-Split (Stufe 3); behebt heute schon den §19-Datarace Loop<->esp-mqtt-Task.
static SemaphoreHandle_t fansMutex = nullptr;
static PendingCleanup    g_pendingCleanup = {};
static inline void fansLock()   { if (fansMutex) xSemaphoreTake(fansMutex, portMAX_DELAY); }
static inline void fansUnlock() { if (fansMutex) xSemaphoreGive(fansMutex); }
// §18: einen sanitized Namen zum Nachraeumen vormerken (Loop-Task; Liste ist Cross-Task -> Lock).
static void pendingCleanupQueue(const char *sanitizedName) {
  fansLock();
  bool ok = pendingCleanupAdd(g_pendingCleanup, sanitizedName);
  fansUnlock();
  if (!ok) LOGW("MQTT", "cleanup-Liste voll - Topic verwaist");
}

// ==== Netz/MQTT-Objekte (nativer ETH.h-Stack) ====
NetworkServer    httpServer(80);
ESP32MQTTClient  mqtt;   // esp-mqtt im eigenen FreeRTOS-Task, thread-safe, Auto-Reconnect
static String    g_mqttClientId;   // persistent (ESP32MQTTClient speichert nur den Pointer)
static String    g_mqttLwtTopic;

// ==== MQTT Helpers ====
static String topicDev() { return String(mqttConfig.prefix) + "/" + deviceId; }
// Flaches Schema (Spec §4): <prefix>/<deviceId>/<name>/{speed,set,rpm}
// §19: Namen unter Lock snapshotten — der Loop-Task kann fans[i].name gerade memset/strcpy'en.
static String topicFan(uint8_t i) {
  char name[20]; uint8_t n = 0;
  fansLock();
  for (; n < sizeof(name) - 1 && fans[i].name[n]; n++) name[n] = fans[i].name[n];
  name[n] = 0;
  fansUnlock();
  return topicDev() + "/" + sanitizeName(String(name));
}

// pctFromDuty/dutyFromPct kommen aus fw_util.h (host-getestet)

// Vorab-Prototypen
static void mqttPublishSpeed(uint8_t i);
static void mqttPublishRPM(uint8_t i, bool force = false);

// ==== Safety: alle PWM-Pins LOW ====
static void safetyZeroPins() {
  for (size_t k = 0; k < COUNT_OF(PWM_ALLOWED); k++) {
    uint8_t p = PWM_ALLOWED[k];
    if (!isPinBlocked(p)) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  }
  LOGI("SAFETY", "All PWM pins LOW");
}

// ==== Device-ID & MAC ====
static void buildMacAndDeviceId() {
  uint64_t e = ESP.getEfuseMac();
  g_mac[0] = ((e >> 40) & 0xFE) | 0x02;
  g_mac[1] = (e >> 32) & 0xFF;
  g_mac[2] = (e >> 24) & 0xFF;
  g_mac[3] = (e >> 16) & 0xFF;
  g_mac[4] = (e >> 8)  & 0xFF;
  g_mac[5] = e & 0xFF;
  char buf[32]; snprintf(buf, sizeof(buf), "ws-s3eth-%02X%02X%02X", g_mac[3], g_mac[4], g_mac[5]);
  deviceId = buf;
  LOGI("ID", (String("DeviceID=") + deviceId).c_str());
}

// ==== Radios aus ====
static void disableRadios() {
  (void)esp_wifi_stop();
#if CONFIG_BT_ENABLED
  esp_bt_controller_status_t st = esp_bt_controller_get_status();
  if (st == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    (void)esp_bt_controller_disable();
    st = esp_bt_controller_get_status();
  }
  if (st == ESP_BT_CONTROLLER_STATUS_INITED) {
    (void)esp_bt_controller_deinit();
  }
#endif
  LOGI("RADIO", "WiFi/BT disabled");
}

// ==== W5500 Reset ====
// W5500-Reset/Init liegt jetzt in net_eth.h: ethBegin() / ethHardReset().

// ==== Duty-State Persistenz ====
static uint8_t  g_lastSavedDuty[MAX_FANS] = {0};
static bool     g_stateDirty[MAX_FANS]    = {false};
static uint32_t g_lastStateWriteMs        = 0;

static void stateScheduleDutySave(uint8_t idx, uint8_t duty) {
  if (idx >= MAX_FANS) return;
  if (g_lastSavedDuty[idx] != duty) g_stateDirty[idx] = true;
}
static void stateFlushIfNeeded(uint32_t now) {
  if (g_crashLoopDetected) return;  // Safe-Mode-Failsafe-Duties nie in NVS schreiben
  if (!elapsed(now, g_lastStateWriteMs, STATE_WRITE_DEBOUNCE_MS)) return;
  Preferences p; p.begin("state", false);
  bool wrote = false;
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i) || !g_stateDirty[i]) continue;
    String k = "f" + String(i) + "_duty";
    p.putUChar(k.c_str(), fans[i].duty);
    g_lastSavedDuty[i] = fans[i].duty;
    g_stateDirty[i] = false;
    wrote = true;
  }
  p.end();
  if (wrote) { g_lastStateWriteMs = now; LOGI("STATE", "duties persisted"); }
}

// ==== Config laden ====
static void loadConfigFans() {
  Preferences p; p.begin("fans", true);
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    fanInitDefaults(i);
    String k = "f" + String(i) + "_";
    fans[i].pwmPin     = p.getUChar((k + "pwm").c_str(),  0xFF);
    fans[i].tachPin    = p.getUChar((k + "tac").c_str(),  0xFF);
    fans[i].invertPwm  = p.getUChar((k + "inv").c_str(),  0) == 1;
    fans[i].calMinStart = p.getUChar((k + "cmin").c_str(), 0);
    String n  = p.getString((k + "cnote").c_str(), "");
    String nm = p.getString((k + "name").c_str(), "");
    safeStrcpy(fans[i].calNote, sizeof(fans[i].calNote), n);
    safeStrcpy(fans[i].name,    sizeof(fans[i].name),    nm);

    if (!isPwmAllowed(fans[i].pwmPin))   fans[i].pwmPin  = 0xFF;
    if (!isTachAllowed(fans[i].tachPin)) fans[i].tachPin = 0xFF;

    // Boot-Safety: Hardware immer 0, Duty wird spaeter aus NVS restored
    fans[i].duty = 0;
  }
  p.end();
  LOGI("PREFS", "Fan config loaded");
}
static void loadMQTTConfig() {
  Preferences p; p.begin("mqtt", true);
  if (p.isKey("enabled")) {
    mqttConfig.enabled = p.getBool("enabled", false);
    p.getString("host",   mqttConfig.host,   sizeof(mqttConfig.host));
    mqttConfig.port = p.getUShort("port", 1883);
    p.getString("user",   mqttConfig.user,   sizeof(mqttConfig.user));
    p.getString("pass",   mqttConfig.pass,   sizeof(mqttConfig.pass));
    p.getString("prefix", mqttConfig.prefix, sizeof(mqttConfig.prefix));
  }
  p.end();
  LOGI("PREFS", String("MQTT: ") + (mqttConfig.enabled ? "on" : "off") + " host=" + mqttConfig.host + " port=" + mqttConfig.port);
}

// ==== JSON Helpers ====
static void jsonPrintEscaped(NetworkClient &c, const char *s) {
  c.print('"');
  for (const char *p = s; *p; ++p) {
    char ch = *p;
    if (ch == '"' || ch == '\\') { c.print('\\'); c.print(ch); }
    else if ((uint8_t)ch < 0x20) { char b[7]; snprintf(b, sizeof(b), "\\u%04x", ch); c.print(b); }
    else c.print(ch);
  }
  c.print('"');
}

// ==== PWM setzen ====
static inline uint32_t computeMinPulseUs(uint8_t duty) {
  uint32_t rpmExp = (uint32_t)MAX_EXPECTED_RPM * duty / 255; rpmExp = max<uint32_t>(rpmExp, 1);
  uint32_t periodUs = (60UL * 1000000UL) / (rpmExp * PULSES_PER_REV);
  return max<uint32_t>(MIN_PULSE_US_FLOOR, periodUs / 3);
}
static void fanSetDuty(Fan &f, uint8_t duty) {
  if (duty > 0 && f.calMinStart > 0 && duty < f.calMinStart) duty = f.calMinStart;
  f.duty = duty;
  f.minPulseUs = computeMinPulseUs(duty);
  const uint32_t out = f.invertPwm ? (255 - duty) : duty;
  if (!isPinBlocked(f.pwmPin)) ledcWrite(f.pwmPin, out);
  f.burstLeft = RPM_BURST_SAMPLES;
}
// [B3] g_stateRev++ entfernt — nur Strukturaenderungen inkrementieren rev
static void onDutyChanged(uint8_t idx) {
  if (idx >= MAX_FANS || !fanPresentIdx(idx)) return;
  stateScheduleDutySave(idx, fans[idx].duty);
  TelemetrySample s = { idx, TELEM_SPEED, fans[idx].duty, 0, (uint8_t)fans[idx].fault };  // §4.1: Control postet
  telemPost(s);
}

// ==== ISR (Glitchfilter) ====
static inline void IRAM_ATTR tachEdgeCore(uint8_t idx) {
  Fan &f = fans[idx];
  if (f.measureSuspended) return;
  const uint32_t nowUs = micros();
  const uint32_t dUs   = nowUs - f.lastEdgeUs;
  if (dUs >= f.minPulseUs) {
    f.lastEdgeUs = nowUs;
    f.pulseCount++;
  }
}
static void IRAM_ATTR tachIsr0() { tachEdgeCore(0); }
static void IRAM_ATTR tachIsr1() { tachEdgeCore(1); }
static void IRAM_ATTR tachIsr2() { tachEdgeCore(2); }
static void IRAM_ATTR tachIsr3() { tachEdgeCore(3); }
static void IRAM_ATTR tachIsr4() { tachEdgeCore(4); }
static void IRAM_ATTR tachIsr5() { tachEdgeCore(5); }
static void IRAM_ATTR tachIsr6() { tachEdgeCore(6); }
static void IRAM_ATTR tachIsr7() { tachEdgeCore(7); }

typedef void (*ISRFunction)();
static ISRFunction tachISRs[MAX_FANS] = {
  tachIsr0, tachIsr1, tachIsr2, tachIsr3,
  tachIsr4, tachIsr5, tachIsr6, tachIsr7
};

// ==== PCNT (Hardware Pulse Counter) ====
static void detachFanCounters(uint8_t idx) {
  if (fans[idx].pcntEnabled) {
    pcnt_counter_pause(fans[idx].pcntUnit);
    pcnt_counter_clear(fans[idx].pcntUnit);
    fans[idx].pcntEnabled = false;
  }
  if (canAttachInterruptPin(fans[idx].tachPin)) {
    detachInterrupt(digitalPinToInterrupt(fans[idx].tachPin));
  }
}

static bool enablePcntForFan(uint8_t idx, pcnt_unit_t unit) {
  Fan &f = fans[idx];
  if (!isTachAllowed(f.tachPin)) return false;

  pcnt_config_t cfg = {};
  cfg.pulse_gpio_num = (int)f.tachPin;
  cfg.ctrl_gpio_num  = PCNT_PIN_NOT_USED;
  cfg.channel        = PCNT_CHANNEL_0;
  cfg.unit           = unit;
  cfg.pos_mode       = PCNT_COUNT_DIS;
  cfg.neg_mode       = PCNT_COUNT_INC;
  cfg.lctrl_mode     = PCNT_MODE_KEEP;
  cfg.hctrl_mode     = PCNT_MODE_KEEP;
  cfg.counter_h_lim  = 32767;
  cfg.counter_l_lim  = -32768;

  if (pcnt_unit_config(&cfg) != ESP_OK) {
    LOGW("PCNT", String("unit_config fail fan=") + idx);
    return false;
  }
  pcnt_set_filter_value(cfg.unit, PCNT_FILTER_CYCLES);
  pcnt_filter_enable(cfg.unit);
  pcnt_counter_pause(cfg.unit);
  pcnt_counter_clear(cfg.unit);
  pcnt_counter_resume(cfg.unit);

  f.pcntUnit    = cfg.unit;
  f.pcntEnabled = true;
  LOGI("PCNT", String("fan=") + idx + " unit=" + (int)cfg.unit + " gpio=" + (int)f.tachPin);
  return true;
}

static void enableIsrForFan(uint8_t idx) {
  if (xPortGetCoreID() != CONTROL_CORE) LOGE("AFFIN", String("ISR-Setup auf Core ") + xPortGetCoreID() + " (erwartet " + CONTROL_CORE + ")");  // §4.7
  Fan &f = fans[idx];
  if (canAttachInterruptPin(f.tachPin)) {
    attachInterrupt(digitalPinToInterrupt(f.tachPin), tachISRs[idx], FALLING);
    LOGI("ISR", String("attach fan=") + idx + " gpio=" + (int)f.tachPin);
  } else {
    LOGW("ISR", String("no irq fan=") + idx + " gpio=" + (int)f.tachPin);
  }
}

static uint32_t pcntReadDeltaAndClear(uint8_t idx) {
  Fan &f = fans[idx];
  if (!f.pcntEnabled) return 0;
  int16_t val = 0;
  pcnt_get_counter_value(f.pcntUnit, &val);
  pcnt_counter_clear(f.pcntUnit);
  if (val < 0) val = 0;
  return (uint32_t)val;
}

// ==== Deterministische PCNT-Zuweisung ====
static void rebuildPcntMap() {
  if (xPortGetCoreID() != CONTROL_CORE) LOGE("AFFIN", String("PCNT-Setup auf Core ") + xPortGetCoreID() + " (erwartet " + CONTROL_CORE + ")");  // §4.7
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i)) continue;
    fans[i].measureSuspended = true;
    detachFanCounters(i);
  }

  uint8_t assigned = 0;
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i)) continue;
    fanAutoValidate(i);
    if (!fans[i].validated) { fanMarkFault(i, FF_NO_IRQ); continue; }
    if (assigned < 4) {
      if (enablePcntForFan(i, (pcnt_unit_t)assigned)) { assigned++; continue; }
    }
    enableIsrForFan(i);
  }

  String map = "PCNT map: ";
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i)) continue;
    map += "#" + String(i) + (fans[i].pcntEnabled ? ("=U" + String((int)fans[i].pcntUnit)) : "=ISR") + " ";
  }
  LOGI("PCNT", map);

  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i)) continue;
    fans[i].measureSuspended = false;
  }
}

// ==== Storm-Shield ====
static inline void stormTrip(uint8_t i) {
  Fan &f = fans[i];
  fanMarkFault(i, FF_PULSE_STORM);
  f.measureSuspended = true;
  detachFanCounters(i);
  f.stormActive = true;
  f.stormSinceMs = millis();
  f.rpmRawA = f.rpmRawB = f.rpmRawC = 0;
  f.rpmEma  = 0;
  f.rpmShown = 0;
  LOGW("STORM", String("fan=") + i + " cooldown");
}
static inline void stormTryRecover(uint8_t i) {
  Fan &f = fans[i];
  if (!f.stormActive) return;
  if (!elapsed(millis(), f.stormSinceMs, STORM_COOLDOWN_MS)) return;

  f.stormActive = false;
  f.measureSuspended = true;
  detachFanCounters(i);
  rebuildPcntMap();
  f.measureSuspended = false;
  fanMarkFault(i, FF_OK);
  LOGI("STORM", String("fan=") + i + " recovered");
}

// ==== Deferred Apply ====
struct ApplyJob {
  bool     active     = false;
  uint8_t  idx        = 0;
  uint8_t  pwmPin     = 0xFF;
  uint8_t  tachPin    = 0xFF;
  bool     invert     = false;
  char     name[20]   = {0};
  bool     deleteFan  = false;
  bool     pinsChanged = false;
  bool     nameChanged = false;
  bool     invChanged  = false;
  bool     calChanged  = false;        // §4.4: Calib ueber die Queue statt fans[]-Bypass in apiCalib
  uint8_t  calMin      = 0;
  char     calNote[40] = {0};
};

// §4.2: Apply-Job-Queue (by value) Core0->Core1, ersetzt g_apply[]/g_anyApplyPending.
static void applyQueue(uint8_t idx, const ApplyJob &src) {
  if (idx >= MAX_FANS || !g_applyQ) return;
  ApplyJob j = src; j.idx = idx; j.active = true;
  xQueueSend(g_applyQ, &j, 0);   // Drop bei Voll (UI macht eine Aktion/Mal; Loop drained jeden Durchlauf)
  LOGI("APPLY", String("queued idx=") + idx + " del=" + (src.deleteFan ? "1" : "0") +
       " pins=" + (src.pinsChanged ? "1" : "0") + " name=" + (src.nameChanged ? "1" : "0"));
}

// ==== Duty-Queue (§4.2: FreeRTOS-Queue Core0->Core1, ersetzt g_pendingDuty[]) ====
// Producer (Netz: HTTP/MQTT) postet nur; der Consumer (Control-Core) validiert gegen fans[].
static inline void dutyEnqueue(uint8_t idx, uint8_t duty) {
  if (idx >= MAX_FANS) return;
  DutyCmd d = { idx, duty };
  dutyPost(d);
}
static void dutyProcessQueue() {
  if (!g_dutyQ) return;
  DutyCmd d;
  while (xQueueReceive(g_dutyQ, &d, 0) == pdTRUE) {
    if (d.idx < MAX_FANS && fanPresentIdx(d.idx)) {
      fanSetDuty(fans[d.idx], d.duty);
      onDutyChanged(d.idx);
    }
  }
}

// ==== MQTT Publish/Subscribe ====
static uint16_t g_lastRpmSent[MAX_FANS] = {0};

static void mqttPublishSpeed(uint8_t i) {
  if (!mqtt.isConnected() || !fanPresentIdx(i)) return;
  char b[4]; snprintf(b, sizeof(b), "%u", (unsigned)pctFromDuty(fans[i].duty));
  mqtt.publish((topicFan(i) + "/speed").c_str(), b, 0, true);
}

// §4.1: RPM-Telemetrie posten statt direkt publishen. Throttle bleibt PRODUCER-seitig
// (Keepalive/Delta) -> haelt die Queue leicht; der Netz-Task (telemDrain) published nur noch.
// Throttle-State (lastRpmPubMs/g_lastRpmSent) gehoert dem Control-Core -> kein Cross-Core-Zugriff.
static void telemPostRpm(uint8_t i) {
  if (!fanPresentIdx(i)) return;
  uint32_t now = millis();
  uint16_t rpm = fans[i].rpmShown;
  bool due = (fans[i].lastRpmPubMs == 0)
          || elapsed(now, fans[i].lastRpmPubMs, MQTT_PUB_MS_KEEPALIVE)
          || abs((int)rpm - (int)g_lastRpmSent[i]) >= MQTT_RPM_ABS_DELTA;
  if (!due) return;
  TelemetrySample s = { i, TELEM_RPM, fans[i].duty, rpm, (uint8_t)fans[i].fault };
  telemPost(s);
  fans[i].lastRpmPubMs = now;
  g_lastRpmSent[i] = rpm;
}

// Telemetrie-Queue leeren und via esp-mqtt publishen. NUR aus dem Netz-Kontext aufrufen
// (Task 8: networkTask). Liest fans[] nicht ausser topicFan (Name unter fansMutex).
static void telemDrain() {
  if (!g_telemQ) return;
  TelemetrySample s;
  while (xQueueReceive(g_telemQ, &s, 0) == pdTRUE) {
    if (!mqtt.isConnected()) continue;          // Queue trotzdem leeren (kein Stau)
    String base = topicFan(s.idx);
    if (s.kind == TELEM_SPEED) {
      char b[4]; snprintf(b, sizeof(b), "%u", (unsigned)pctFromDuty(s.duty));
      mqtt.publish((base + "/speed").c_str(), b, 0, true);
    } else {
      char b[12]; snprintf(b, sizeof(b), "%u", (unsigned)s.rpm);
      mqtt.publish((base + "/rpm").c_str(), b, 0, false);
    }
  }
}

// Befehl von <prefix>/<deviceId>/<fan>/set anwenden. idx ist im Subscribe-Lambda gebunden,
// daher kein Topic-Parsen nötig. [Review-Fund E] Payload ohne Ziffer ignorieren (kein 0%-Unfall).
static void mqttApplySet(uint8_t idx, const std::string &payload) {
  bool hasDigit = false;
  for (char c : payload) if (c >= '0' && c <= '9') { hasDigit = true; break; }
  if (!hasDigit) { LOGW("MQTT", "set ohne Zahl ignoriert"); return; }
  int pct = constrain(atoi(payload.c_str()), 0, 100);
  if (idx < MAX_FANS && fanPresentIdx(idx)) {
    dutyEnqueue(idx, dutyFromPct((uint8_t)pct));   // Cross-Task -> Duty-Queue
    char nm[20]; fansLock(); { uint8_t n=0; for(; n<19 && fans[idx].name[n]; n++) nm[n]=fans[idx].name[n]; nm[n]=0; } fansUnlock();  // §19: Name fuer Log unter Lock
    LOGI("MQTT", String("set ") + nm + " -> " + pct + "%");
  }
}

static bool                  ethHasIP = false;   // nur im networkTask (Core 0) benutzt
static std::atomic<bool>     httpUp{false};       // networkTask setzt; Health-Window (loopTask) liest -> Commit-Gate
static std::atomic<uint32_t> g_netCtr{0};         // networkTask-Heartbeat (Liveness: Commit-Gate + /api/status)
static std::atomic<uint32_t> g_loopCtr{0};        // Control-Loop-Heartbeat (Liveness)
static TaskHandle_t          g_netTaskHandle = nullptr;
static inline void feedWdt() { esp_task_wdt_reset(); }   // §5.3: fuettert den AKTUELLEN Task (beide abonniert)

// Jeden vorhandenen Lüfter auf .../set abonnieren (idx-gebundenes Lambda).
static void mqttSubscribeFan(uint8_t i) {
  mqtt.subscribe((topicFan(i) + "/set").c_str(), [i](const std::string &p) { mqttApplySet(i, p); });
}

// Von esp-mqtt bei (Re-)Connect aufgerufen (MQTT-Task-Kontext): online melden, abonnieren, Ist publishen.
void onMqttConnect(esp_mqtt_client_handle_t client) {
  if (!mqtt.isMyTurn(client)) return;
  mqtt.publish((topicDev() + "/status").c_str(), "online", 0, true);
  // §18: ZUERST verwaiste retained Topics aus der Disconnect-Phase raeumen (snapshot-then-send) —
  // vor dem present-Loop, damit ein im selben Slot neu angelegter, gleichnamiger Luefter zuletzt
  // gewinnt (Subscribe/Publish nicht versehentlich vom Cleanup ueberschrieben).
  PendingCleanup snap;
  fansLock(); snap = g_pendingCleanup; pendingCleanupClear(g_pendingCleanup); fansUnlock();
  for (uint8_t k = 0; k < snap.count; k++) {
    String base = topicDev() + "/" + snap.names[k];
    mqtt.publish((base + "/speed").c_str(), "", 0, true);
    mqtt.unsubscribe((base + "/set").c_str());
  }
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i)) continue;
    mqttSubscribeFan(i);
    mqttPublishSpeed(i);
  }
  LOGI("MQTT", "connected");
}

// Event-Trampolin für esp-mqtt (IDF 5.x), von loopStart() registriert.
void handleMQTT(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  (void)handler_args; (void)base; (void)event_id;
  mqtt.onEventCallback(static_cast<esp_mqtt_event_handle_t>(event_data));
}

// ==== Apply-Worker ====
// [B3] g_stateRev++ nur hier bei Strukturaenderungen
static void applyDo(ApplyJob &j) {
  uint8_t idx = j.idx;
  if (idx >= MAX_FANS) return;

  Fan &f = fans[idx];

  // DELETE
  if (j.deleteFan) {
    if (!isPinBlocked(f.pwmPin)) { ledcWrite(f.pwmPin, 0); ledcDetach(f.pwmPin); }
    detachFanCounters(idx);

    if (f.name[0]) {
      String sname = sanitizeName(String(f.name));
      if (mqtt.isConnected()) {
        String baseOld = topicDev() + "/" + sname;
        mqtt.publish((baseOld + "/speed").c_str(), "", 0, true);
        mqtt.unsubscribe((baseOld + "/set").c_str());
      } else {
        pendingCleanupQueue(sname.c_str());   // §18: bei Disconnect vormerken -> onMqttConnect raeumt nach
      }
    }

    clearFanNVS(idx);
    // Persistierten Duty im RICHTIGEN Namespace ("state") raeumen + RAM-Tracking nullen,
    // sonst erbt ein neuer Luefter im selben Slot nach Reboot die alte Drehzahl.
    { Preferences p; p.begin("state", false); p.remove((String("f") + idx + "_duty").c_str()); p.end(); }
    g_lastSavedDuty[idx] = 0;   // stale DutyCmd in g_dutyQ wird beim Drain via fanPresentIdx verworfen
    g_stateDirty[idx]    = false;
    fansLock();                         // §19: gegen Cross-Task-Leser (topicFan im esp-mqtt-Task)
    memset(&f, 0, sizeof(Fan));
    f.pwmPin = 0xFF; f.tachPin = 0xFF;
    fansUnlock();
    g_lastRpmSent[idx] = 0;

    rebuildPcntMap();
    g_stateRev.fetch_add(1);
    LOGI("APPLY", String("deleted idx=") + idx);
    return;
  }

  // Name change
  if (j.nameChanged) {
    if (f.name[0]) {  // alten Topic nur raeumen, wenn es einen gab (nicht bei Neuanlage)
      String sname = sanitizeName(String(f.name));
      if (mqtt.isConnected()) {
        String oldBase = topicDev() + "/" + sname;
        mqtt.publish((oldBase + "/speed").c_str(), "", 0, true);
        mqtt.unsubscribe((oldBase + "/set").c_str());
      } else {
        pendingCleanupQueue(sname.c_str());   // §18: bei Disconnect vormerken
      }
    }
    fansLock(); safeStrcpy(f.name, sizeof(f.name), String(j.name)); fansUnlock();   // §19
    if (mqtt.isConnected()) { mqttSubscribeFan(idx); mqttPublishSpeed(idx); }
    Preferences p; p.begin("fans", false);
    p.putString((String("f") + idx + "_name").c_str(), f.name);
    p.end();
    g_stateRev.fetch_add(1);
  }

  // Invert change
  if (j.invChanged) {
    f.invertPwm = j.invert;
    if (!isPinBlocked(f.pwmPin)) {
      ledcAttach(f.pwmPin, PWM_FREQ_HZ, PWM_BITS);
      ledcWrite(f.pwmPin, f.invertPwm ? (255 - f.duty) : f.duty);
    }
    Preferences p; p.begin("fans", false);
    p.putUChar((String("f") + idx + "_inv").c_str(), f.invertPwm ? 1 : 0);
    p.end();
    g_stateRev.fetch_add(1);
  }

  // Pins changed
  if (j.pinsChanged) {
    f.measureSuspended = true;
    if (!isPinBlocked(f.pwmPin)) { ledcWrite(f.pwmPin, 0); ledcDetach(f.pwmPin); }
    detachFanCounters(idx);
    delay(50);

    f.pwmPin  = j.pwmPin;
    f.tachPin = j.tachPin;

    if (!isPinBlocked(f.pwmPin)) {
      ledcAttach(f.pwmPin, PWM_FREQ_HZ, PWM_BITS);
      ledcWrite(f.pwmPin, f.invertPwm ? (255 - f.duty) : f.duty);
    }
    if (!isPinBlocked(f.tachPin)) pinMode(f.tachPin, INPUT_PULLUP);

    rebuildPcntMap();
    f.measureSuspended = false;

    Preferences p; p.begin("fans", false);
    String k = "f" + String(idx) + "_";
    p.putUChar((k + "pwm").c_str(), f.pwmPin);
    p.putUChar((k + "tac").c_str(), f.tachPin);
    p.end();
    g_stateRev.fetch_add(1);
  }

  // Calib change (§4.4: ueber die Queue statt direktem fans[]-Write in apiCalib)
  if (j.calChanged) {
    fansLock();
    fans[idx].calMinStart = j.calMin;
    safeStrcpy(fans[idx].calNote, sizeof(fans[idx].calNote), String(j.calNote));
    fansUnlock();
    Preferences p; p.begin("fans", false);
    String k = "f" + String(idx) + "_";
    p.putUChar((k + "cmin").c_str(), j.calMin);
    p.putString((k + "cnote").c_str(), j.calNote);
    p.end();
    g_stateRev.fetch_add(1);
  }

  LOGI("APPLY", String("done idx=") + idx);
}

// ==== HTTP Basics ====
static void httpSendHeaderOK(NetworkClient &c, const char *ctype) {
  c.println(F("HTTP/1.1 200 OK"));
  c.print(F("Content-Type: ")); c.println(ctype);
  c.println(F("Cache-Control: no-cache, no-store, must-revalidate"));
  c.println(F("Pragma: no-cache"));
  c.println(F("Connection: close"));
  c.println();
}
static void httpSend400(NetworkClient &c, const char *msg) {
  c.println(F("HTTP/1.1 400 Bad Request"));
  c.println(F("Content-Type: text/plain; charset=UTF-8"));
  c.println(F("Connection: close"));
  c.println(); c.println(msg);
}
static void httpSend500(NetworkClient &c, const char *msg) {
  c.println(F("HTTP/1.1 500 Internal Server Error"));
  c.println(F("Content-Type: text/plain; charset=UTF-8"));
  c.println(F("Connection: close"));
  c.println(); c.println(msg);
}
static void httpSend404(NetworkClient &c) {
  c.println(F("HTTP/1.1 404 Not Found"));
  c.println(F("Content-Type: text/plain; charset=UTF-8"));
  c.println(F("Connection: close"));
  c.println(); c.println(F("Not found"));
}
// ==== UI-Asset (gzip im Flash, chunked) ====
static void sendUiAsset(NetworkClient &c) {
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
    if (c.write(UI_ASSET + off, n) == 0) return;
    off += n;
    feedWdt();
  }
}

// ==== JSON-API-Antwort-Helfer ====
static void apiOk(NetworkClient &c) {
  httpSendHeaderOK(c, "application/json; charset=UTF-8");
  c.print(F("{\"ok\":true}"));
}
static void apiErr(NetworkClient &c, const char *msg) {  // nur statische msg!
  c.println(F("HTTP/1.1 400 Bad Request"));
  c.println(F("Content-Type: application/json; charset=UTF-8"));
  c.println(F("Connection: close"));
  c.println();
  c.print(F("{\"ok\":false,\"error\":\"")); c.print(msg); c.print(F("\"}"));
}

// Gesamtbudget des aktuellen Nicht-OTA-Requests (Spec §3.1). In handleClient gesetzt.
static uint32_t g_reqStart = 0;

static bool readLine(NetworkClient &c, String &out, unsigned long timeoutMs) {
  out = "";
  unsigned long t0 = millis();
  while (!elapsed(millis(), t0, timeoutMs) && !elapsed(millis(), g_reqStart, HTTP_REQ_BUDGET_MS)) {
    while (c.available()) {
      char ch = (char)c.read();
      if (ch == '\r') continue;
      if (ch == '\n') return true;
      out += ch;
      if (out.length() > 4096) return true;
    }
    feedWdt();
    delay(1);
  }
  return (out.length() > 0);
}

static bool readHeaders(NetworkClient &c, size_t &contentLength, String &contentType) {
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

// [B2] Buffer-Overflow fix: read max sizeof(b)-1 damit b[rd]=0 sicher ist
static bool handleBodyToString(NetworkClient &c, size_t contentLength, String &out) {
  out.reserve(contentLength + 8);
  size_t got = 0;
  unsigned long t0 = millis();
  // Form-Bodies sind auf 4 KB gecappt; zusaetzlich das 10s-Gesamtbudget (Spec §3.1).
  while (got < contentLength && !elapsed(millis(), t0, BODY_RD_TIMEOUT)
         && !elapsed(millis(), g_reqStart, HTTP_REQ_BUDGET_MS)) {
    int n = c.available();
    if (n > 0) {
      uint8_t b[256];
      int rd = c.read(b, min((int)(sizeof(b) - 1), (int)(contentLength - got)));
      if (rd > 0) {
        b[rd] = 0;
        out.concat((const char*)b);
        got += rd; t0 = millis();
      }
    } else { feedWdt(); delay(1); }
  }
  return (got == contentLength);
}

// ==== DRY: POST-Body Dispatcher ====
// Inline function-pointer statt typedef (Arduino autoproto Kompatibilitaet)
static bool handleFormPost(NetworkClient &c, size_t contentLength, const String &contentType,
                           void (*handler)(NetworkClient &, const String &)) {
  if (contentLength == 0 || contentLength > 4096) { httpSend400(c, "bad length"); return false; }
  if (!contentType.startsWith(F("application/x-www-form-urlencoded"))) {
    httpSend400(c, "Unsupported Content-Type");
    return false;
  }
  String body;
  if (!handleBodyToString(c, contentLength, body)) {
    httpSend500(c, "Body timeout");
    return false;
  }
  handler(c, body);
  return true;
}


// NetworkClient::write() kappt still bei Socket-Puffergroesse (4KB) und meldet
// trotzdem Erfolg -> NIE mehr als 1KB pro write() schicken (CLAUDE.md §6.11).
static void printChunked(NetworkClient &c, const char *s, size_t len) {
  while (len > 0) {
    size_t n = len < 1024 ? len : 1024;
    if (c.write((const uint8_t *)s, n) == 0) return;
    s += n; len -= n;
    feedWdt();
  }
}

static void handleLogTxt(NetworkClient &c)     { httpSendHeaderOK(c, "text/plain; charset=UTF-8"); logLock(); String snap = gLogBuf; logUnlock(); printChunked(c, snap.c_str(), snap.length()); }
static void handlePrevLogTxt(NetworkClient &c)  { httpSendHeaderOK(c, "text/plain; charset=UTF-8"); printChunked(c, gPrevLogTail.c_str(), gPrevLogTail.length()); }

// ==== JSON Status ====
static void sendJsonStatus(NetworkClient &c) {
  httpSendHeaderOK(c, "application/json");
  c.print(F("{\"rev\":")); c.print(g_stateRev.load());
  c.print(F(",\"device\":")); jsonPrintEscaped(c, deviceId.c_str());
  c.print(F(",\"ip\":")); jsonPrintEscaped(c, ethLocalIp().c_str());
  c.print(F(",\"mqtt_connected\":")); c.print(mqtt.isConnected() ? "true" : "false");
  c.print(F(",\"boot_count\":")); c.print(g_bootCount);
  c.print(F(",\"safe_mode\":")); c.print(g_crashLoopDetected ? "true" : "false");
  c.print(F(",\"reset_reason\":")); jsonPrintEscaped(c, g_resetReasonStr.c_str());
  c.print(F(",\"min_free_heap\":")); c.print(g_minFreeHeap.load());
  c.print(F(",\"largest_block\":")); c.print(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  c.print(F(",\"uptime_s\":")); c.print((uint32_t)(esp_timer_get_time() / 1000000ULL));
  c.print(F(",\"wdt\":true"));
  c.print(F(",\"core1_loops\":")); c.print(g_loopCtr.load());        // §8: Control-Liveness
  c.print(F(",\"net_loops\":")); c.print(g_netCtr.load());          // §8: networkTask-Liveness
  c.print(F(",\"net_stack_hwm\":")); c.print(g_netTaskHandle ? uxTaskGetStackHighWaterMark(g_netTaskHandle) : 0);  // §9
  c.print(F(",\"ota_pending\":")); c.print(g_otaPendingVerify.load() ? "true" : "false");
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
  // §4.3: alle pro-Luefter-Felder unter EINEM Lock snapshotten, dann serialisieren (nie Lock ueber Socket).
  struct FanView { char name[20], cnote[40]; bool occupied, present, validated, inv;
                   uint8_t duty, pwmPin, tachPin, calMinStart, fault; uint16_t rpm; };
  FanView v[MAX_FANS];
  fansLock();
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    Fan &f = fans[i];
    v[i].occupied = (f.name[0] != 0);
    v[i].present  = fanPresentIdx(i);
    { uint8_t n = 0; for (; n < 19 && f.name[n];    n++) v[i].name[n]  = f.name[n];    v[i].name[n]  = 0; }
    { uint8_t n = 0; for (; n < 39 && f.calNote[n]; n++) v[i].cnote[n] = f.calNote[n]; v[i].cnote[n] = 0; }
    v[i].duty = f.duty; v[i].pwmPin = f.pwmPin; v[i].tachPin = f.tachPin; v[i].calMinStart = f.calMinStart;
    v[i].rpm = f.rpmShown; v[i].fault = (uint8_t)f.fault; v[i].validated = f.validated; v[i].inv = f.invertPwm;
  }
  fansUnlock();
  c.print(F(",\"fans\":["));
  bool first = true;
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!v[i].occupied) continue;  // alle belegten Slots (auch unkonfigurierte)
    if (!first) c.print(','); first = false;
    c.print(F("{\"index\":")); c.print(i);
    c.print(F(",\"name\":")); jsonPrintEscaped(c, v[i].name);
    c.print(F(",\"present\":")); c.print(v[i].present ? "true" : "false");
    c.print(F(",\"pwm\":")); c.print(v[i].duty);
    c.print(F(",\"pct\":")); c.print((int)pctFromDuty(v[i].duty));
    c.print(F(",\"rpm\":")); c.print(v[i].rpm);
    c.print(F(",\"pwmPin\":")); c.print(v[i].pwmPin);
    c.print(F(",\"tachPin\":")); c.print(v[i].tachPin);
    c.print(F(",\"fault\":")); c.print((int)v[i].fault);
    c.print(F(",\"validated\":")); c.print(v[i].validated ? "true" : "false");
    c.print(F(",\"inv\":")); c.print(v[i].inv ? "true" : "false");
    c.print(F(",\"cmin\":")); c.print((int)pctFromDuty(v[i].calMinStart));
    c.print(F(",\"cnote\":")); jsonPrintEscaped(c, v[i].cnote);
    c.print(F("}"));
  }
  c.print(F("]}"));
}

// [Spec §3.2 / Review-Fund 2] Ein Image, das HTTP/OTA bedient, hat seine Lauffaehigkeit
// bewiesen -> als gueltig markieren, damit ein Reboot/zweites OTA INNERHALB des 90s-
// Health-Windows NICHT auf die alte FW zurueckrollt. Schwaecht den Anti-Brick-Schutz
// NICHT: ein Image, das nicht bootet/haengt, erreicht diesen Pfad nie (bleibt PENDING
// -> Rollback). Voraussetzung bleibt, dass der Safe-Mode HTTP+OTA am Leben haelt.
static void commitIfPending() {
  if (g_otaPendingVerify.exchange(false)) {   // idempotent: Doppelaufruf ungefaehrlich
    esp_ota_mark_app_valid_cancel_rollback();
    LOGI("OTA", "Image als VALID markiert (bewusster Reboot/OTA aus laufendem Image)");
  }
}

// Geordneter Abgang vor jedem Restart: ioBroker sieht sofort "offline",
// Log-Tail landet im NVS (zusaetzlich zum Shutdown-Handler).
static void prepareRestart() {
  commitIfPending();
  if (mqtt.isConnected()) {
    mqtt.publish((topicDev() + "/status").c_str(), "offline", 0, true);
    delay(50);   // dem MQTT-Task Zeit zum Senden geben (kein disconnect() in der Lib)
  }
  persistLogTail();
}

// ==== OTA ====
static bool handleOTA(NetworkClient &c, size_t contentLength) {
  // [Review-Fund 2] Laufendes Image zuerst gueltig markieren: es bedient gerade einen
  // OTA-Upload, ist also gesund. Sonst ruft Update.end()->set_boot_partition() aus einem
  // noch PENDING_VERIFY-Zustand (unklare Rollback-Ecke beim OTA INNERHALB des Health-Windows).
  commitIfPending();
  if (contentLength == 0) {
    httpSendHeaderOK(c, "application/json; charset=UTF-8");
    c.print(F("{\"ok\":false,\"error\":\"No Content or chunked unsupported\"}"));
    LOGE("OTA", "no content/chunked");
    return false;
  }
  if (contentLength > 0x200000) {  // > 2 MB passt in keinen App-Slot
    httpSendHeaderOK(c, "application/json; charset=UTF-8");
    c.print(F("{\"ok\":false,\"error\":\"too large\"}"));
    LOGE("OTA", "too large");
    return false;
  }
  if (!Update.begin(contentLength)) {
    httpSendHeaderOK(c, "application/json; charset=UTF-8");
    c.print(String("{\"ok\":false,\"error\":\"Update.begin failed: ") + Update.errorString() + "\"}");
    LOGE("OTA", "begin failed");
    return false;
  }
  const size_t BUFSZ = 1024;
  uint8_t buf[BUFSZ];
  size_t received = 0;
  unsigned long t0 = millis();
  int lastPct = -1;
  LOGI("OTA", String("start, size=") + contentLength + " bytes");
  while (received < contentLength && !elapsed(millis(), t0, BODY_RD_TIMEOUT)) {
    int n = c.read(buf, min((int)BUFSZ, (int)(contentLength - received)));
    if (n > 0) {
      feedWdt();
      size_t w = Update.write(buf, n);
      if (w != (size_t)n) {
        httpSendHeaderOK(c, "application/json; charset=UTF-8");
        c.print(F("{\"ok\":false,\"error\":\"Write error\"}"));
        Update.abort();
        LOGE("OTA", "write error");
        return false;
      }
      received += n; t0 = millis();
      int pct = (int)((received * 100UL) / contentLength);
      if (pct != lastPct && (pct % 5 == 0 || pct >= 99)) { LOGI("OTA", String("progress ") + pct + "%"); lastPct = pct; }
    } else { feedWdt(); delay(1); }
  }
  if (received != contentLength) {
    httpSendHeaderOK(c, "application/json; charset=UTF-8");
    c.print(F("{\"ok\":false,\"error\":\"Timeout or size mismatch\"}"));
    Update.abort();
    LOGE("OTA", "timeout/size mismatch");
    return false;
  }
  if (!Update.end()) {
    httpSendHeaderOK(c, "application/json; charset=UTF-8");
    c.print(String("{\"ok\":false,\"error\":\"Update.end failed: ") + Update.errorString() + "\"}");
    LOGE("OTA", "end failed");
    return false;
  }
  LOGI("OTA", "OK -> reboot");
  httpSendHeaderOK(c, "application/json; charset=UTF-8");
  c.print(F("{\"ok\":true,\"message\":\"Firmware erfolgreich aktualisiert\",\"reboot\":true,\"reboot_in\":5}"));
  c.flush(); prepareRestart(); delay(300); ESP.restart();
  return true;
}

// ==== JSON-API-Handler (schreibend -> Queues) ====
static void apiFanSet(NetworkClient &c, const String &body) {
  String s; long idx = -1, pct = -1;
  if (formGet(body, F("idx"), s)) idx = s.toInt();
  if (formGet(body, F("pct"), s)) pct = s.toInt();
  if (idx < 0 || idx >= MAX_FANS || !fanPresentIdx((uint8_t)idx) || pct < 0 || pct > 100) {
    apiErr(c, "bad idx/pct"); return;
  }
  dutyEnqueue((uint8_t)idx, dutyFromPct((uint8_t)pct));
  apiOk(c);
}

static void apiFanSave(NetworkClient &c, const String &body) {
  String s;
  if (!formGet(body, F("idx"), s) && !formGet(body, F("fan"), s)) { apiErr(c, "missing idx"); return; }
  int idx = s.toInt();
  // §4.3: alle Namen EINMAL unter Lock snapshotten — applyDo (Control-Core) schreibt fans[].name
  // parallel; sanitizeName liest das ganze Array (Multi-Byte) -> sonst torn read nach dem Split.
  char names[MAX_FANS][20];
  fansLock();
  for (uint8_t i = 0; i < MAX_FANS; i++) { uint8_t n = 0; for (; n < 19 && fans[i].name[n]; n++) names[i][n] = fans[i].name[n]; names[i][n] = 0; }
  fansUnlock();
  bool isNew = (idx < 0);   // idx<0 => neuer Luefter: ersten freien Slot ERST HIER belegen (kein Phantom-Slot beim Klick)
  if (isNew) {
    idx = -1;
    for (uint8_t i = 0; i < MAX_FANS; i++) if (names[i][0] == 0) { idx = (int)i; break; }
    if (idx < 0) { apiErr(c, "no free slot"); return; }
  }
  if (idx < 0 || idx >= MAX_FANS) { apiErr(c, "bad idx"); return; }
  Fan &f = fans[idx];   // nur fuer Einzelbyte-Felder (pwmPin/tachPin/invertPwm) — benigne Race

  String newName = String(names[idx]);
  if (formGet(body, F("name"), s)) newName = s;
  { String rt = newName; rt.trim(); if (rt.isEmpty()) { apiErr(c, "name required"); return; } }  // kein leerer Name (sonst sanitize->"fan"-Fallback = unsichtbarer Geist)
  String cleanName = sanitizeName(newName);
  if (!fanNameValid(cleanName.c_str())) { apiErr(c, "invalid name"); return; }
  for (uint8_t i = 0; i < MAX_FANS; i++) {  // Topic-Kollision (Spec §4): gegen ALLE belegten Slots, nicht nur present-e
    if ((int)i == idx || names[i][0] == 0) continue;
    if (sanitizeName(String(names[i])) == cleanName) { apiErr(c, "name in use"); return; }
  }

  bool inv = f.invertPwm;  // Default = Ist-Zustand: fehlt das Feld (Nicht-UI-Client), bleibt invert erhalten
  if (formGet(body, F("inv"), s)) inv = (s.toInt() == 1);
  uint8_t newPwm = f.pwmPin, newTac = f.tachPin;
  if (formGet(body, F("pwm"),  s)) newPwm = (uint8_t)constrain(s.toInt(), 0, 255);
  if (formGet(body, F("tach"), s)) newTac = (uint8_t)constrain(s.toInt(), 0, 255);
  if (newPwm != f.pwmPin && !validPwmForFan(newPwm,  (int8_t)idx)) { apiErr(c, "pwm pin invalid/busy"); return; }
  if (newTac != f.tachPin && !validTachForFan(newTac, (int8_t)idx)) { apiErr(c, "tach pin invalid/busy"); return; }
  if (isNew && (newPwm == 0xFF || newTac == 0xFF)) { apiErr(c, "pins required"); return; }  // neuer Luefter braucht Pins
  if (newPwm != 0xFF && newPwm == newTac) { apiErr(c, "pwm/tach pin gleich"); return; }     // ein GPIO kann nicht PWM UND Tacho

  ApplyJob j; j.idx = (uint8_t)idx;
  // isNew: Name IMMER schreiben — sonst bleibt bei cleanName=="fan" (leer/"Fan"/Sonderzeichen) nameChanged false
  // und der Slot wuerde mit Pins, aber ohne Namen angelegt = unsichtbarer Geist (nicht present).
  if (isNew || sanitizeName(String(names[idx])) != cleanName) { safeStrcpy(j.name, sizeof(j.name), cleanName); j.nameChanged = true; }
  if (inv != f.invertPwm) { j.invert = inv; j.invChanged = true; }
  if (newPwm != f.pwmPin || newTac != f.tachPin) { j.pwmPin = newPwm; j.tachPin = newTac; j.pinsChanged = true; }
  if (j.nameChanged || j.invChanged || j.pinsChanged) applyQueue((uint8_t)idx, j);
  apiOk(c);
}

static void apiFanDelete(NetworkClient &c, const String &body) {
  String s;
  if (!formGet(body, F("idx"), s)) { apiErr(c, "missing idx"); return; }
  int idx = s.toInt();
  // Jeder BELEGTE Slot (Name gesetzt) ist loeschbar — auch unkonfiguriert (ohne Pins).
  if (idx < 0 || idx >= MAX_FANS || fans[idx].name[0] == 0) { apiErr(c, "bad idx"); return; }
  ApplyJob j; j.idx = (uint8_t)idx; j.deleteFan = true;
  applyQueue((uint8_t)idx, j);
  apiOk(c);
}

static void apiCalib(NetworkClient &c, const String &body) {
  String s;
  if (!formGet(body, F("idx"), s)) { apiErr(c, "missing idx"); return; }
  int idx = s.toInt();
  if (idx < 0 || idx >= MAX_FANS || !fanPresentIdx((uint8_t)idx)) { apiErr(c, "bad idx"); return; }
  // §4.4: kein direkter fans[]-Write mehr — ueber die Apply-Queue an den Control-Core leiten.
  ApplyJob j; j.idx = (uint8_t)idx; j.calChanged = true;
  fansLock();                                  // Default = Ist-Wert (fehlt ein Feld, bleibt er erhalten)
  j.calMin = fans[idx].calMinStart;
  { uint8_t n = 0; for (; n < sizeof(j.calNote) - 1 && fans[idx].calNote[n]; n++) j.calNote[n] = fans[idx].calNote[n]; j.calNote[n] = 0; }
  fansUnlock();
  if (formGet(body, F("cmin"),  s)) j.calMin = dutyFromPct((uint8_t)constrain(s.toInt(), 0, 100));
  if (formGet(body, F("cnote"), s)) safeStrcpy(j.calNote, sizeof(j.calNote), s);
  applyQueue((uint8_t)idx, j);
  apiOk(c);
}

static void apiMqttSave(NetworkClient &c, const String &body) {
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
  LOGI("MQTT", "config saved -> reboot");
  apiOk(c);
  // esp-mqtt wird in setup() konfiguriert (kein sauberes Runtime-Reconfig) -> Neustart
  // übernimmt die neue Konfig. Reboot ist rollback-sicher (Image ist valid).
  c.flush();
  prepareRestart();
  delay(200);
  esp_restart();
}

static void apiSafeModeReset(NetworkClient &c, const String &body) {
  (void)body;
  Preferences p; p.begin("sys", false);
  p.putUChar("crash_streak", 0);
  p.putUChar("safe_mode", 0);
  p.end();
  apiOk(c);  // wirkt vollstaendig nach dem naechsten Reboot
}

static void apiReboot(NetworkClient &c, const String &body) {
  (void)body;
  apiOk(c);
  c.flush();
  prepareRestart();
  delay(200);
  ESP.restart();
}

// ==== Router ====
static void handleClient(NetworkClient &c) {
  g_reqStart = millis();  // [Spec §3.1] 10s-Gesamtbudget ab erster Zeile (OTA-Body liest direkt via c.read())
  String rl;
  if (!readLine(c, rl, CLIENT_RD_TIMEOUT)) return;
  String method = rl.substring(0, rl.indexOf(' '));
  String pathQuery = rl.substring(rl.indexOf(' ') + 1, rl.lastIndexOf(' '));
  String path = pathQuery;
  int q = pathQuery.indexOf('?');
  if (q >= 0) path = pathQuery.substring(0, q);  // Query wird in der JSON-API nicht genutzt

  size_t contentLength = 0;
  String contentType;
  if (!readHeaders(c, contentLength, contentType)) { httpSend400(c, "bad headers"); return; }
  // bewusst KEIN Request-Logging (NVS-Wear + Heap-Churn, Spec §3.5)

  // --- GET ---
  if (method == "GET") {
    if (path == "/")            { sendUiAsset(c); return; }
    if (path == "/api/status")  { sendJsonStatus(c); return; }
    if (path == "/log.txt")     { handleLogTxt(c); return; }
    if (path == "/prevlog.txt") { handlePrevLogTxt(c); return; }
  }

  // --- POST (JSON-API) ---
  if (method == "POST") {
    if (path == "/api/fan/set")        { handleFormPost(c, contentLength, contentType, apiFanSet); return; }
    if (path == "/api/fan/save")       { handleFormPost(c, contentLength, contentType, apiFanSave); return; }
    if (path == "/api/fan/delete")     { handleFormPost(c, contentLength, contentType, apiFanDelete); return; }
    if (path == "/api/calib")          { handleFormPost(c, contentLength, contentType, apiCalib); return; }
    if (path == "/api/mqtt")           { handleFormPost(c, contentLength, contentType, apiMqttSave); return; }
    if (path == "/api/safemode/reset") { apiSafeModeReset(c, String("")); return; }
    if (path == "/api/reboot")         { apiReboot(c, String("")); return; }
    if (path == "/ota") {
      if (contentType.startsWith(F("application/octet-stream"))) (void)handleOTA(c, contentLength);
      else httpSend400(c, "Use application/octet-stream");
      return;
    }
  }

  httpSend404(c);
}

// ==== Ethernet State ====
static uint32_t g_linkLostSince = 0, g_lastEthReinit = 0;

// ==== setup() helpers ====
static void ledcInitAllPresentToZero() {
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i) || isPinBlocked(fans[i].pwmPin)) continue;
    ledcAttach(fans[i].pwmPin, PWM_FREQ_HZ, PWM_BITS);
    ledcWrite(fans[i].pwmPin, 0);
  }
}
static void tachInitAllPresentPullups() {
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i) || isPinBlocked(fans[i].tachPin)) continue;
    pinMode(fans[i].tachPin, INPUT_PULLUP);
  }
}

// [B1] Gespeicherte Duty-Werte aus NVS laden und per Queue anwenden
static void restoreSavedDuties() {
  Preferences p; p.begin("state", true);
  uint8_t restored = 0;
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i)) continue;
    String k = "f" + String(i) + "_duty";
    uint8_t saved = p.getUChar(k.c_str(), 0);
    if (saved > 0) {
      dutyEnqueue(i, saved);
      restored++;
    }
  }
  p.end();
  if (restored > 0) LOGI("STATE", String("restored ") + restored + " duty values from NVS");
}

// §5.3: Netz-Task auf Core 0 — Link-Watch + HTTP + Telemetrie-/Log-Drains.
// Der Control-Loop bleibt im Arduino-loopTask (Core 1). Dieser Task wird beim TWDT abonniert,
// damit ein Hang (z.B. W5500-SPI, §12A) zum Panic-Reboot -> Bootloader-Rollback fuehrt.
static void networkTask(void *arg) {
  (void)arg;
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();
    g_netCtr.fetch_add(1);
    uint32_t now = millis();

    // Netz-Status aus async ETH-Events
    bool ipNow = ethHasIp();
    if (ipNow && !httpUp.load()) { httpServer.begin(); ethStartMdns(); httpUp.store(true); LOGI("NET", String("IP: ") + ethLocalIp()); }
    if (!ipNow && httpUp.load() && !ethLinkUp()) httpUp.store(false);   // Link weg -> Server pausiert
    ethHasIP = ipNow;
    // Link > 15 s weg -> harter Treiber-Reset (Cooldown 5 s). DHCP-Lease erneuert esp-netif selbst.
    if (!ethLinkUp()) {
      if (g_linkLostSince == 0) g_linkLostSince = now;
      if (elapsed(now, g_linkLostSince, ETH_LINK_LOST_RESET_MS) && elapsed(now, g_lastEthReinit, ETH_REINIT_COOLDOWN_MS)) {
        LOGW("NET", "link down >15s -> ETH hard reset");
        g_lastEthReinit = now;
        httpUp.store(false);
        ethHardReset();
      }
    } else {
      g_linkLostSince = 0;
    }

    // HTTP
    if (httpUp.load() && ethHasIP) {
      NetworkClient c = httpServer.accept();
      if (c) {
        uint32_t t0 = millis();
        while (c.connected() && !c.available() && !elapsed(millis(), t0, 400)) { feedWdt(); delay(1); }
        if (!c.available()) c.stop();
        else { handleClient(c); delay(1); c.stop(); }
      }
    }

    telemDrain();   // Telemetrie -> MQTT publishen (Netz-Kontext)
    logDrain();     // Log-Queue -> gLogBuf
    delay(2);
  }
}

// ==== setup() ====
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("== ESP32-S3-ETH Fan Controller v5.0 =="));

  // §4: Cross-Core-Queues + fans[]-/Cleanup-Mutex VOR allem anderen anlegen (esp-mqtt-Task
  // startet erst mit loopStart() weiter unten; Log-Queue (spaeter) faengt ab hier alle LOGs).
  concurrencyInit();
  g_applyQ = xQueueCreate(MAX_FANS, sizeof(ApplyJob));   // hier, da sizeof(ApplyJob) erst im Sketch sichtbar
  fansMutex = xSemaphoreCreateMutex();
  logMutex  = xSemaphoreCreateMutex();

  // Anti-Brick: Task-WDT bleibt AKTIV und ueberwacht den Loop-Task.
  // 8 s Budget; Panic => Reboot => ggf. Bootloader-Rollback (Spec §3.1/3.2).
  {
    esp_task_wdt_config_t wcfg = {};
    wcfg.timeout_ms     = 8000;
    wcfg.idle_core_mask = (1 << 0);
    wcfg.trigger_panic  = true;
    esp_task_wdt_reconfigure(&wcfg);
  }
  enableLoopWDT();

  {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
      g_otaPendingVerify.store(true);
      LOGW("OTA", "Image PENDING_VERIFY - Health-Window 90s laeuft");
    }
  }

  loadPrevLogTail();
  bootTrackInit();

  // Log-Tail bei jedem geordneten Reboot sichern (ESP.restart ruft Handler).
  esp_register_shutdown_handler([]() { persistLogTail(); });

  disableRadios();
  safetyZeroPins();

  loadConfigFans();
  loadMQTTConfig();
  buildMacAndDeviceId();
  // (Duty-Queue wird in concurrencyInit() oben angelegt — kein separates Init mehr)

  // Hardware auf 0, dann Tach-Pullups
  ledcInitAllPresentToZero();
  tachInitAllPresentPullups();

  for (uint8_t i = 0; i < MAX_FANS; i++)
    if (fanPresentIdx(i)) fanAutoValidate(i);

  rebuildPcntMap();

  // [Spec §3.4/3.7] Luefter drehen BEVOR das Netz initialisiert wird.
  if (g_crashLoopDetected) {
    // SAFE MODE: feste 70 % Failsafe, kein Restore, MQTT bleibt aus (loop).
    for (uint8_t i = 0; i < MAX_FANS; i++)
      if (fanPresentIdx(i)) dutyEnqueue(i, dutyFromPct(70));
    LOGW("SAFE", "crash loop erkannt -> Failsafe 70%, MQTT aus");
  } else {
    restoreSavedDuties();  // [B1] (Hardware ist bereits auf 0)
  }
  dutyProcessQueue();

  // [Stufe2/1] Nativer W5500-Treiber (ETH.h). DHCP laeuft async; das GOT_IP-Event
  // setzt g_ethHasIp; httpServer wird in loop() gestartet, sobald die IP da ist.
  feedWdt();
  if (!ethBegin()) LOGE("NET", "ETH.begin failed");
  feedWdt();

  // [Stufe2/2] esp-mqtt zuletzt starten (Task läuft eigenständig, Auto-Reconnect; onMqttConnect
  // feuert erst, wenn alles oben initialisiert ist). Nur wenn aktiviert + nicht im Safe-Mode.
  // Client-ID/LWT-Topic in globalen Strings halten — die Lib speichert nur den Pointer.
  if (mqttConfig.enabled && strlen(mqttConfig.host) > 0 && !g_crashLoopDetected) {
    g_mqttClientId = deviceId + "-cli";
    g_mqttLwtTopic = topicDev() + "/status";
    mqtt.setMqttClientName(g_mqttClientId.c_str());
    if (strlen(mqttConfig.user) > 0) mqtt.setURL(mqttConfig.host, mqttConfig.port, mqttConfig.user, mqttConfig.pass);
    else                             mqtt.setURL(mqttConfig.host, mqttConfig.port);
    mqtt.enableLastWillMessage(g_mqttLwtTopic.c_str(), "offline", true);
    mqtt.setKeepAlive(30);
    mqtt.loopStart();
    LOGI("MQTT", String("esp-mqtt -> ") + mqttConfig.host + ":" + mqttConfig.port);
  }

  LOGI("BOOT", "Setup complete.");
  logDrain();   // §4.6: Setup-Log-Burst aus der Queue in gLogBuf flushen (vor Task-Start)

  // §5.3: Netz-Task auf Core 0 starten (Control bleibt loopTask/Core 1, anderer Core -> keine
  // Prio-Konkurrenz). Stack 8 KB konservativ (HTTP, kein TLS) -> in Stufe 9 per
  // uxTaskGetStackHighWaterMark pruefen. Prio 5 = wie der esp-mqtt-Task (beide auf Core 0; net
  // yieldet per delay(2), daher keine MQTT-Starvation).
  xTaskCreatePinnedToCore(networkTask, "net", 8192, nullptr, 5, &g_netTaskHandle, 0);
}

// ==== loop() ====
void loop() {
  static uint32_t lastMeasureTick = 0;
  const uint32_t now = millis();
  g_loopCtr.fetch_add(1);   // §8: Control-Loop-Liveness (Heartbeat)

  // [Stufe2/1 + Stufe3] Netz-gebundenes Health-Window. NACH dem Dual-Core-Split beweist "IP da"
  // allein NICHT mehr Erreichbarkeit: der W5500-Treiber-Task haelt die IP auch dann, wenn der
  // networkTask defekt ist (Stack/HTTP-begin) -> sonst Soft-Brick. Daher Commit nur mit POSITIVEM
  // Beweis: HTTP hochgebracht (httpUp) UND networkTask tickt (Heartbeat in den letzten 5s).
  // Crash/Hang des networkTask faengt separat der TWDT (Panic-Reboot). Nie erreichbar bis 120s
  // -> Selbst-Neustart OHNE commit -> Bootloader-Rollback aufs vorherige Image.
  if (g_otaPendingVerify.load()) {
    static uint32_t lastNetCtr = 0, lastNetChkMs = 0;
    static bool netTicking = false;
    if (lastNetChkMs == 0) lastNetChkMs = now;
    if (elapsed(now, lastNetChkMs, 5000)) {
      uint32_t nc = g_netCtr.load();
      netTicking = (nc != lastNetCtr);
      lastNetCtr = nc; lastNetChkMs = now;
    }
    bool reachable = ethHasIp() && httpUp.load() && netTicking;
    if (now > OTA_HEALTH_MS && reachable) {
      commitIfPending();
    } else if (now > 120000UL && !reachable) {
      LOGE("OTA", "nicht erreichbar (IP+HTTP+net-heartbeat) in 120s -> Selbst-Neustart, Rollback aufs vorherige Image");
      persistLogTail();
      delay(100);
      esp_restart();   // ohne commit -> PENDING_VERIFY bleibt -> Bootloader-Rollback
    }
  }

  uint32_t hf = ESP.getFreeHeap();
  if (hf < g_minFreeHeap.load()) g_minFreeHeap.store(hf);

  // --- Apply-Queue ---
  { ApplyJob j;
    while (g_applyQ && xQueueReceive(g_applyQ, &j, 0) == pdTRUE) applyDo(j); }

  // --- Duty-Queue ---
  dutyProcessQueue();

  // --- Netz (Link-Watch + HTTP) + MQTT laufen jetzt im networkTask (Core 0) bzw. esp-mqtt-Task ---

  // --- Storm-Recovery ---
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i)) continue;
    stormTryRecover(i);
  }

  // --- RPM-Messung ---
  bool anyBurst = false;
  for (uint8_t i = 0; i < MAX_FANS; i++)
    if (fanPresentIdx(i) && fans[i].burstLeft > 0) { anyBurst = true; break; }
  uint32_t interval = anyBurst ? RPM_BURST_INTERVAL_MS : RPM_BASE_INTERVAL_MS;

  if (now - lastMeasureTick >= interval) {
    lastMeasureTick = now;

    for (uint8_t i = 0; i < MAX_FANS; i++) {
      if (!fanPresentIdx(i)) continue;
      Fan &f = fans[i];

      if (f.stormActive) {
        f.rpmRawA = f.rpmRawB = f.rpmRawC = 0;
        if (f.rpmEma > 0.01f) f.rpmEma *= (1.0f - EMA_ALPHA);
        else f.rpmEma = 0;
        f.rpmShown = (uint16_t)roundf(f.rpmEma);
        continue;
      }

      uint32_t pulsesDelta = 0;
      if (f.pcntEnabled) {
        pulsesDelta = pcntReadDeltaAndClear(i);
      } else {
        uint32_t pulses = f.pulseCount;
        pulsesDelta = pulses - f.lastPulseCount;
        f.lastPulseCount = pulses;
      }

      // Storm-Budget
      if (f.pcntEnabled) {
        if (pulsesDelta > PCNT_STORM_BUDGET) { stormTrip(i); continue; }
      } else {
        if (pulsesDelta > ISR_STORM_BUDGET)  { stormTrip(i); continue; }
      }

      // Fault-Logik
      if (!f.validated) {
        f.rpmRawC = 0;
        fanMarkFault(i, FF_OK);
      } else {
        if (f.duty >= max<uint8_t>(1, f.calMinStart) && pulsesDelta == 0)
          fanMarkFault(i, FF_NO_TACH_PULSES);
        else
          fanMarkFault(i, FF_OK);
      }

      // RPM berechnen + glaetten
      uint16_t rpmSample = 0;
      if (f.duty == 0 || f.measureSuspended) {
        rpmSample = 0;
      } else {
        uint32_t rpm = (pulsesDelta * (60000UL / interval)) / max<uint8_t>(1, PULSES_PER_REV);
        if (rpm > RPM_HARD_CAP) rpm = RPM_HARD_CAP;
        rpmSample = (uint16_t)rpm;
      }

      // Median-of-3
      f.rpmRawA = f.rpmRawB;
      f.rpmRawB = f.rpmRawC;
      f.rpmRawC = rpmSample;
      uint16_t a = f.rpmRawA, b = f.rpmRawB, cc = f.rpmRawC;
      uint16_t med = max<uint16_t>(min<uint16_t>(a, b), min<uint16_t>(max<uint16_t>(a, b), cc));

      // EMA
      if (f.rpmEma <= 0.01f) f.rpmEma = (float)med;
      else f.rpmEma = EMA_ALPHA * (float)med + (1.0f - EMA_ALPHA) * f.rpmEma;
      f.rpmShown = (uint16_t)roundf(f.rpmEma);

      if (f.burstLeft > 0) f.burstLeft--;

      telemPostRpm(i);   // §4.1: Telemetrie in die Queue (Netz-Task published)
    }
  }

  // --- Duty-State persistieren ---
  stateFlushIfNeeded(now);

  // --- Log-Tail persistieren ---
  static uint32_t lastLogFlush = 0;
  if (now - lastLogFlush >= LOG_FLUSH_MS) {
    lastLogFlush = now;
    persistLogTail();
  }

  delay(2);
  yield();
}

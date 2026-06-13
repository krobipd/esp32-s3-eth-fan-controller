/**************************************************************
 * WAVESHARE ESP32-S3-ETH Fan Controller v4.0 (Stufe 1: Haertung + neues UI)
 * Board: Waveshare ESP32-S3-ETH (W5500 via SPI2, PoE)
 * Arduino IDE 2.3.7 | ESP32 Arduino Core 3.3.5
 * W5500 Pins: MISO=12, MOSI=11, SCLK=13, CS=14, RST=9
 * Board-Setting: "USB CDC On Boot: Enabled" ist PFLICHT
 **************************************************************/

// --- Arduino autoproto fix ---
#include <stdint.h>
enum FanFault : uint8_t;
struct Fan;
struct ApplyJob;

// ==== Includes ====
#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <Update.h>
#include <PubSubClient.h>
#include <Preferences.h>

#include "fw_util.h"

#include "esp_wifi.h"
#include "esp_bt.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"

extern "C" {
#include "driver/pcnt.h"
#include "esp_idf_version.h"
}

// [Spec §3.2] Boot-Auto-Validierung aufschieben: WIR markieren das Image erst
// nach gesundem Lauf-Fenster als gueltig. Crash/WDT vorher => Bootloader-Rollback.
// (Definition NACH allen Includes — Arduino-Autoproto, siehe CLAUDE.md §4.)
extern "C" bool verifyRollbackLater() { return true; }

static bool g_otaPendingVerify = false;
static const uint32_t OTA_HEALTH_MS = 90000;

// ==== Board-Pins (Waveshare ESP32-S3-ETH, W5500) ====
#define ETH_MISO 12
#define ETH_MOSI 11
#define ETH_SCLK 13
#define ETH_CS   14
#define ETH_RST   9

// ==== Limits & Timing ====
static const uint8_t  MAX_FANS              = 8;
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
static const size_t        LOG_MAX           = 8192;
static const uint32_t      LOG_FLUSH_MS      = 8000;
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

// Ethernet
static const unsigned long DHCP_RETRY_MS           = 3000;
static const uint32_t      ETH_REINIT_COOLDOWN_MS  = 5000;
static const uint32_t      ETH_LINK_LOST_RESET_MS  = 15000;
static const uint32_t      ETH_MAINTAIN_INTERVAL_MS = 10000;

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
static uint32_t g_minFreeHeap = UINT32_MAX;
static String   deviceId;
static uint8_t  g_mac[6];

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

static char g_logLineBuf[256];

static void logLine(const char *s) {
  size_t slen = strlen(s);
  if (gLogBuf.length() + slen + 1 > LOG_MAX) {
    int cut = (gLogBuf.length() + slen + 1) - LOG_MAX + 256;
    if (cut < (int)gLogBuf.length()) gLogBuf.remove(0, cut);
    else gLogBuf = "";
  }
  gLogBuf += s; gLogBuf += '\n';
  Serial.println(s);
}

static void logFmt(char level, const char *tag, const char *msg) {
  uint32_t ms = millis();
  snprintf(g_logLineBuf, sizeof(g_logLineBuf),
           "[T+%04lu.%03lus #%lu] [%c] %s: %s",
           (unsigned long)(ms / 1000UL), (unsigned long)(ms % 1000UL),
           (unsigned long)g_bootCount, level, tag, msg);
  logLine(g_logLineBuf);
}

#define LOGI(t, m) logFmt('I', t, (String(m)).c_str())
#define LOGW(t, m) logFmt('W', t, (String(m)).c_str())
#define LOGE(t, m) logFmt('E', t, (String(m)).c_str())

static void persistLogTail() {
  if (gLogBuf.isEmpty()) return;
  String tail = gLogBuf;
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
static bool parseQueryParamStr(const String &qs, const String &key, String &out) {
  String k = key + "="; int p = qs.indexOf(k); if (p < 0) return false;
  int s = p + k.length(); int e = qs.indexOf('&', s);
  out = urlDecode((e < 0) ? qs.substring(s) : qs.substring(s, e)); return true;
}
static bool parseQueryParam(const String &qs, const String &key, long &out) {
  String tmp; if (!parseQueryParamStr(qs, key, tmp)) return false; out = tmp.toInt(); return true;
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
  p.putUChar((k + "duty").c_str(), 0);
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
static uint32_t g_stateRev = 0;

// ==== Ethernet/MQTT Objects ====
EthernetServer httpServer(80);
EthernetClient ethClient;
PubSubClient   mqtt(ethClient);

// ==== MQTT Helpers ====
static String topicDev() { return String(mqttConfig.prefix) + "/" + deviceId; }
static String topicFan(uint8_t i) { return topicDev() + "/fan/" + sanitizeName(fans[i].name); }

// pctFromDuty/dutyFromPct kommen aus fw_util.h (host-getestet)

// Vorab-Prototypen
static void mqttPublishPWM(uint8_t i);
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
static void resetW5500() {
  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, LOW);
  delay(50);
  digitalWrite(ETH_RST, HIGH);
  delay(200);
  SPI.end();
  SPI.begin(ETH_SCLK, ETH_MISO, ETH_MOSI, ETH_CS);
  SPI.setFrequency(20000000);
  Ethernet.init(ETH_CS);
}

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
static void jsonPrintEscaped(EthernetClient &c, const char *s) {
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
  mqttPublishPWM(idx);
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
};

static ApplyJob g_apply[MAX_FANS];
static bool     g_anyApplyPending = false;

static void applyQueue(uint8_t idx, const ApplyJob &src) {
  if (idx >= MAX_FANS) return;
  g_apply[idx] = src;
  g_apply[idx].active = true;
  g_anyApplyPending = true;
  LOGI("APPLY", String("queued idx=") + idx + " del=" + (src.deleteFan ? "1" : "0") +
       " pins=" + (src.pinsChanged ? "1" : "0") + " name=" + (src.nameChanged ? "1" : "0"));
}

// ==== Duty-Queue (HTTP/MQTT entkoppelt) ====
static volatile int16_t g_pendingDuty[MAX_FANS];
static volatile bool    g_pendingDutyAny = false;

static void dutyPendingInit() {
  for (uint8_t i = 0; i < MAX_FANS; i++) g_pendingDuty[i] = -1;
  g_pendingDutyAny = false;
}
static inline void dutyEnqueue(uint8_t idx, uint8_t duty) {
  if (idx >= MAX_FANS || !fanPresentIdx(idx)) return;
  g_pendingDuty[idx] = (int16_t)duty;
  g_pendingDutyAny = true;
}
static void dutyProcessQueue() {
  if (!g_pendingDutyAny) return;
  bool any = false;
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    int16_t d = g_pendingDuty[i];
    if (d >= 0) {
      g_pendingDuty[i] = -1;
      fanSetDuty(fans[i], (uint8_t)d);
      onDutyChanged(i);
    }
    if (g_pendingDuty[i] >= 0) any = true;
  }
  g_pendingDutyAny = any;
}

// ==== MQTT Publish/Subscribe ====
static uint16_t g_lastRpmSent[MAX_FANS] = {0};

static void mqttPublishPWM(uint8_t i) {
  if (!mqtt.connected() || !fanPresentIdx(i)) return;
  char b[4]; snprintf(b, sizeof(b), "%u", (unsigned)pctFromDuty(fans[i].duty));
  String t = topicFan(i) + "/pct";
  mqtt.publish(t.c_str(), b, true);
}

static void mqttPublishRPM(uint8_t i, bool force) {
  if (!mqtt.connected() || !fanPresentIdx(i)) return;
  uint32_t now = millis();
  uint16_t rpm = fans[i].rpmShown;
  bool due = force;
  if (!due) {
    if (fans[i].lastRpmPubMs == 0) due = true;
    else if (now - fans[i].lastRpmPubMs >= MQTT_PUB_MS_KEEPALIVE) due = true;
    else if (abs((int)rpm - (int)g_lastRpmSent[i]) >= MQTT_RPM_ABS_DELTA) due = true;
  }
  if (!due) return;

  char b[12]; snprintf(b, sizeof(b), "%u", (unsigned)rpm);
  String t = topicFan(i) + "/rpm";
  mqtt.publish(t.c_str(), b, false);
  fans[i].lastRpmPubMs = now;
  g_lastRpmSent[i] = rpm;
}

static void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String t(topic);
  if (!t.endsWith(F("/set"))) return;

  char buf[8];
  unsigned int n = min(length, (unsigned)sizeof(buf) - 1), j = 0;
  for (unsigned int i = 0; i < n; i++) if (payload[i] >= 32) buf[j++] = (char)payload[i];
  buf[j] = 0;
  int pct = constrain(atoi(buf), 0, 100);

  String pre = topicDev() + "/fan/";
  if (!t.startsWith(pre)) return;
  int p = t.indexOf(F("/set"), pre.length());
  if (p < 0) return;
  String fanKey = t.substring(pre.length(), p);

  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i)) continue;
    if (sanitizeName(fans[i].name) == fanKey) {
      dutyEnqueue(i, dutyFromPct((uint8_t)pct));
      LOGI("MQTT", String("set ") + fans[i].name + " -> " + pct + "%");
      return;
    }
  }
  LOGW("MQTT", "set for unknown fan");
}

static bool ethHasIP = false, httpUp = false;
static bool mqttWasConnected = false;
static uint32_t g_lastMqttTryMs = 0;
static uint32_t g_mqttBackoffMs = 3000;

static void mqttEnsureConnected() {
  if (!mqttConfig.enabled || strlen(mqttConfig.host) == 0 || !ethHasIP) { mqttWasConnected = false; return; }
  uint32_t now = millis();

  if (mqtt.connected()) {
    if (!mqttWasConnected) {
      String willTopic = topicDev() + "/status";
      mqtt.publish(willTopic.c_str(), "online", true);
      for (uint8_t i = 0; i < MAX_FANS; i++) {
        if (!fanPresentIdx(i)) continue;
        mqtt.subscribe((topicFan(i) + "/set").c_str());
        mqttPublishPWM(i);
      }
      mqttWasConnected = true;
      g_mqttBackoffMs = 3000;
      g_lastMqttTryMs = now;
      LOGI("MQTT", "connected");
    }
    return;
  }

  if (!elapsed(now, g_lastMqttTryMs, g_mqttBackoffMs)) return;
  g_lastMqttTryMs = now;

  mqtt.setServer(mqttConfig.host, mqttConfig.port);
  mqtt.setCallback(mqttCallback);

  String willTopic = topicDev() + "/status";
  bool ok = (strlen(mqttConfig.user) > 0)
              ? mqtt.connect((deviceId + "-cli").c_str(), mqttConfig.user, mqttConfig.pass, willTopic.c_str(), 1, true, "offline")
              : mqtt.connect((deviceId + "-cli").c_str(), NULL, NULL, willTopic.c_str(), 1, true, "offline");

  if (ok) {
    mqttWasConnected = false;
  } else {
    LOGW("MQTT", String("connect failed, state=") + mqtt.state());
    g_mqttBackoffMs = min<uint32_t>(g_mqttBackoffMs * 2, 60000);
  }
}

// ==== Apply-Worker ====
// [B3] g_stateRev++ nur hier bei Strukturaenderungen
static void applyDo(uint8_t idx) {
  if (idx >= MAX_FANS) return;
  ApplyJob &j = g_apply[idx];
  if (!j.active) return;

  Fan &f = fans[idx];

  // DELETE
  if (j.deleteFan) {
    if (!isPinBlocked(f.pwmPin)) { ledcWrite(f.pwmPin, 0); ledcDetach(f.pwmPin); }
    detachFanCounters(idx);

    if (mqtt.connected() && f.name[0]) {
      String baseOld = String(mqttConfig.prefix) + "/" + deviceId + "/fan/" + sanitizeName(String(f.name));
      mqtt.publish((baseOld + "/pct").c_str(), "", true);
      mqtt.unsubscribe((baseOld + "/set").c_str());
    }

    clearFanNVS(idx);
    memset(&f, 0, sizeof(Fan));
    f.pwmPin = 0xFF; f.tachPin = 0xFF;
    g_lastRpmSent[idx] = 0;

    rebuildPcntMap();
    g_stateRev++;
    j.active = false;
    LOGI("APPLY", String("deleted idx=") + idx);
    return;
  }

  // Name change
  if (j.nameChanged) {
    if (mqtt.connected()) {
      String oldBase = String(mqttConfig.prefix) + "/" + deviceId + "/fan/" + sanitizeName(String(f.name));
      mqtt.publish((oldBase + "/pct").c_str(), "", true);
      mqtt.unsubscribe((oldBase + "/set").c_str());
      safeStrcpy(f.name, sizeof(f.name), String(j.name));
      mqtt.subscribe((topicFan(idx) + "/set").c_str());
      mqttPublishPWM(idx);
    } else {
      safeStrcpy(f.name, sizeof(f.name), String(j.name));
    }
    Preferences p; p.begin("fans", false);
    p.putString((String("f") + idx + "_name").c_str(), f.name);
    p.end();
    g_stateRev++;
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
    g_stateRev++;
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
    g_stateRev++;
  }

  j.active = false;
  LOGI("APPLY", String("done idx=") + idx);
}

// ==== HTTP Basics ====
static void httpSendHeaderOK(EthernetClient &c, const char *ctype) {
  c.println(F("HTTP/1.1 200 OK"));
  c.print(F("Content-Type: ")); c.println(ctype);
  c.println(F("Cache-Control: no-cache, no-store, must-revalidate"));
  c.println(F("Pragma: no-cache"));
  c.println(F("Connection: close"));
  c.println();
}
static void httpSend400(EthernetClient &c, const char *msg) {
  c.println(F("HTTP/1.1 400 Bad Request"));
  c.println(F("Content-Type: text/plain; charset=UTF-8"));
  c.println(F("Connection: close"));
  c.println(); c.println(msg);
}
static void httpSend500(EthernetClient &c, const char *msg) {
  c.println(F("HTTP/1.1 500 Internal Server Error"));
  c.println(F("Content-Type: text/plain; charset=UTF-8"));
  c.println(F("Connection: close"));
  c.println(); c.println(msg);
}
static void httpSendRedirect(EthernetClient &c, const char *loc) {
  c.println(F("HTTP/1.1 303 See Other"));
  c.print(F("Location: ")); c.println(loc);
  c.println(F("Connection: close"));
  c.println();
}

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
    feedLoopWDT();
    delay(1);
  }
  return (out.length() > 0);
}

static void readHeaders(EthernetClient &c, size_t &contentLength, String &contentType) {
  contentLength = 0;
  contentType   = "";
  bool teChunked = false;
  String h;
  while (readLine(c, h, CLIENT_RD_TIMEOUT) && h.length() > 0) {
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
}

// [B2] Buffer-Overflow fix: read max sizeof(b)-1 damit b[rd]=0 sicher ist
static bool handleBodyToString(EthernetClient &c, size_t contentLength, String &out) {
  out.reserve(contentLength + 8);
  size_t got = 0;
  unsigned long t0 = millis();
  while (got < contentLength && !elapsed(millis(), t0, BODY_RD_TIMEOUT)) {
    int n = c.available();
    if (n > 0) {
      uint8_t b[256];
      int rd = c.read(b, min((int)(sizeof(b) - 1), (int)(contentLength - got)));
      if (rd > 0) {
        b[rd] = 0;
        out.concat((const char*)b);
        got += rd; t0 = millis();
      }
    } else { feedLoopWDT(); delay(1); }
  }
  return (got == contentLength);
}

// ==== DRY: POST-Body Dispatcher ====
// Inline function-pointer statt typedef (Arduino autoproto Kompatibilitaet)
static bool handleFormPost(EthernetClient &c, size_t contentLength, const String &contentType,
                           void (*handler)(EthernetClient &, const String &)) {
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

// ==== DRY: Pin-Dropdown Rendering ====
static void renderPinDropdown(EthernetClient &c, const char *fieldName, const uint8_t *allowed, size_t count,
                               bool (*checkFn)(uint8_t), uint8_t currentPin, int8_t fanIdx) {
  c.print(F("<select name='")); c.print(fieldName); c.print(F("'>"));
  for (size_t k = 0; k < count; k++) {
    uint8_t p = allowed[k];
    bool ok = ((p == currentPin) || (!pinInUse(p, fanIdx) && checkFn(p)));
    if (!ok) continue;
    c.print(F("<option value='")); c.print(p); c.print(F("'"));
    if (p == currentPin) c.print(F(" selected"));
    c.print(F(">GPIO ")); c.print(p); c.print(F("</option>"));
  }
  c.print(F("</select>"));
}

// ==== UI Frame ====
static void uiHeader(EthernetClient &c, const char *title) {
  httpSendHeaderOK(c, "text/html; charset=UTF-8");
  c.print(F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>"));
  c.print(title);
  c.print(F("</title>"
            "<style>"
            ":root{--bg:#0b0f14;--card:#111825;--muted:#9aa4b2;--fg:#e5e7eb;--pri:#1b2533;--bd:#223047;--ok:#11b981;--warn:#f59e0b;--err:#ef4444}"
            "html,body{height:100%} body{margin:0;background:var(--bg);color:var(--fg);font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial}"
            "header{position:sticky;top:0;background:rgba(11,15,20,.85);backdrop-filter:saturate(180%) blur(8px);border-bottom:1px solid var(--bd);z-index:1000}"
            ".wrap{max-width:1080px;margin:0 auto;padding:16px} h1{font-size:22px;margin:0}"
            ".row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}"
            "a,button,input,select{font:inherit}"
            ".btn{padding:8px 12px;border-radius:10px;border:1px solid var(--bd);background:var(--pri);color:var(--fg);text-decoration:none;cursor:pointer;pointer-events:auto;position:relative;z-index:2}"
            ".btn:hover{filter:brightness(1.08)} .btn.danger{border-color:#4b1b1b;background:#2b1515}"
            ".btn[disabled]{opacity:.6;cursor:not-allowed}"
            ".grid{display:grid;gap:12px} @media(min-width:900px){.cols-2{grid-template-columns:1fr 1fr}}"
            ".card{background:var(--card);border:1px solid var(--bd);border-radius:14px;padding:16px;box-shadow:0 8px 24px rgba(0,0,0,.25)}"
            "table{width:100%;border-collapse:collapse} th,td{padding:10px;border-bottom:1px solid var(--bd);text-align:left}"
            "input[type=text],input[type=number],input[type=password],select,textarea{width:100%;padding:10px;border-radius:10px;border:1px solid var(--bd);background:var(--bg);color:var(--fg)}"
            "input[type=range]{width:220px}"
            ".badge{display:inline-block;padding:2px 8px;border-radius:10px;border:1px solid var(--bd)}"
            ".ok{background:#0f2a1d;color:#a7f3d0}.warn{background:#3a2a0f;color:#fde68a}.err{background:#3a1313;color:#fecaca}"
            ".muted{color:var(--muted)}"
            "#offlineBanner{display:none;background:var(--err);color:#fff;padding:8px 16px;text-align:center;font-weight:600;border-radius:10px;margin-bottom:12px}"
            "</style></head><body>"
            "<header><div class='wrap row'>"
            "<h1 style='flex:1'>ESP32-S3-ETH Fan Controller</h1>"
            "<nav class='row'>"
            "<a class='btn' href='/'>Start</a>"
            "<a class='btn' href='/fans'>L&uuml;fter</a>"
            "<a class='btn' href='/mqtt'>MQTT</a>"
            "<a class='btn' href='/ota'>Firmware</a>"
            "<a class='btn' href='/diag'>Diag</a>"
            "</nav></div></header><main><div class='wrap'>"));
}
static void uiFooter(EthernetClient &c) { c.print(F("</div></main></body></html>")); }

// ==== Pages ====
// [F1] esc() fuer XSS-Schutz, [F2] data-fan Attribut, [F3] Offline-Banner, [B3] rev nur bei Strukturaenderung, [B4] Reboot-Confirm
static void renderRoot(EthernetClient &c) {
  uiHeader(c, "Start");
  c.print(F(
    "<div id='offlineBanner'>Verbindung zum ESP unterbrochen!</div>"
    "<div class='grid cols-2'>"
    "<section class='card'><h2>Status</h2>"
    "<div id='net' class='muted'></div>"
    "<div id='dev' class='muted'></div>"
    "<div class='spacer' style='height:8px'></div>"
    "<div id='fanshort'></div>"
    "</section>"
    "<section class='card'><h2>Aktionen</h2>"
    "<div class='row'><a class='btn' href='/fans'>L&uuml;fter verwalten</a>"
    "<a class='btn' href='/ota'>Firmware-Update</a>"
    "<a class='btn' href='/reboot' onclick='return confirm(\"ESP jetzt neu starten?\")'>Reboot</a></div>"
    "</section></div>"

    "<section class='card'><h2>L&uuml;fter Live</h2>"
    "<table><thead><tr><th>#</th><th>Name</th><th>PWM-Pin</th><th>Tacho-Pin</th><th>PWM</th><th>RPM</th><th>Status</th><th>Aktion</th></tr></thead>"
    "<tbody id='fanrows'></tbody></table></section>"

    "<script>"
    "function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}"
    "let lastRev=-1,failCount=0;"
    "const pwmCtrl={timers:new Map(),inflight:new Map(),send(i,pct){if(this.timers.has(i))clearTimeout(this.timers.get(i));const to=setTimeout(()=>{const old=this.inflight.get(i);if(old){try{old.abort();}catch(e){}}const ac=new AbortController();this.inflight.set(i,ac);const duty=Math.round((pct/100)*255);fetch('/fans/set?fan='+i+'&duty='+duty,{cache:'no-store',signal:ac.signal}).catch(()=>{}).finally(()=>{this.inflight.delete(i);});},120);this.timers.set(i,to);}};"
    "function badge(f){ if(!f.validated) return `<span class='badge err'>Config invalid (fault ${f.fault})</span>`;"
    " if(f.fault==0) return `<span class='badge ok'>OK</span>`; if(f.fault==3) return `<span class='badge warn'>No tach</span>`; if(f.fault==4) return `<span class='badge warn'>Storm</span>`; return `<span class='badge err'>Fault ${f.fault}</span>`; }"
    "async function load(){ let j=null; try{const r=await fetch('/api/status',{cache:'no-store'}); j=await r.json(); failCount=0; document.getElementById('offlineBanner').style.display='none';}catch(e){return;} lastRev=(j&&typeof j.rev==='number')?j.rev:0;"
    "document.getElementById('net').textContent='IP: '+(j.ip||'-')+'  |  MQTT: '+(j.mqtt_connected?'verbunden':'getrennt')+'  | Boot# '+(j.boot_count||'-')+(j.safe_mode?' (CRASH-LOOP)':'');"
    "document.getElementById('dev').textContent='Device: '+(j.device||'-')+' | Reset: '+(j.reset_reason||'-')+' | minHeap: '+(j.min_free_heap||'-');"
    "const tb=document.getElementById('fanrows'); tb.innerHTML=''; const fans=Array.isArray(j.fans)?j.fans:[]; if(!fans.length){ tb.innerHTML='<tr><td colspan=8 class=\"muted\">Keine L\\u00fcfter konfiguriert.</td></tr>'; }"
    "let count=0; fans.forEach((f)=>{ const i=f.index; const pct=(typeof f.pct==='number')?f.pct:Math.round((f.pwm/255)*100); const tr=document.createElement('tr');"
    "tr.innerHTML="
    "`<td>${(++count)}</td>`+"
    "`<td>${esc(f.name)}</td>`+"
    "`<td>${f.pwmPin}</td>`+"
    "`<td>${f.tachPin}</td>`+"
    "`<td><input type='range' min='0' max='100' value='${pct}' data-fan='${i}' oninput='setPWM(${i},this.value)'/> <span id='p${i}'>${pct}%</span></td>`+"
    "`<td><span id='r${i}'>${f.rpm}</span></td>`+"
    "`<td id='s${i}'>${badge(f)}</td>`+"
    "`<td><a class='btn' href='/fans/edit?fan=${i}'>Bearbeiten</a> <a class='btn' href='/fans/calib?fan=${i}'>Kalibrieren</a></td>`;"
    "tb.appendChild(tr); }); document.getElementById('fanshort').textContent = fans.length + ' L\\u00fcfter konfiguriert'; }"
    "function setPWM(i,val){ const pct=Math.max(0,Math.min(100,Number(val)||0)); const e=document.getElementById('p'+i); if(e) e.textContent=pct+'%'; pwmCtrl.send(i,pct); }"
    "let pollBusy=false; setInterval(async ()=>{ if(pollBusy) return; try{ pollBusy=true; const r=await fetch('/api/status',{cache:'no-store'}); const j=await r.json(); failCount=0; document.getElementById('offlineBanner').style.display='none';"
    "if(j.rev!==undefined && j.rev==lastRev){ (Array.isArray(j.fans)?j.fans:[]).forEach((f)=>{ const rpm=document.getElementById('r'+f.index); if(rpm) rpm.textContent=f.rpm; const s=document.getElementById('s'+f.index); if(s) s.innerHTML=badge(f); const p=document.getElementById('p'+f.index); if(p){ const pct=(typeof f.pct==='number')?f.pct:Math.round((f.pwm/255)*100); p.textContent=pct+'%'; const input=document.querySelector(`input[data-fan='${f.index}']`); if(input && document.activeElement!==input) input.value=pct; } }); } else { lastRev=j.rev; load(); }"
    "}catch(e){ failCount++; if(failCount>=3) document.getElementById('offlineBanner').style.display='block'; } finally{ pollBusy=false; } },1200);"
    "load();"
    "</script>"));
  uiFooter(c);
}

static void renderFanManager(EthernetClient &c) {
  uiHeader(c, "L&uuml;fter");
  c.print(F("<section class='card'>"
            "<div class='row' style='justify-content:space-between'><h2 style='margin:0'>L&uuml;fter</h2>"
            "<a class='btn' href='/fans/new'>+ Neuer L&uuml;fter</a></div><div class='spacer' style='height:8px'></div>"
            "<table><thead><tr><th>#</th><th>Name</th><th>PWM-Pin</th><th>Tacho-Pin</th><th>PWM</th><th>RPM</th><th>Status</th><th>Aktion</th></tr></thead><tbody>"));
  uint16_t counter = 0;
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i)) continue;
    uint8_t pct = pctFromDuty(fans[i].duty);
    c.print(F("<tr><td>")); c.print(++counter);
    c.print(F("</td><td>")); c.print(fans[i].name);
    c.print(F("</td><td>")); c.print(fans[i].pwmPin);
    c.print(F("</td><td>")); c.print(fans[i].tachPin);
    c.print(F("</td><td>")); c.print((int)pct); c.print(F("%"));
    c.print(F("</td><td>")); c.print(fans[i].rpmShown);
    c.print(F("</td><td>")); c.print((fans[i].validated && fans[i].fault == FF_OK) ? "OK" : "FAULT");
    c.print(F("</td><td>"));
    c.print(F("<a class='btn' href='/fans/edit?fan=")); c.print(i); c.print(F("'>Bearbeiten</a> "));
    c.print(F("<a class='btn' href='/fans/calib?fan=")); c.print(i); c.print(F("'>Kalibrieren</a></td></tr>"));
  }
  c.print(F("</tbody></table></section>"));
  uiFooter(c);
}

static void renderFanEdit(EthernetClient &c, int idx) {
  if (idx < 0 || idx >= MAX_FANS) { httpSend400(c, "bad fan index"); return; }
  Fan &f = fans[idx];
  uiHeader(c, "L&uuml;fter bearbeiten");
  c.print(F("<section class='card'><h2>L&uuml;fter bearbeiten</h2><form method='POST' action='/fans/save'>"));
  c.print(F("<input type='hidden' name='fan' value='")); c.print(idx); c.print(F("'>"));
  c.print(F("<label>Name</label><input name='name' maxlength='19' value='")); c.print(f.name); c.print(F("'>"));
  c.print(F("<div class='row'><label><input type='checkbox' name='inv' value='1'")); if (f.invertPwm) c.print(F(" checked")); c.print(F("> invert PWM</label></div>"));

  c.print(F("<label>PWM-Pin</label>"));
  renderPinDropdown(c, "pwm", PWM_ALLOWED, COUNT_OF(PWM_ALLOWED), isPwmAllowed, f.pwmPin, (int8_t)idx);

  c.print(F("<label>Tacho-Pin</label>"));
  renderPinDropdown(c, "tach", TACH_ALLOWED, COUNT_OF(TACH_ALLOWED), isTachAllowed, f.tachPin, (int8_t)idx);

  c.print(F("<div class='spacer' style='height:8px'></div>"
            "<button class='btn' type='submit'>Speichern</button> "
            "<button class='btn danger' name='delete' value='1' onclick='return confirm(\"L\\u00fcfter wirklich l\\u00f6schen?\")'>L&ouml;schen</button> "
            "<a class='btn' href='/fans'>Abbrechen</a>"
            "</form></section>"));
  uiFooter(c);
}

static void renderCalib(EthernetClient &c, int idx) {
  if (idx < 0 || idx >= MAX_FANS) { httpSend400(c, "bad fan index"); return; }
  Fan &f = fans[idx];
  uiHeader(c, "Kalibrierung");
  c.print(F("<section class='card'><h2>Kalibrierung: ")); c.print(f.name); c.print(F("</h2>"
            "<form method='POST' action='/fans/calib/save'>"
            "<input type='hidden' name='fan' value='")); c.print(idx); c.print(F("'>"));
  uint8_t cminPct = pctFromDuty(f.calMinStart);
  c.print(F("<label>Minimaler Start (% 0..100)</label><input type='number' min='0' max='100' name='cmin' value='")); c.print((int)cminPct); c.print(F("'>"));
  c.print(F("<label>Notiz (Charakteristik)</label><textarea name='cnote' rows='3'>")); c.print(f.calNote); c.print(F("</textarea>"
            "<div class='spacer' style='height:8px'></div><button class='btn' type='submit'>Speichern</button> <a class='btn' href='/fans'>Zur&uuml;ck</a>"));

  c.print(F("<div class='spacer' style='height:12px'></div><h3>Live-Test</h3>"
            "<div>Duty: <input id='d' type='range' min='0' max='100' value='0' oninput='sp(this.value)'> <span id='dv'>0%</span></div>"
            "<div style='margin-top:8px'>RPM: <span id='lrpm'>-</span></div>"
            "<script>"
            "const live={timer:null,inflight:null,send(pct){ if(this.timer) clearTimeout(this.timer); this.timer=setTimeout(()=>{ if(this.inflight){try{this.inflight.abort();}catch(e){} } this.inflight=new AbortController(); const duty=Math.round((pct/100)*255); fetch('/fans/set?fan="));
  c.print(idx);
  c.print(F("&duty='+duty,{cache:'no-store',signal:this.inflight.signal}).catch(()=>{}).finally(()=>{this.inflight=null;}); },120);}};"
            "function sp(v){ const pct=Math.max(0,Math.min(100,Number(v)||0)); document.getElementById('dv').textContent=pct+'%'; live.send(pct); }"
            "setInterval(async()=>{ try{ const r=await fetch('/api/status',{cache:'no-store'}); const j=await r.json(); const fans=Array.isArray(j.fans)?j.fans:[]; const f=fans.find(x=>x.index=="));
  c.print(idx);
  c.print(F("); if(f){ document.getElementById('lrpm').textContent=f.rpm+' RPM'; } }catch(e){} },1500);"
            "</script></form></section>"));
  uiFooter(c);
}

static void renderMQTTConfig(EthernetClient &c) {
  uiHeader(c, "MQTT");
  c.print(F("<section class='card'><h2>MQTT Konfiguration</h2>"
            "<form method='POST' action='/mqtt/save'>"
            "<label><input type='checkbox' name='enabled' value='1'")); if (mqttConfig.enabled) c.print(F(" checked")); c.print(F("> MQTT aktiv</label>"
            "<label>Host</label><input type='text' name='host' value='")); c.print(mqttConfig.host); c.print(F("'>"
            "<div class='row'><div style='flex:1'><label>Port</label><input type='number' name='port' value='")); c.print(mqttConfig.port); c.print(F("'></div>"
            "<div style='flex:1'><label>MQTT Pr&auml;fix</label><input type='text' name='prefix' value='")); c.print(mqttConfig.prefix); c.print(F("'></div></div>"
            "<div class='row'><div style='flex:1'><label>User</label><input type='text' name='user' value='")); c.print(mqttConfig.user); c.print(F("'></div>"
            "<div style='flex:1'><label>Pass</label><input type='password' name='pass' value='")); c.print(mqttConfig.pass); c.print(F("'></div></div>"
            "<p class='muted'>Topics:</p>"
            "<pre class='muted' style='white-space:pre-wrap'>"));
  c.print(mqttConfig.prefix); c.print(F("/")); c.print(deviceId);
  c.print(F("/status              (retained \"online\"/\"offline\")\n"));
  c.print(mqttConfig.prefix); c.print(F("/")); c.print(deviceId);
  c.print(F("/fan/&lt;name&gt;/set   (0..100, no retain)\n"));
  c.print(mqttConfig.prefix); c.print(F("/")); c.print(deviceId);
  c.print(F("/fan/&lt;name&gt;/pct   (0..100, retained)\n"));
  c.print(mqttConfig.prefix); c.print(F("/")); c.print(deviceId);
  c.print(F("/fan/&lt;name&gt;/rpm   (RPM, no retain)</pre>"
            "<div class='spacer' style='height:8px'></div><button class='btn' type='submit'>Speichern</button> <a class='btn' href='/'>Zur&uuml;ck</a></form></section>"));
  uiFooter(c);
}

static void renderOTA(EthernetClient &c) {
  uiHeader(c, "Firmware");
  c.print(F(
    "<section class='card'><h2>Firmware-Update (OTA)</h2>"
    "<p class='muted'>Bitte eine <code>.bin</code>-Datei ausw&auml;hlen.</p>"
    "<input id='fw' type='file' accept='.bin'><div class='spacer' style='height:8px'></div>"
    "<div class='row'><button class='btn' id='btn' type='button'>Upload &amp; Flash</button> <a class='btn' href='/'>Abbrechen</a></div>"
    "<div class='spacer' style='height:8px'></div>"
    "<div id='box' style='display:none'>"
    "<progress id='pr' max='100' value='0' style='width:100%'></progress>"
    "<div class='spacer' style='height:6px'></div>"
    "<div id='st' class='muted'>Bereit&hellip;</div>"
    "</div>"
    "<pre id='log' style='white-space:pre-wrap;background:#0b0f14;border:1px solid var(--bd);border-radius:10px;padding:10px;margin-top:12px;max-height:40vh;overflow:auto'></pre>"
    "</section>"
    "<script>"
    "function byId(x){return document.getElementById(x)};"
    "function log(msg){const el=byId('log'); el.textContent+=msg+'\\n'; el.scrollTop=el.scrollHeight;}"
    "async function waitForReboot(){ byId('st').textContent='Rebooting\\u2026 warte auf Ger\\u00e4t'; let ok=false; const t0=Date.now();"
    "while(!ok && Date.now()-t0<120000){ try{ const r=await fetch('/api/status?ts='+Date.now(),{cache:'no-store'}); if(r.ok){ const j=await r.json(); byId('st').textContent='Online: Boot# '+j.boot_count+' | IP '+j.ip; ok=true; break; } }catch(e){} await new Promise(r=>setTimeout(r,1500)); }"
    "if(!ok){ byId('st').textContent='Ger\\u00e4t antwortet nicht.'; } }"
    "byId('btn').addEventListener('click', async ()=>{ const f=byId('fw').files[0]; if(!f){ log('Bitte Datei ausw\\u00e4hlen.'); return; }"
    "byId('btn').disabled=true; byId('fw').disabled=true; byId('box').style.display='block';"
    "const xhr=new XMLHttpRequest(); xhr.open('POST','/ota',true); xhr.setRequestHeader('Content-Type','application/octet-stream');"
    "xhr.upload.onprogress=(e)=>{ if(e.lengthComputable){ const p=Math.round((e.loaded/e.total)*100); byId('pr').value=p; byId('st').textContent='Upload: '+p+'%'; } };"
    "xhr.onerror=()=>{ log('Netzwerkfehler.'); byId('st').textContent='Fehler'; byId('btn').disabled=false; byId('fw').disabled=false; };"
    "xhr.onload=()=>{ try{ const j=JSON.parse(xhr.responseText||'{}'); if(j.ok){ log(j.message||'Update erfolgreich.'); const s=(j.reboot_in||5); byId('st').innerHTML='Flash OK \\u2013 Reboot in <b id=cd>'+s+'</b>s'; let t=s; const iv=setInterval(()=>{t--; if(t<=0){clearInterval(iv);} const cd=byId('cd'); if(cd) cd.textContent=t;},1000); waitForReboot(); } else { log(j.error||'Fehler'); byId('st').textContent='Fehler'; byId('btn').disabled=false; byId('fw').disabled=false; } }catch(e){ log('Antwort unlesbar.'); byId('btn').disabled=false; byId('fw').disabled=false; } };"
    "xhr.send(f);"
    "});"
    "</script>"));
  uiFooter(c);
}

static void renderDiag(EthernetClient &c) {
  uiHeader(c, "Diagnose");
  c.print(F(
    "<section class='card'>"
    "<h2>Live-Status</h2>"
    "<div id='kv' class='grid' style='grid-template-columns: max-content 1fr; gap:8px'></div>"
    "<div class='row' style='margin-top:8px'><button class='btn' onclick='refresh()'>Jetzt aktualisieren</button>"
    "<span id='ts' class='muted' style='margin-left:8px'>&#x2013;</span></div>"
    "<small class='muted'>JSON: <a href='/api/status' target='_blank'>/api/status</a></small>"
    "</section>"

    "<section class='card'><h2>Log (aktueller Boot)</h2>"
    "<pre id='log' style='white-space:pre-wrap;background:#0b0f14;border:1px solid var(--bd);border-radius:10px;padding:10px;max-height:40vh;overflow:auto'>Lade...</pre>"
    "<div class='row'><a class='btn' href='/log.txt' target='_blank'>Log herunterladen</a></div>"
    "</section>"

    "<section class='card'><h2>Vorheriger Boot Log-Tail</h2>"
    "<pre id='prev' style='white-space:pre-wrap;background:#0b0f14;border:1px solid var(--bd);border-radius:10px;padding:10px;max-height:30vh;overflow:auto'>Lade...</pre>"
    "</section>"

    "<script>"
    "function esc(s){return (s||'').toString().replace(/[&<>]/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[m]))}"
    "function pill(v,cls){return `<span class='badge ${cls}'>${v}</span>`}"
    "function setKV(j){const kv=document.getElementById('kv'); let h='';"
    "function row(k,v){h+=`<div style=\\\"font-weight:600\\\">${k}</div><div>${v}</div>`}"
    "row('Device', esc(j.device));"
    "row('IP', esc(j.ip));"
    "row('Reset', esc(j.reset_reason));"
    "row('Boot-Count', j.boot_count);"
    "row('Crash-Loop', j.safe_mode ? pill('erkannt','warn') : pill('nein','ok'));"
    "row('MQTT', j.mqtt_connected ? pill('verbunden','ok') : pill('getrennt','err'));"
    "row('min Free Heap', j.min_free_heap);"
    "let cnt=(j.fans||[]).length; row('L\\u00fcfter', cnt);"
    "if(Array.isArray(j.fans)){ j.fans.forEach(f=>{ const pct=(typeof f.pct==='number')?f.pct:Math.round((f.pwm/255)*100); row('Fan #'+(f.index+1), `${esc(f.name)} \\u2014 PWM ${f.pwmPin}, Tach ${f.tachPin}, Duty ${pct}%, RPM ${f.rpm}, Fault ${f.fault}, Valid ${f.validated}`);}); }"
    "kv.innerHTML=h;"
    "}"
    "async function refresh(){ try{const r1=await fetch('/api/status',{cache:'no-store'}); const j=await r1.json(); setKV(j); const r2=await fetch('/log.txt',{cache:'no-store'}); const t=await r2.text(); document.getElementById('log').textContent=t.trim().split('\\n').slice(-500).join('\\n'); const r3=await fetch('/prevlog.txt',{cache:'no-store'}); const p=await r3.text(); document.getElementById('prev').textContent=p.trim(); document.getElementById('ts').textContent='Aktualisiert: '+(new Date()).toLocaleTimeString(); }catch(e){document.getElementById('ts').textContent='Fehler: '+e.message} }"
    "setInterval(refresh,2000); refresh();"
    "</script>"));
  uiFooter(c);
}

static void handleLogTxt(EthernetClient &c)     { httpSendHeaderOK(c, "text/plain; charset=UTF-8"); c.print(gLogBuf); }
static void handlePrevLogTxt(EthernetClient &c)  { httpSendHeaderOK(c, "text/plain; charset=UTF-8"); c.print(gPrevLogTail); }

// ==== JSON Status ====
static void sendJsonStatus(EthernetClient &c) {
  httpSendHeaderOK(c, "application/json");
  c.print(F("{\"rev\":")); c.print(g_stateRev);
  c.print(F(",\"device\":")); jsonPrintEscaped(c, deviceId.c_str());
  c.print(F(",\"ip\":")); jsonPrintEscaped(c, Ethernet.localIP().toString().c_str());
  c.print(F(",\"mqtt_connected\":")); c.print(mqtt.connected() ? "true" : "false");
  c.print(F(",\"boot_count\":")); c.print(g_bootCount);
  c.print(F(",\"safe_mode\":")); c.print(g_crashLoopDetected ? "true" : "false");
  c.print(F(",\"reset_reason\":")); jsonPrintEscaped(c, g_resetReasonStr.c_str());
  c.print(F(",\"min_free_heap\":")); c.print(g_minFreeHeap);
  c.print(F(",\"fans\":["));
  bool first = true;
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    if (!fanPresentIdx(i)) continue;
    if (!first) c.print(','); first = false;
    c.print(F("{\"index\":")); c.print(i);
    c.print(F(",\"name\":")); jsonPrintEscaped(c, fans[i].name);
    c.print(F(",\"pwm\":")); c.print(fans[i].duty);
    c.print(F(",\"pct\":")); c.print((int)pctFromDuty(fans[i].duty));
    c.print(F(",\"rpm\":")); c.print(fans[i].rpmShown);
    c.print(F(",\"pwmPin\":")); c.print(fans[i].pwmPin);
    c.print(F(",\"tachPin\":")); c.print(fans[i].tachPin);
    c.print(F(",\"fault\":")); c.print((int)fans[i].fault);
    c.print(F(",\"validated\":")); c.print(fans[i].validated ? "true" : "false");
    c.print(F("}"));
  }
  c.print(F("]}"));
}

// Geordneter Abgang vor jedem Restart: ioBroker sieht sofort "offline",
// Log-Tail landet im NVS (zusaetzlich zum Shutdown-Handler).
static void prepareRestart() {
  if (mqtt.connected()) {
    String t = topicDev() + "/status";
    mqtt.publish(t.c_str(), "offline", true);
    mqtt.disconnect();
  }
  persistLogTail();
}

// ==== OTA ====
static bool handleOTA(EthernetClient &c, size_t contentLength) {
  if (contentLength == 0) {
    httpSendHeaderOK(c, "application/json; charset=UTF-8");
    c.print(F("{\"ok\":false,\"error\":\"No Content or chunked unsupported\"}"));
    LOGE("OTA", "no content/chunked");
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
      feedLoopWDT();
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
    } else { feedLoopWDT(); delay(1); }
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

// ==== Save-Handler ====
static void handleMQTTConfigSave(EthernetClient &c, const String &body) {
  String s;
  mqttConfig.enabled = body.indexOf(F("enabled=1")) >= 0;
  if (formGet(body, F("host"),   s)) s.toCharArray(mqttConfig.host,   sizeof(mqttConfig.host));
  if (formGet(body, F("port"),   s)) mqttConfig.port = (uint16_t)constrain(s.toInt(), 1, 65535);
  if (formGet(body, F("user"),   s)) s.toCharArray(mqttConfig.user,   sizeof(mqttConfig.user));
  if (formGet(body, F("pass"),   s)) s.toCharArray(mqttConfig.pass,   sizeof(mqttConfig.pass));
  if (formGet(body, F("prefix"), s)) s.toCharArray(mqttConfig.prefix, sizeof(mqttConfig.prefix));

  Preferences p; p.begin("mqtt", false);
  p.putBool("enabled", mqttConfig.enabled);
  p.putString("host",   mqttConfig.host);
  p.putUShort("port",   mqttConfig.port);
  p.putString("user",   mqttConfig.user);
  p.putString("pass",   mqttConfig.pass);
  p.putString("prefix", mqttConfig.prefix);
  p.end();

  LOGI("MQTT", "config saved");
  httpSendRedirect(c, "/mqtt");
}

static void handleFanSave(EthernetClient &c, const String &body) {
  String s;
  if (!formGet(body, F("fan"), s)) { httpSend400(c, "missing fan"); return; }
  int idx = s.toInt();
  if (idx < 0 || idx >= MAX_FANS) { httpSend400(c, "bad fan index"); return; }

  Fan &f = fans[idx];

  if (body.indexOf(F("delete=1")) >= 0) {
    ApplyJob j; j.idx = (uint8_t)idx; j.deleteFan = true;
    applyQueue((uint8_t)idx, j);
    httpSendRedirect(c, "/fans");
    return;
  }

  String newName = String(f.name);
  if (formGet(body, F("name"), s)) newName = s;
  bool inv = (body.indexOf(F("inv=1")) >= 0);
  uint8_t newPwm = f.pwmPin, newTac = f.tachPin;
  if (formGet(body, F("pwm"),  s)) newPwm = (uint8_t)constrain(s.toInt(), 0, 255);
  if (formGet(body, F("tach"), s)) newTac = (uint8_t)constrain(s.toInt(), 0, 255);

  if (newName.length() == 0)    { httpSend400(c, "name required"); return; }
  if ((int)newName.length() > 19) { httpSend400(c, "name too long"); return; }
  if (newPwm != f.pwmPin && !validPwmForFan(newPwm,  (int8_t)idx)) { httpSend400(c, "pwm pin invalid/busy"); return; }
  if (newTac != f.tachPin && !validTachForFan(newTac, (int8_t)idx)) { httpSend400(c, "tach pin invalid/busy"); return; }

  ApplyJob j; j.idx = (uint8_t)idx;
  if (sanitizeName(String(f.name)) != sanitizeName(newName)) { safeStrcpy(j.name, sizeof(j.name), newName); j.nameChanged = true; }
  if (inv != f.invertPwm) { j.invert = inv; j.invChanged = true; }
  if (newPwm != f.pwmPin || newTac != f.tachPin) { j.pwmPin = newPwm; j.tachPin = newTac; j.pinsChanged = true; }

  if (!j.nameChanged && !j.invChanged && !j.pinsChanged) { httpSendRedirect(c, "/fans"); return; }
  applyQueue((uint8_t)idx, j);
  httpSendRedirect(c, "/fans");
}

static void handleCalibSave(EthernetClient &c, const String &body) {
  String s;
  if (!formGet(body, F("fan"), s)) { httpSend400(c, "missing fan"); return; }
  int idx = s.toInt();
  if (idx < 0 || idx >= MAX_FANS) { httpSend400(c, "bad fan index"); return; }
  Fan &f = fans[idx];

  if (formGet(body, F("cmin"), s)) {
    int pct = constrain(s.toInt(), 0, 100);
    f.calMinStart = dutyFromPct((uint8_t)pct);
  }
  if (formGet(body, F("cnote"), s)) { safeStrcpy(f.calNote, sizeof(f.calNote), s); }

  Preferences p; p.begin("fans", false);
  String k = "f" + String(idx) + "_";
  p.putUChar((k + "cmin").c_str(), f.calMinStart);
  p.putString((k + "cnote").c_str(), f.calNote);
  p.end();

  httpSendRedirect(c, "/fans");
}

// ==== Router ====
static void handleClient(EthernetClient &c) {
  String rl;
  if (!readLine(c, rl, CLIENT_RD_TIMEOUT)) return;
  String method = rl.substring(0, rl.indexOf(' '));
  String pathQuery = rl.substring(rl.indexOf(' ') + 1, rl.lastIndexOf(' '));
  String path = pathQuery, query = "";
  int q = pathQuery.indexOf('?');
  if (q >= 0) { path = pathQuery.substring(0, q); query = pathQuery.substring(q + 1); }

  size_t contentLength = 0;
  String contentType;
  readHeaders(c, contentLength, contentType);
  LOGI("HTTP", method + " " + path + (query.length() ? "?" + query : ""));

  // --- GET ---
  if (method == "GET") {
    if (path == "/")            { renderRoot(c); return; }
    if (path == "/fans")        { renderFanManager(c); return; }
    if (path == "/mqtt")        { renderMQTTConfig(c); return; }
    if (path == "/ota")         { renderOTA(c); return; }
    if (path == "/diag")        { renderDiag(c); return; }
    if (path == "/log.txt")     { handleLogTxt(c); return; }
    if (path == "/prevlog.txt") { handlePrevLogTxt(c); return; }
    if (path == "/api/status")  { sendJsonStatus(c); return; }

    if (path == "/fans/edit") {
      long idx = -1; if (parseQueryParam(query, "fan", idx)) { renderFanEdit(c, (int)idx); return; }
      httpSend400(c, "missing fan idx"); return;
    }
    if (path == "/fans/calib") {
      long idx = -1; if (parseQueryParam(query, "fan", idx)) { renderCalib(c, (int)idx); return; }
      httpSend400(c, "missing fan idx"); return;
    }
    // [B8] Slot-Suche: pwmPin==0xFF statt fanPresentIdx, verhindert Slot-Leak bei Abbruch
    if (path == "/fans/new") {
      int idx = -1;
      for (uint8_t i = 0; i < MAX_FANS; i++) if (fans[i].pwmPin == 0xFF) { idx = i; break; }
      if (idx < 0) { httpSend400(c, "no free slot"); return; }
      fanInitDefaults((uint8_t)idx);
      clearFanNVS((uint8_t)idx);
      safeStrcpy(fans[idx].name, sizeof(fans[idx].name), String("New Fan ") + String(idx + 1));
      httpSendRedirect(c, (String("/fans/edit?fan=") + idx).c_str());
      return;
    }
    if (path == "/reboot") {
      httpSendHeaderOK(c, "text/plain; charset=UTF-8");
      c.print(F("Rebooting...")); c.flush(); prepareRestart(); delay(200); ESP.restart(); return;
    }
    if (path == "/fans/set") {
      long idx = -1, duty = -1;
      if (parseQueryParam(query, "fan", idx) && parseQueryParam(query, "duty", duty)) {
        if (idx >= 0 && idx < MAX_FANS && fanPresentIdx((uint8_t)idx)) {
          dutyEnqueue((uint8_t)idx, (uint8_t)constrain(duty, 0, 255));
          httpSendHeaderOK(c, "text/plain; charset=UTF-8");
          c.print(F("OK"));
          return;
        }
      }
      httpSend400(c, "Invalid parameters"); return;
    }
  }

  // --- POST ---
  if (method == "POST") {
    if (path == "/mqtt/save")        { handleFormPost(c, contentLength, contentType, handleMQTTConfigSave); return; }
    if (path == "/fans/save")        { handleFormPost(c, contentLength, contentType, handleFanSave); return; }
    if (path == "/fans/calib/save")  { handleFormPost(c, contentLength, contentType, handleCalibSave); return; }
    if (path == "/ota") {
      if (contentType.startsWith(F("application/octet-stream"))) (void)handleOTA(c, contentLength);
      else httpSend400(c, "Use application/octet-stream");
      return;
    }
  }

  httpSend400(c, "Not found");
}

// ==== Ethernet State ====
static unsigned long lastDhcpTryMs = 0;
static uint32_t g_linkLostSince = 0, g_lastEthReinit = 0;
static uint32_t g_lastEthMaintainMs = 0;

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

// ==== setup() ====
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("== ESP32-S3-ETH Fan Controller v4.0 =="));

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
      g_otaPendingVerify = true;
      LOGW("OTA", "Image PENDING_VERIFY - Health-Window 90s laeuft");
    }
  }

  loadPrevLogTail();
  bootTrackInit();

  disableRadios();
  safetyZeroPins();

  loadConfigFans();
  loadMQTTConfig();
  mqtt.setSocketTimeout(3);  // CONNACK-Busy-Wait < WDT-Budget (Spec §3.1)
  buildMacAndDeviceId();
  dutyPendingInit();

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

  // [B7] W5500 Ethernet — resetW5500() macht SPI.begin + Ethernet.init
  resetW5500();

  if (Ethernet.begin(g_mac, 4000, 1200)) {
    ethHasIP = true;
    httpServer.begin();
    httpUp = true;
    LOGI("NET", String("W5500 OK - IP: ") + Ethernet.localIP().toString());
  } else {
    ethHasIP = false;
    LOGW("NET", "DHCP failed - retry in loop()");
  }

  LOGI("BOOT", "Setup complete.");
}

// ==== loop() ====
void loop() {
  static uint32_t lastMeasureTick = 0;
  const uint32_t now = millis();

  // [Spec §3.2] Health-Window: 90s gesund gelaufen => Image valid, Rollback abgewaehlt.
  if (g_otaPendingVerify && now > OTA_HEALTH_MS) {
    g_otaPendingVerify = false;
    esp_ota_mark_app_valid_cancel_rollback();
    LOGI("OTA", "Image nach 90s als VALID markiert");
  }

  uint32_t hf = ESP.getFreeHeap();
  if (hf < g_minFreeHeap) g_minFreeHeap = hf;

  // --- Apply-Queue ---
  if (g_anyApplyPending) {
    bool stillPending = false;
    for (uint8_t i = 0; i < MAX_FANS; i++) {
      if (g_apply[i].active) {
        applyDo(i);
        if (g_apply[i].active) stillPending = true;
      }
    }
    g_anyApplyPending = stillPending;
  }

  // --- Duty-Queue ---
  dutyProcessQueue();

  // --- Ethernet/DHCP/Link-Watch ---
  if (!ethHasIP) {
    if (Ethernet.linkStatus() == LinkON && (now - lastDhcpTryMs) > DHCP_RETRY_MS) {
      lastDhcpTryMs = now;
      if (Ethernet.begin(g_mac, 4000, 1200)) {
        ethHasIP = true;
        if (!httpUp) { httpServer.begin(); httpUp = true; }
        LOGI("NET", String("IP: ") + Ethernet.localIP().toString());
      } else {
        LOGW("NET", "DHCP retry failed");
      }
    }
  } else {
    if (now - g_lastEthMaintainMs >= ETH_MAINTAIN_INTERVAL_MS) {
      g_lastEthMaintainMs = now;
      Ethernet.maintain();
    }
    if (Ethernet.linkStatus() != LinkON) {
      if (g_linkLostSince == 0) g_linkLostSince = now;
      if ((now - g_linkLostSince) > ETH_LINK_LOST_RESET_MS && (now - g_lastEthReinit) > ETH_REINIT_COOLDOWN_MS) {
        LOGW("NET", "Link down too long -> W5500 reset");
        ethHasIP = false;
        mqttWasConnected = false;
        g_lastEthReinit = now;
        resetW5500();
      }
    } else {
      g_linkLostSince = 0;
    }
  }

  // --- HTTP Server ---
  if (httpUp && ethHasIP) {
    EthernetClient c = httpServer.available();
    if (c) {
      unsigned long t0 = now;
      while (c.connected() && !c.available() && !elapsed(millis(), t0, 400)) { feedLoopWDT(); delay(1); }
      if (!c.available()) c.stop();
      else { handleClient(c); delay(1); c.stop(); }
    }
  }

  // --- MQTT ---
  if (mqttConfig.enabled && ethHasIP && !g_crashLoopDetected) {
    mqttEnsureConnected();
    if (mqtt.connected()) mqtt.loop();
  }

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

      mqttPublishRPM(i, false);
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

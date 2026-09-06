#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "esp_system.h"
#include "concurrency.h"

// Logger fuer beide Cores.
//
// §E3: Bis v5.4.3 deklarierte net_eth.h die Funktion logFmt() vorwaerts und erwartete ihre
// Definition aus dem Hauptsketch — das Netz-Modul haing also am Sketch statt umgekehrt.
// Jetzt binden beide diesen Header ein und die Abhaengigkeit zeigt in die richtige Richtung.
//
// §A3: Der Ringpuffer ist STATISCHER Speicher, kein Arduino-String. Der String reallozierte
// bei jeder Zeile und memmove'te beim Ueberlauf — auf einem Geraet, das monatelang laeuft,
// der wahrscheinlichste Heap-Fragmentierer.

// Grenzen des Log-Puffers (Spec §3.5: NVS-Wear).
static const size_t   LOG_MAX      = 8192;
static const size_t   LOG_LINE_MAX = 160;    // §F3: war 128 — der Praefix belegt ~28
static const size_t   LOG_NVS_MAX  = 1600;

// Boot-Zaehler steht im Log-Praefix jeder Zeile.
static uint32_t g_bootCount = 0;

// Die Queue-Zeile und der Formatpuffer muessen gleich gross sein — sonst wuerden Log-Zeilen
// beim Einreihen ein zweites Mal still gekappt. Ein Kommentar allein haelt das nicht.
static_assert(sizeof(((LogLine *)nullptr)->text) == LOG_LINE_MAX,
              "LogLine::text (concurrency.h) und LOG_LINE_MAX muessen uebereinstimmen");

// Der Ringpuffer selbst. gLogSnap ist der Kopier-Zwischenspeicher: so wird der Lock NIE
// ueber Socket-I/O oder NVS gehalten (eigene Disziplin, §4.3).
static char   gLogBuf[LOG_MAX + 1] = {0};
static size_t gLogLen = 0;
static char   gLogSnap[LOG_MAX + 1];

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
// §B1: statisch — xSemaphoreCreateMutex() kann fehlschlagen, und logLock()/fansLock() sind
// null-tolerant. Ein Fehlschlag haette den Schutz also LAUTLOS abgeschaltet. Statisch angelegt
// gibt es diesen Zustand nicht.
static StaticSemaphore_t logMutexBuf;
static SemaphoreHandle_t logMutex = nullptr;
static inline void logLock()   { if (logMutex) xSemaphoreTake(logMutex, portMAX_DELAY); }
static inline void logUnlock() { if (logMutex) xSemaphoreGive(logMutex); }

// Consumer-Seite: haengt eine Zeile an gLogBuf (mit Ringkuerzung). NUR via logDrain (ein Task).
static void logLine(const char *s) {
  size_t slen = strlen(s);
  if (slen + 1 > LOG_MAX) slen = LOG_MAX - 1;   // pathologisch lange Zeile: kappen statt ueberlaufen
  logLock();
  if (gLogLen + slen + 1 > LOG_MAX) {
    // Vorne blockweise Platz schaffen (256 B Reserve), damit nicht jede Zeile ein memmove ausloest.
    size_t cut = (gLogLen + slen + 1) - LOG_MAX + 256;
    if (cut >= gLogLen) { gLogLen = 0; }
    else { memmove(gLogBuf, gLogBuf + cut, gLogLen - cut); gLogLen -= cut; }
  }
  memcpy(gLogBuf + gLogLen, s, slen); gLogLen += slen;
  gLogBuf[gLogLen++] = '\n';
  gLogBuf[gLogLen] = 0;
  logUnlock();
}

// Snapshot unter Lock in den statischen Zwischenspeicher; gibt die Laenge zurueck.
// Danach ist der Lock frei — Senden/Schreiben passiert NIE unter Lock.
static size_t logSnapshot() {
  logLock();
  size_t n = gLogLen;
  memcpy(gLogSnap, gLogBuf, n);
  gLogSnap[n] = 0;
  logUnlock();
  return n;
}

// Producer-Seite (beide Cores): sofort auf Serial (Debug) + in die Log-Queue (gLogBuf via Consumer).
// Lokaler Puffer statt geteiltem Static -> thread-safe ohne Lock im Hot-Path.
static void logFmt(char level, const char *tag, const char *msg) {
  char line[LOG_LINE_MAX];
  uint32_t ms = millis();
  // §F3: snprintf kappt still. Wir merken die gewuenschte Laenge und markieren ein
  // Abschneiden sichtbar mit "~", damit eine gekuerzte Zeile nicht als vollstaendig gilt.
  int want = snprintf(line, sizeof(line),
           "[T+%04lu.%03lus #%lu] [%c] %s: %s",
           (unsigned long)(ms / 1000UL), (unsigned long)(ms % 1000UL),
           (unsigned long)g_bootCount, level, tag, msg);
  if (want >= (int)sizeof(line)) line[sizeof(line) - 2] = '~';
  Serial.println(line);
  logPost(line);
}
// §A2: Overload fuer Arduino-String. Die LOG*-Makros uebergeben ihr Argument jetzt
// unveraendert — Aufrufe mit reinem Literal (26 von 60) landen direkt beim const-char*-
// Overload und sparen die Heap-Allokation, die das alte (String(m)).c_str() erzwang.
static void logFmt(char level, const char *tag, const String &msg) {
  logFmt(level, tag, msg.c_str());
}

// Leert die Log-Queue in gLogBuf. NUR aus EINEM Task (Loop bis Task 8, dann networkTask).
static void logDrain() {
  if (!g_logQ) return;
  LogLine l;
  while (xQueueReceive(g_logQ, &l, 0) == pdTRUE) logLine(l.text);
}

#define LOGI(t, m) logFmt('I', t, (m))
#define LOGW(t, m) logFmt('W', t, (m))
#define LOGE(t, m) logFmt('E', t, (m))


#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ==== Stufe 3: Cross-Core-Kanaele (FreeRTOS-Queues statt volatile-Flags) ====
// Message-Passing ersetzt geteilten Speicher: keine Cross-Core-Barriere noetig, nie blockierend.

// Snapshot eines Luefter-Ereignisses, Core 1 (Control) -> Core 0 (Netz/MQTT).
// Werte liegen IM Sample; Core 0 liest fans[] nur fuer den Topic-Namen (topicFan, unter fansMutex).
enum TelemKind : uint8_t { TELEM_SPEED = 0, TELEM_RPM = 1 };
struct TelemetrySample {
  uint8_t  idx;
  uint8_t  kind;   // TelemKind
  uint8_t  duty;   // gueltig bei TELEM_SPEED
  uint16_t rpm;    // gueltig bei TELEM_RPM
  uint8_t  fault;
};

// Befehl Core 0 (Netz/MQTT) -> Core 1 (Control): setze Duty fuer idx.
struct DutyCmd { uint8_t idx; uint8_t duty; };

static QueueHandle_t g_telemQ = nullptr;   // TelemetrySample, Core1 -> Core0
static QueueHandle_t g_dutyQ  = nullptr;   // DutyCmd,         Core0 -> Core1

// Tiefen konservativ: 8 Luefter * wenige offene Events. Drop bei Voll ist tolerierbar
// (RPM hat Keepalive; naechstes Sample ueberschreibt die Info) -> nie blockieren = WDT-sicher.
#define TELEM_Q_DEPTH 24
#define DUTY_Q_DEPTH  16

static inline void concurrencyInit() {
  g_telemQ = xQueueCreate(TELEM_Q_DEPTH, sizeof(TelemetrySample));
  g_dutyQ  = xQueueCreate(DUTY_Q_DEPTH,  sizeof(DutyCmd));
}
// Nicht-blockierend einreihen (Timeout 0): im Fehlerfall lieber droppen als den Core stallen.
static inline void telemPost(const TelemetrySample &s) { if (g_telemQ) xQueueSend(g_telemQ, &s, 0); }
static inline void dutyPost(const DutyCmd &d)          { if (g_dutyQ)  xQueueSend(g_dutyQ,  &d, 0); }

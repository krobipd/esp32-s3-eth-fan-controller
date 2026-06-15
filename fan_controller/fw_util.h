#pragma once
#include <stdint.h>
#include <string.h>
#include <string>

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
  if (strcmp(s, "status") == 0 || strcmp(s, "sys") == 0 || strcmp(s, "info") == 0) return false;  // Geraete-Namespaces
  return true;
}

// §18: RAM-only Pending-Cleanup-Liste fuer verwaiste retained MQTT-Topics.
// Wird ein Luefter geloescht/umbenannt waehrend MQTT getrennt ist, laesst sich der alte
// retained Topic (…/<name>/speed) nicht sofort raeumen -> sanitized Namen hier vormerken;
// onMqttConnect raeumt sie nach (leere retained Payload + unsubscribe). RAM-only bewusst:
// der reboot-vor-reconnect-Edge ist kosmetisch und rechtfertigt kein NVS (Wear-Historie).
#define CLEANUP_MAX 8
struct PendingCleanup {
  char    names[CLEANUP_MAX][20];
  uint8_t count;
};
// Sanitized Namen vormerken. Dedup; leerer Name abgelehnt; bei voller Liste verworfen
// (Rueckgabe false = degradiert auf altes Verhalten fuer diesen einen Topic).
static inline bool pendingCleanupAdd(PendingCleanup &pc, const char *name) {
  if (!name || name[0] == 0) return false;
  for (uint8_t i = 0; i < pc.count; i++)
    if (strcmp(pc.names[i], name) == 0) return true;   // schon vorgemerkt -> ok
  if (pc.count >= CLEANUP_MAX) return false;           // voll -> drop
  size_t n = strlen(name); if (n > 19) n = 19;
  memcpy(pc.names[pc.count], name, n); pc.names[pc.count][n] = 0;
  pc.count++;
  return true;
}
static inline void pendingCleanupClear(PendingCleanup &pc) { pc.count = 0; }

// §5.5 Home-Assistant-MQTT-Discovery — Config-JSON (pure, host-getestet).
// Entitaeten: number (Sollwert 0-100 %) + sensor (RPM). Geteilter device-Block gruppiert alle
// Entitaeten eines Geraets. Abgekuerzte HA-Keys (uniq_id/cmd_t/stat_t/avty_t/dev) = kleinere Payloads.
static inline std::string haDeviceBlock(const std::string &dev, const std::string &fwver) {
  return "\"dev\":{\"ids\":[\"" + dev + "\"],\"name\":\"Fan Controller " + dev +
         "\",\"mf\":\"DIY (Waveshare ESP32-S3-ETH)\",\"mdl\":\"ESP32-S3-ETH Fan Controller\",\"sw\":\"" + fwver + "\"}";
}
// number: Sollwert. min:0 EXPLIZIT (HA-Default waere 1). avty_t = info/status (online/offline).
static inline std::string haNumberConfig(const std::string &prefix, const std::string &dev,
                                         const std::string &fan, const std::string &fwver) {
  std::string base = prefix + "/" + dev + "/" + fan;
  return "{\"name\":\"" + fan + "\",\"uniq_id\":\"" + dev + "_" + fan + "_set\","
         "\"cmd_t\":\"" + base + "/set\",\"stat_t\":\"" + base + "/speed\","
         "\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\",\"unit_of_meas\":\"%\","
         "\"avty_t\":\"" + prefix + "/" + dev + "/info/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         + haDeviceBlock(dev, fwver) + "}";
}
// sensor: Drehzahl (RPM). Eigene unique_id (Pflicht).
static inline std::string haSensorConfig(const std::string &prefix, const std::string &dev,
                                         const std::string &fan, const std::string &fwver) {
  std::string base = prefix + "/" + dev + "/" + fan;
  return "{\"name\":\"" + fan + " RPM\",\"uniq_id\":\"" + dev + "_" + fan + "_rpm\","
         "\"stat_t\":\"" + base + "/rpm\",\"unit_of_meas\":\"RPM\","
         "\"avty_t\":\"" + prefix + "/" + dev + "/info/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
         + haDeviceBlock(dev, fwver) + "}";
}

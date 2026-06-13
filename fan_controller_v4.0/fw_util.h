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

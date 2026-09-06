#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Reine Logik ohne Arduino- und ohne Hardware-Abhaengigkeit — deshalb host-testbar
// (tests/host/test_logic.cpp). Bis v5.4.3 lagen diese Funktionen im Sketch und liefen
// durch keinen einzigen Test, obwohl darunter der CSRF-Schutz und der komplette
// Request-Parser sind: durch sie geht JEDER POST.
//
// Die Signaturen arbeiten auf char-Puffern statt auf Arduino-Strings. Das ist nicht nur
// testbar, sondern spart im Request-Pfad auch die Heap-Allokation je Feld.

// ---------------------------------------------------------------- Pins
// Gesperrt: W5500 (9-14), USB CDC (19-20), Boot/JTAG (0,3), Strapping (43-46).
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

// ---------------------------------------------------------------- Drehzahl
// Erwartete kuerzeste Impulsdauer bei gegebenem Duty — Grundlage des ISR-Glitchfilters.
static inline uint32_t computeMinPulseUs(uint8_t duty, uint16_t maxRpm, uint8_t pulsesPerRev,
                                         uint32_t floorUs) {
  uint32_t rpmExp = (uint32_t)maxRpm * duty / 255;
  if (rpmExp < 1) rpmExp = 1;
  uint32_t periodUs = (60UL * 1000000UL) / (rpmExp * (pulsesPerRev ? pulsesPerRev : 1));
  uint32_t drittel = periodUs / 3;
  return drittel > floorUs ? drittel : floorUs;
}

// §G: Drehzahl aus Impulsen und der TATSAECHLICH vergangenen Zeitspanne.
// Frueher stand hier das nominale Intervall, obwohl die echte Spanne immer >= Intervall ist —
// der Fehler ging systematisch nach oben. Der Zwischenwert ist gegen Ueberlauf abgesichert:
// bei pulses > 71582 wuerde pulses*60000 den uint32 sprengen, deshalb die Schranke.
static inline uint32_t rpmFromPulses(uint32_t pulses, uint32_t spanMs, uint8_t pulsesPerRev) {
  if (spanMs == 0) return 0;
  if (pulsesPerRev == 0) pulsesPerRev = 1;
  if (pulses > 71582UL) pulses = 71582UL;          // Ueberlaufschranke fuer pulses * 60000
  return (pulses * 60000UL) / (spanMs * pulsesPerRev);
}

// Median dreier Messwerte — daempft einzelne Ausreisser, ohne zu verzoegern.
static inline uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
  uint16_t lo_ab = a < b ? a : b, hi_ab = a < b ? b : a;
  uint16_t hi_lo = hi_ab < c ? hi_ab : c;
  return lo_ab > hi_lo ? lo_ab : hi_lo;
}

// ---------------------------------------------------------------- URL/Form-Parser
static inline char fromHex(char c) {
  if (c >= '0' && c <= '9') return (char)(c - '0');
  if (c >= 'a' && c <= 'f') return (char)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (char)(c - 'A' + 10);
  return 0;
}

// '+' -> Leerzeichen, %XX -> Byte. Eine unvollstaendige %-Sequenz am Ende bleibt woertlich
// stehen (Verhalten von v5.4.3, bewusst beibehalten). Gibt die geschriebene Laenge zurueck;
// der Puffer ist immer nullterminiert.
static inline size_t urlDecodeInto(const char *src, size_t srcLen, char *dst, size_t cap) {
  size_t o = 0;
  if (cap == 0) return 0;
  for (size_t i = 0; i < srcLen && o + 1 < cap; i++) {
    char c = src[i];
    if (c == '+') dst[o++] = ' ';
    else if (c == '%' && i + 2 < srcLen) {
      dst[o++] = (char)((fromHex(src[i + 1]) << 4) | fromHex(src[i + 2]));
      i += 2;
    } else dst[o++] = c;
  }
  dst[o] = 0;
  return o;
}

// Sucht key=… im x-www-form-urlencoded-Rumpf und dekodiert den Wert.
// Trifft nur auf Feldgrenzen (Anfang oder nach '&'), damit "pct" nicht in "maxpct" anschlaegt.
static inline bool formGetInto(const char *body, const char *key, char *out, size_t cap) {
  size_t klen = strlen(key);
  for (const char *p = body; *p; ) {
    bool feldanfang = (p == body) || (p[-1] == '&');
    if (feldanfang && strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char *v = p + klen + 1;
      const char *e = strchr(v, '&');
      urlDecodeInto(v, e ? (size_t)(e - v) : strlen(v), out, cap);
      return true;
    }
    const char *next = strchr(p, '&');
    if (!next) break;
    p = next + 1;
  }
  if (cap) out[0] = 0;
  return false;
}

// ---------------------------------------------------------------- Namen
// Leerzeichen -> '_', Grossbuchstaben klein, alles ausser [a-z0-9_-] faellt weg.
// Fuehrende/abschliessende Leerzeichen werden vorher abgeschnitten. Leeres Ergebnis -> "fan".
static inline void sanitizeNameInto(const char *in, char *out, size_t cap) {
  if (cap == 0) return;
  size_t n = strlen(in);
  size_t a = 0, b = n;
  while (a < b && (in[a] == ' ' || in[a] == '\t' || in[a] == '\r' || in[a] == '\n')) a++;
  while (b > a && (in[b-1] == ' ' || in[b-1] == '\t' || in[b-1] == '\r' || in[b-1] == '\n')) b--;
  size_t o = 0;
  for (size_t i = a; i < b && o + 1 < cap; i++) {
    char c = in[i];
    if (c == ' ') c = '_';
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') out[o++] = c;
  }
  out[o] = 0;
  if (o == 0 && cap > 3) { out[0]='f'; out[1]='a'; out[2]='n'; out[3]=0; }
}

// ---------------------------------------------------------------- CSRF (§SEC-1)
// Prueft Origin/Referer gegen den Host-Header. Leerer Origin (curl/native) ist erlaubt —
// LAN-direkt ist im Bedrohungsmodell akzeptiert und haelt den /ota-Notfallweg offen.
// Eine FREMDE Authority (Browser, cross-origin) wird abgelehnt.
static inline bool originIsSelf(const char *originRef, const char *host) {
  if (!originRef || originRef[0] == 0) return true;
  if (!host || host[0] == 0)           return true;   // nicht entscheidbar -> nicht blocken
  const char *auth = strstr(originRef, "://");
  auth = auth ? auth + 3 : originRef;
  size_t n = 0;
  while (auth[n] && auth[n] != '/') n++;              // authority = host[:port]
  if (n != strlen(host)) return false;
  for (size_t i = 0; i < n; i++) {
    char a = auth[i], b = host[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

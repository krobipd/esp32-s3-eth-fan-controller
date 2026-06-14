#include <cassert>
#include <cstdio>
#include "../../fan_controller/fw_util.h"

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

  // §18: RAM-only Pending-Cleanup-Liste — verwaiste retained Topics bei MQTT-disconnect merken
  PendingCleanup pc = {};
  assert(pc.count == 0);
  assert(pendingCleanupAdd(pc, "nas"));            // neu
  assert(pc.count == 1 && strcmp(pc.names[0], "nas") == 0);
  assert(pendingCleanupAdd(pc, "nas"));            // dedup -> bleibt 1
  assert(pc.count == 1);
  assert(pendingCleanupAdd(pc, "mac"));
  assert(pc.count == 2);
  assert(!pendingCleanupAdd(pc, ""));              // leerer Name -> abgelehnt
  assert(pc.count == 2);
  char nm[8];                                       // bis CLEANUP_MAX auffuellen
  for (int k = pc.count; k < CLEANUP_MAX; k++) { snprintf(nm, sizeof nm, "f%d", k); assert(pendingCleanupAdd(pc, nm)); }
  assert(pc.count == CLEANUP_MAX);
  assert(!pendingCleanupAdd(pc, "ueberlauf"));      // voll -> drop (= altes Verhalten)
  assert(pc.count == CLEANUP_MAX);
  pendingCleanupClear(pc);
  assert(pc.count == 0);

  puts("OK");
  return 0;
}

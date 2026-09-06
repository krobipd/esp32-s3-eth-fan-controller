// Host-Tests fuer die extrahierte Logik (fan_logic.h).
// Bis v5.4.3 lag all das im Sketch und lief durch KEINEN Test — darunter der CSRF-Schutz
// und der Request-Parser, durch den jeder POST geht. Die Erwartungen hier bilden das
// Verhalten von v5.4.3 ab; die Extraktion darf nichts daran aendern.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../../fan_controller/fan_logic.h"

static void pruefeDecode(const char *ein, const char *erwartet) {
  char buf[128];
  urlDecodeInto(ein, strlen(ein), buf, sizeof(buf));
  if (strcmp(buf, erwartet) != 0) {
    printf("urlDecode(\"%s\") = \"%s\", erwartet \"%s\"\n", ein, buf, erwartet);
    assert(false);
  }
}
static void pruefeSanitize(const char *ein, const char *erwartet) {
  char buf[20];
  sanitizeNameInto(ein, buf, sizeof(buf));
  if (strcmp(buf, erwartet) != 0) {
    printf("sanitize(\"%s\") = \"%s\", erwartet \"%s\"\n", ein, buf, erwartet);
    assert(false);
  }
}

int main() {
  // ---------------- Pin-Sperren (W5500, USB, Strapping, Boot) ----------------
  for (uint8_t p = 9; p <= 14; p++) assert(isPinBlocked(p));      // W5500-SPI
  assert(isPinBlocked(19) && isPinBlocked(20));                    // USB CDC
  assert(isPinBlocked(0) && isPinBlocked(3));                      // Boot/JTAG
  for (uint8_t p = 43; p <= 46; p++) assert(isPinBlocked(p));      // Strapping
  assert(isPinBlocked(0xFF));                                      // "kein Pin"
  assert(!isPinBlocked(1) && !isPinBlocked(21) && !isPinBlocked(48));
  // GPIO 33-37: PSRAM-Pins, aber hier NICHT gesperrt (PSRAM ist disabled, dort haengen Tachos)
  for (uint8_t p = 33; p <= 37; p++) assert(!isPinBlocked(p));
  static const uint8_t liste[] = {1, 2, 4, 8};
  assert(inList(2, liste, 4) && !inList(3, liste, 4));

  // ---------------- Drehzahl ----------------
  // 2 Impulse/Umdrehung, 1000 Impulse in 2500 ms -> 1000*60000/(2500*2) = 12000
  assert(rpmFromPulses(1000, 2500, 2) == 12000);
  assert(rpmFromPulses(0, 2500, 2) == 0);
  assert(rpmFromPulses(100, 0, 2) == 0);          // Division geschuetzt
  assert(rpmFromPulses(100, 500, 0) == 12000);    // pulsesPerRev 0 -> als 1 gerechnet
  // §G: dieselbe Impulszahl ueber eine LAENGERE Spanne ergibt eine NIEDRIGERE Drehzahl.
  // Genau das war der Fehler: mit dem nominalen Intervall kam immer der hoehere Wert heraus.
  assert(rpmFromPulses(100, 500, 2) > rpmFromPulses(100, 550, 2));
  // Ueberlaufschranke: Zwischenwert pulses*60000 muss in uint32 passen (max 71582)
  assert(rpmFromPulses(71582, 1, 1) == 71582UL * 60000UL);
  assert(rpmFromPulses(4000000000UL, 1000, 2) == rpmFromPulses(71582, 1000, 2));  // geklemmt
  // Realistischer Storm-Fall: PCNT-Budget 16000 Impulse in 2500 ms
  assert(rpmFromPulses(16000, 2500, 2) == 192000);

  // Median-of-3 in allen sechs Reihenfolgen
  assert(median3(1, 2, 3) == 2); assert(median3(1, 3, 2) == 2);
  assert(median3(2, 1, 3) == 2); assert(median3(2, 3, 1) == 2);
  assert(median3(3, 1, 2) == 2); assert(median3(3, 2, 1) == 2);
  assert(median3(5, 5, 5) == 5); assert(median3(0, 0, 9) == 0);

  // Glitchfilter-Schwelle: hoeherer Duty -> hoehere erwartete Drehzahl -> kuerzere Impulse
  assert(computeMinPulseUs(255, 2200, 2, 400) < computeMinPulseUs(10, 2200, 2, 400));
  assert(computeMinPulseUs(0, 2200, 2, 400) >= 400);       // Boden greift
  assert(computeMinPulseUs(255, 2200, 2, 999999) == 999999);

  // ---------------- URL-Dekodierung ----------------
  pruefeDecode("abc", "abc");
  pruefeDecode("a+b", "a b");
  pruefeDecode("%41%42", "AB");
  pruefeDecode("%2f", "/");
  pruefeDecode("%2F", "/");                 // Gross- und Kleinschreibung im Hex
  pruefeDecode("Neu+L%C3%BCfter", "Neu L\xC3\xBC" "fter");   // UTF-8 bleibt heil
  pruefeDecode("100%", "100%");             // unvollstaendige Sequenz am Ende: woertlich
  pruefeDecode("50%2", "50%2");             // dito
  pruefeDecode("", "");
  { // Ueberlauf: Ziel kleiner als Quelle -> abschneiden, immer nullterminiert
    char klein[5];
    urlDecodeInto("abcdefgh", 8, klein, sizeof(klein));
    assert(strcmp(klein, "abcd") == 0);
  }

  // ---------------- Formularfelder ----------------
  char v[64];
  assert(formGetInto("idx=3&pct=75", "idx", v, sizeof(v)) && strcmp(v, "3") == 0);
  assert(formGetInto("idx=3&pct=75", "pct", v, sizeof(v)) && strcmp(v, "75") == 0);
  assert(!formGetInto("idx=3&pct=75", "name", v, sizeof(v)) && v[0] == 0);
  assert(formGetInto("name=Neu+L%C3%BCfter&pwm=18", "name", v, sizeof(v))
         && strcmp(v, "Neu L\xC3\xBC" "fter") == 0);
  assert(formGetInto("a=1&b=&c=3", "b", v, sizeof(v)) && v[0] == 0);   // leerer Wert existiert
  assert(formGetInto("x=9", "x", v, sizeof(v)) && strcmp(v, "9") == 0);
  // Feldgrenzen: ein Schluessel darf NICHT als Teilstring eines anderen anschlagen
  assert(!formGetInto("maxpct=50", "pct", v, sizeof(v)));
  assert(!formGetInto("a=pct=7", "pct", v, sizeof(v)));
  assert(formGetInto("maxpct=50&pct=7", "pct", v, sizeof(v)) && strcmp(v, "7") == 0);
  assert(!formGetInto("", "idx", v, sizeof(v)));

  // ---------------- Namens-Bereinigung ----------------
  pruefeSanitize("fan-a", "fan-a");
  pruefeSanitize("Fan A", "fan_a");                 // gross -> klein, Leerzeichen -> _
  pruefeSanitize("  Lueft er  ", "lueft_er");        // aussen trimmen, innen ersetzen
  pruefeSanitize("Fan#1!", "fan1");                  // Sonderzeichen fallen weg
  pruefeSanitize("", "fan");                          // leer -> Ersatzname
  pruefeSanitize("###", "fan");                       // nur Sonderzeichen -> Ersatzname
  pruefeSanitize("   ", "fan");
  pruefeSanitize("ABC-123_x", "abc-123_x");
  { char eng[6]; sanitizeNameInto("abcdefghij", eng, sizeof(eng)); assert(strcmp(eng, "abcde") == 0); }

  // ---------------- CSRF (§SEC-1) ----------------
  assert(originIsSelf("", "geraet.local"));                       // curl: kein Origin -> erlaubt
  assert(originIsSelf(nullptr, "geraet.local"));
  assert(originIsSelf("http://geraet.local", "geraet.local"));
  assert(originIsSelf("http://geraet.local/pfad", "geraet.local"));
  assert(originIsSelf("https://geraet.local", "geraet.local"));
  assert(originIsSelf("http://GERAET.local", "geraet.local"));     // Vergleich ohne Gross/Klein
  assert(originIsSelf("http://geraet.local:80", "geraet.local:80"));
  assert(originIsSelf("http://192.0.2.9", "192.0.2.9"));
  assert(originIsSelf("http://x", ""));                            // kein Host -> nicht entscheidbar
  // ... und was abgelehnt werden MUSS:
  assert(!originIsSelf("http://boese.example", "geraet.local"));
  assert(!originIsSelf("http://geraet.local.boese.example", "geraet.local"));  // Praefix-Trick
  assert(!originIsSelf("http://geraet.loca", "geraet.local"));                 // kuerzer
  assert(!originIsSelf("http://geraet.local:8080", "geraet.local"));           // anderer Port
  assert(!originIsSelf("http://geraet.local", "geraet.local:80"));

  puts("OK");
  return 0;
}

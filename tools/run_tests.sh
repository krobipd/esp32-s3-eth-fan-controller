#!/usr/bin/env bash
# EIN Einstiegspunkt fuer die geraetelose Testkette: C++-Host-Tests plus Syntaxpruefung
# der Oberflaeche und der Werkzeuge. Vorher liefen die Host-Tests nur als Copy-Paste-Zeile
# aus der README bzw. innerhalb von tools/release.sh -- und das JavaScript lief durch
# gar nichts. Nach der Lehre "was niemand ausfuehrt, ist nicht geprueft" gehoert die
# ganze Kette in ein ausfuehrbares Skript, das auch die CI faehrt.
#
# Der Firmware-Build (arduino-cli) bleibt ein eigener Schritt -- er braucht die
# ESP32-Toolchain; dieses Skript kommt mit c++, node und python3 aus.
set -euo pipefail
cd "$(dirname "$0")/.."

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Mit JEDEM verfuegbaren Compiler bauen: clang und gcc warnen unterschiedlich, und mit
# -Werror faellt ein Befund schon dann auf, wenn ihn nur einer von beiden sieht.
UEBERSETZER=""
for c in c++ g++ clang++; do
    command -v "$c" >/dev/null 2>&1 || continue
    pfad="$(command -v "$c")"
    ziel="$(readlink "$pfad" 2>/dev/null || echo "$pfad")"
    case " $UEBERSETZER " in *" $ziel "*) ;; *) UEBERSETZER="$UEBERSETZER $ziel" ;; esac
done
[ -n "$UEBERSETZER" ] || { echo "FEHLER: kein C++-Compiler gefunden."; exit 1; }

# -Wall -Wextra -Werror + Sanitizer: Die Logik rechnet mit rohen Puffern (char[20],
# memcpy, snprintf) und mit Zeit-Arithmetik ueber den 32-bit-Wrap. Ein Ueberlauf oder
# ein Signed-Overflow faellt auf dem Host sonst nicht auf und schlaegt erst auf dem
# Geraet zu -- dort ohne Fehlermeldung und ohne Debugger.
anzahl=0
for CXX in $UEBERSETZER; do
    name="$(basename "$CXX")"
    for t in logic parse; do
        printf '  %-10s test_%s ... ' "$name" "$t"
        "$CXX" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
               "tests/host/test_$t.cpp" -o "$TMP/t_$t"
        "$TMP/t_$t" >/dev/null
        echo "OK"
        anzahl=$((anzahl + 1))
    done
done
echo "Host-Tests: $anzahl Laeufe gruen ($(echo "$UEBERSETZER" | wc -w | tr -d ' ') Compiler)"

bash tools/check_web.sh
echo "Testkette komplett gruen."

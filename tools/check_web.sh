#!/usr/bin/env bash
# Syntaxpruefung aller Nicht-C++-Artefakte im Lieferumfang.
#
# Warum das noetig ist: tools/build_ui.sh gzippt ui/index.html und schreibt sie mit
# xxd -i als Byte-Array nach fan_controller/ui_asset.h. Der Compiler sieht danach nur
# noch Zahlen -- ein Syntaxfehler im inline-JavaScript uebersetzt fehlerfrei und wird
# auf das Geraet geflasht. Genau diese Konstellation hat im Schwesterprojekt eine
# komplette Seite fuenf Tage lang lahmgelegt, ohne dass ein Test es bemerkt hat.
set -euo pipefail
cd "$(dirname "$0")/.."

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
rc=0

# --- inline-JavaScript der Oberflaeche ---
# extract_ui_js.py haelt die Zeilennummern des HTML ein, damit eine Fehlermeldung
# direkt auf die Zeile in ui/index.html zeigt.
if ! command -v node >/dev/null 2>&1; then
    echo "FEHLER: node nicht gefunden -- die JS-Pruefung ist Pflicht, kein optionaler Schritt."
    echo "        (Ein still uebersprungener Check ist schlimmer als keiner: er meldet gruen.)"
    exit 1
fi
python3 tools/extract_ui_js.py ui/index.html "$TMP/ui.js"
if ! node --check "$TMP/ui.js" 2>"$TMP/js.err"; then
    echo "SYNTAXFEHLER im inline-JavaScript von ui/index.html:"
    sed "s#$TMP/ui.js#ui/index.html#" "$TMP/js.err" | head -8
    rc=1
fi

# --- Python: Mock und Werkzeuge ---
for p in tools/mock_api.py tools/extract_ui_js.py; do
    python3 -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" "$p" \
        || { echo "SYNTAXFEHLER: $p"; rc=1; }
done

# --- Shell-Skripte ---
sh   -n tools/build_ui.sh   || { echo "SYNTAXFEHLER: tools/build_ui.sh"; rc=1; }
bash -n tools/release.sh    || { echo "SYNTAXFEHLER: tools/release.sh"; rc=1; }
bash -n tools/check_web.sh  || { echo "SYNTAXFEHLER: tools/check_web.sh"; rc=1; }
bash -n tools/run_tests.sh  || { echo "SYNTAXFEHLER: tools/run_tests.sh"; rc=1; }

# --- ui_asset.h muss zu ui/index.html passen ---
# Sonst laeuft die geflashte Oberflaeche der Quelle hinterher, ohne dass es auffaellt.
# Wichtig: gegen einen FRISCHEN Build vergleichen, nicht gegen den git-Stand -- eine
# legitime, noch nicht committete UI-Aenderung ist kein Fehler. Und niemals `git checkout`
# im Pruefpfad: ein Test darf den Arbeitsbaum nicht anfassen.
gzip -9 -n -c ui/index.html > "$TMP/ui.gz"
{
    echo "// GENERIERT von tools/build_ui.sh - nicht von Hand editieren"
    echo "#pragma once"
    echo "#include <pgmspace.h>"
    echo "static const uint8_t UI_ASSET[] PROGMEM = {"
    xxd -i < "$TMP/ui.gz"
    echo "};"
    echo "static const unsigned int UI_ASSET_LEN = sizeof(UI_ASSET);"
} > "$TMP/ui_asset_erwartet.h"
if ! cmp -s "$TMP/ui_asset_erwartet.h" fan_controller/ui_asset.h; then
    echo "FEHLER: fan_controller/ui_asset.h passt nicht zu ui/index.html."
    echo "        'sh tools/build_ui.sh' ausfuehren und das Ergebnis mit committen."
    rc=1
fi

[ "$rc" -eq 0 ] && echo "Oberflaeche/Werkzeuge: Syntax OK (inline-JS + 2 Python + 4 Shell + ui_asset.h aktuell)"
exit "$rc"

#!/usr/bin/env bash
# tools/release.sh — Release-Workflow für den ESP32-S3-ETH Fan Controller.
#
# Mechanische Schritte als Subcommands. Jeder STOPPT vor dem nächsten (gated) Schritt —
# nichts wird automatisch verkettet. FLASH ist ein eigener Schritt, NUR nach ausdrücklicher Freigabe.
#
#   build                        UI-Build + Host-Tests + Compile (grün = Voraussetzung für jeden Release)
#   prep    <version> <notes>    auf Feature-Branch: FW_VERSION bumpen, build, commit, push, PR öffnen
#   publish <version> <notes>    nach CI-grün: PR squash-mergen, main nachziehen (HEAD prüfen), Tag + GitHub-Release
#   flash   <version>            NUR nach „ja, flash": OTA-Flash des gebauten Images + Health-Check
#
# version = Semver x.y.z (FW_VERSION im .ino ist die EINZIGE Versionsquelle).
# notes   = Pfad zu einer Release-Notes-Datei (1. Zeile = Commit-/PR-/Release-Titel, Rest = Body).
# Env:  FANCTL_DEVICE (OTA-Ziel, default fan-controller.local; fürs Flashen besser die IP setzen) · ARDUINO_CLI (Pfad zur arduino-cli).
set -euo pipefail
cd "$(dirname "$0")/.."

INO="fan_controller/fan_controller.ino"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc"
OUT="/tmp/fanctl-release"
BIN="$OUT/fan_controller.ino.bin"
DEVICE="${FANCTL_DEVICE:-fan-controller.local}"   # OTA-Ziel; fürs Flashen besser die IP setzen (mDNS löst nach Reboot langsam): FANCTL_DEVICE=10.x.x.x
CLI="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"

die()    { echo "FEHLER: $*" >&2; exit 1; }
vcode()  { grep -oE '#define FW_VERSION "[0-9]+\.[0-9]+\.[0-9]+"' "$INO" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+'; }
semver() { [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "Version muss x.y.z sein: '$1'"; }

do_build() {
  echo "== UI-Build =="            ; sh tools/build_ui.sh
  echo "== Host-Tests =="          ; c++ -std=c++17 tests/host/test_logic.cpp -o /tmp/fanctl-test && /tmp/fanctl-test
  echo "== Compile ($FQBN) =="     ; rm -rf "$OUT"; "$CLI" compile --fqbn "$FQBN" --output-dir "$OUT" fan_controller
  echo "OK build — Code-Version $(vcode) — Image $BIN"
}

do_prep() {
  local ver="${1:?Version fehlt (arg1)}" notes="${2:?Notes-Datei fehlt (arg2)}"; semver "$ver"
  [[ -f "$notes" ]] || die "Notes-Datei nicht gefunden: $notes"
  local br; br="$(git branch --show-current)"
  [[ "$br" != "main" && -n "$br" ]] || die "Nicht von main releasen — erst Feature-Branch anlegen + Änderungen committen"
  sed -i '' -E "s/#define FW_VERSION \"[^\"]+\"/#define FW_VERSION \"$ver\"/" "$INO"
  [[ "$(vcode)" == "$ver" ]] || die "Version-Bump fehlgeschlagen"
  do_build
  git add -A
  git commit -F "$notes"
  git push -u origin "$br"
  gh pr create --base main --title "v$ver" --body-file "$notes"
  echo
  echo ">> PREP fertig. PR ist offen (Branch '$br'). Nächstes: CI grün abwarten + PR prüfen,"
  echo ">> dann:  bash tools/release.sh publish $ver $notes"
}

do_publish() {
  local ver="${1:?Version fehlt (arg1)}" notes="${2:?Notes-Datei fehlt (arg2)}"; semver "$ver"
  [[ -f "$notes" ]] || die "Notes-Datei nicht gefunden: $notes"
  local br; br="$(git branch --show-current)"
  [[ "$br" != "main" && -n "$br" ]] || die "publish vom Feature-Branch aus aufrufen (der gemergt werden soll)"
  local pr; pr="$(gh pr view "$br" --json number -q .number 2>/dev/null)" || die "Kein PR für Branch '$br'"
  gh pr checks "$pr" >/dev/null || die "CI für PR #$pr nicht grün/komplett — Merge abgebrochen"
  local before after; before="$(git rev-parse origin/main)"
  gh pr merge "$pr" --squash --admin --delete-branch
  git checkout main && git pull --ff-only origin main
  after="$(git rev-parse origin/main)"
  [[ "$before" != "$after" ]] || die "main-HEAD hat sich NICHT bewegt — Merge nicht erfolgt, KEIN Release"
  echo "main HEAD jetzt: $(git rev-parse --short HEAD)"
  gh release create "v$ver" --target main --title "v$ver" --notes-file "$notes"
  git branch -D "$br" 2>/dev/null || true; git remote prune origin >/dev/null 2>&1 || true
  echo
  echo ">> PUBLISH fertig: v$ver auf main + GitHub-Release (Tag auf $(git rev-parse --short HEAD))."
  echo ">> FLASHEN ist ein eigener, freizugebender Schritt — NUR nach 'ja, flash':"
  echo ">>   bash tools/release.sh flash $ver"
}

do_flash() {
  local ver="${1:?Version fehlt (arg1)}"; semver "$ver"
  [[ -f "$BIN" ]] || die "Kein Image ($BIN) — erst 'build' oder 'prep' laufen lassen"
  [[ "$(vcode)" == "$ver" ]] || echo "WARN: Code-Version $(vcode) ≠ $ver — flashe das gebaute Image $BIN"
  echo "== OTA-Flash v$ver -> $DEVICE =="
  curl -fsS --max-time 120 -X POST --data-binary @"$BIN" \
       -H 'Content-Type: application/octet-stream' "http://$DEVICE/ota" || die "OTA-Upload fehlgeschlagen"
  echo; echo "Gerät rebootet — warte auf Erreichbarkeit (per IP, mDNS löst langsam auf)..."
  local r=""
  for i in $(seq 1 24); do
    r="$(curl -fsS --max-time 4 "http://$DEVICE/api/status" 2>/dev/null || true)"
    [[ -n "$r" ]] && break
    sleep 5
  done
  [[ -n "$r" ]] || die "Gerät nach Flash nicht erreichbar — Status manuell prüfen (Anti-Brick rollt sonst nach 120s zurück)"
  echo "$r" | python3 -c 'import sys,json;d=json.load(sys.stdin);print("fw_version",d["fw_version"],"| ota_pending",d["ota_pending"],"| crash_streak",d["crash_streak"],"| safe_mode",d["safe_mode"],"| mqtt",d["mqtt_connected"],"| boot_count",d["boot_count"])'
  echo
  echo ">> Health prüfen: fw_version == $ver, crash_streak 0, safe_mode false, mqtt true."
  echo ">> Nach ~90s Health-Window sollte ota_pending false sein (Commit, kein Rollback) — ggf. /api/status erneut abrufen."
}

case "${1:-}" in
  build)   do_build ;;
  prep)    shift; do_prep    "$@" ;;
  publish) shift; do_publish "$@" ;;
  flash)   shift; do_flash   "$@" ;;
  *) echo "Usage: bash tools/release.sh {build | prep <ver> <notes> | publish <ver> <notes> | flash <ver>}"; exit 1 ;;
esac

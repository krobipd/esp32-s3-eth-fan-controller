#!/usr/bin/env python3
"""Verhaltenstests gegen den Geraeteersatz (tools/mock_api.py).

Sie halten fest, dass der Mock sich wie die Firmware verhaelt — jede dieser Pruefungen
entspricht einer Stelle in fan_controller.ino. Am 06.09.2026 war der Mock an fuenf
Punkten LASCHER als das Geraet (u.a. eine Route /api/fan/new, die es gar nicht gibt);
gegen so einen Mock laeuft eine Oberflaeche gruen und scheitert dann am Geraet.

    python3 tools/test_mock.py [basis-url]      # Standard: http://127.0.0.1:8077

Die Basis-URL ist absichtlich frei waehlbar: dieselben Tests laufen unveraendert gegen
ein echtes Geraet (dann sind es Abnahmetests) — NUR mit ausdruecklichem Auftrag.
"""
import json
import sys
import urllib.error
import urllib.request

BASIS = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8077"
fehler = []
anzahl = 0


def ruf(pfad, daten=None, ctype="application/x-www-form-urlencoded", kopf=None):
    """Gibt (status, rumpf) zurueck — auch bei 4xx, statt zu werfen."""
    url = BASIS + pfad
    leib = daten.encode() if isinstance(daten, str) else daten
    req = urllib.request.Request(url, data=leib, method="POST" if leib is not None else "GET")
    if leib is not None:
        req.add_header("Content-Type", ctype)
    for k, v in (kopf or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def pruefe(name, bedingung, hinweis=""):
    global anzahl
    anzahl += 1
    if bedingung:
        print("  OK   %s" % name)
    else:
        print("  FEHL %s %s" % (name, hinweis))
        fehler.append(name)


def json_von(rumpf):
    try:
        return json.loads(rumpf)
    except ValueError:
        return {}


print("Verhaltenstests gegen %s" % BASIS)

# --- 1. Routen: was es NICHT gibt, muss 404 sein -------------------------------
st, _ = ruf("/api/fan/new", "idx=-1")
pruefe("/api/fan/new existiert nicht (404)", st == 404, "-> %d" % st)
st, _ = ruf("/gibtsnicht")
pruefe("unbekannter GET-Pfad -> 404", st == 404, "-> %d" % st)
st, _ = ruf("/api/quatsch", "x=1")
pruefe("unbekannter POST-Pfad -> 404", st == 404, "-> %d" % st)

# --- 2. /api/status: alle in docs/API.md zugesagten Felder ---------------------
st, rumpf = ruf("/api/status")
s = json_von(rumpf)
pruefe("/api/status liefert 200", st == 200, "-> %d" % st)
for feld in ("rev", "fw_version", "device", "ip", "mqtt_connected", "boot_count",
             "safe_mode", "reset_reason", "crash_streak", "wdt", "ota_pending",
             "min_free_heap", "largest_block", "uptime_s", "core1_loops",
             "net_loops", "net_stack_hwm", "mqtt", "free_pwm", "free_tach", "fans"):
    pruefe("status.%s vorhanden" % feld, feld in s)
for feld in ("enabled", "host", "port", "user", "prefix", "hadisc"):
    pruefe("status.mqtt.%s vorhanden" % feld, feld in s.get("mqtt", {}))
pruefe("status.mqtt enthaelt KEIN Passwort", "pass" not in s.get("mqtt", {}))

# --- 3. CSRF (§SEC-1) ----------------------------------------------------------
st, _ = ruf("/api/fan/set", "idx=0&pct=50", kopf={"Origin": "http://boese.example"})
pruefe("cross-origin POST -> 403", st == 403, "-> %d" % st)
st, _ = ruf("/api/fan/set", "idx=0&pct=50")
pruefe("POST ohne Origin erlaubt (curl-Pfad)", st == 200, "-> %d" % st)

# --- 4. handleFormPost: Laenge und Content-Type --------------------------------
st, _ = ruf("/api/fan/set", "", ctype="application/x-www-form-urlencoded")
pruefe("leerer Body -> 400", st == 400, "-> %d" % st)
st, _ = ruf("/api/fan/set", "idx=0&pct=50", ctype="application/json")
pruefe("falscher Content-Type -> 400", st == 400, "-> %d" % st)
st, _ = ruf("/api/fan/set", "x=" + "a" * 5000)
pruefe("Body > 4096 -> 400", st == 400, "-> %d" % st)

# --- 5. apiFanSet: Bereichspruefung -------------------------------------------
for daten, was in (("idx=0&pct=101", "pct>100"), ("idx=0&pct=-1", "pct<0"),
                   ("idx=99&pct=50", "idx ausserhalb"), ("idx=7&pct=50", "leerer Slot")):
    st, rumpf = ruf("/api/fan/set", daten)
    j = json_von(rumpf)
    pruefe("fan/set lehnt %s ab" % was, st == 400 and j.get("ok") is False,
           "-> %d %s" % (st, rumpf[:40]))
st, rumpf = ruf("/api/fan/set", "idx=0&pct=50")
pruefe("fan/set akzeptiert gueltig", json_von(rumpf).get("ok") is True)

# --- 6. apiFanSave: Namen und Pins --------------------------------------------
for daten, was in (("idx=0&name=", "leerer Name"),
                   ("idx=0&name=status", "reservierter Name"),
                   ("idx=0&name=zu-lang-1234567890123", "Name > 19 Zeichen"),
                   ("idx=0&name=fan-b", "Name schon belegt"),
                   ("idx=0&name=fan-a&pwm=9&tach=2", "gesperrter Pin (W5500)"),
                   ("idx=0&name=fan-a&pwm=5&tach=5", "PWM == Tacho"),
                   ("idx=0&name=fan-a&pwm=8&tach=4", "Pin von anderem Luefter belegt"),
                   ("idx=-1&name=neu", "neuer Luefter ohne Pins")):
    st, rumpf = ruf("/api/fan/save", daten)
    j = json_von(rumpf)
    pruefe("fan/save lehnt %s ab" % was, st == 400 and j.get("ok") is False,
           "-> %d %s" % (st, rumpf[:50]))

# --- 7. apiMqttSave: Prefix-Validierung (§SEC-2/F4) ---------------------------
for p, was in (("", "leer"), ("a/b", "Hierarchie"), ("a#b", "Wildcard #"),
               ("a+b", "Wildcard +"), ('a"b', "JSON-Break"),
               ("abcdefghij123456", "16 Zeichen")):
    st, rumpf = ruf("/api/mqtt", "prefix=" + urllib.request.quote(p))
    pruefe("mqtt lehnt Prefix %s ab" % was, st == 400 and json_von(rumpf).get("ok") is False,
           "-> %d" % st)
st, rumpf = ruf("/api/mqtt", "prefix=Home_1-x&enabled=1")
pruefe("mqtt akzeptiert gueltigen Prefix", json_von(rumpf).get("ok") is True)

# --- 8. OTA: Content-Type und Groesse -----------------------------------------
st, _ = ruf("/ota", "abc", ctype="application/x-www-form-urlencoded")
pruefe("/ota lehnt falschen Content-Type ab (400)", st == 400, "-> %d" % st)
st, rumpf = ruf("/ota", b"", ctype="application/octet-stream")
pruefe("/ota ohne Rumpf -> 400 + ok:false",
       st == 400 and json_von(rumpf).get("ok") is False, "-> %d" % st)

# --- 9. Loeschen und Wiederanlegen --------------------------------------------
st, rumpf = ruf("/api/fan/delete", "idx=3")
pruefe("fan/delete akzeptiert belegten Slot", json_von(rumpf).get("ok") is True)
st, rumpf = ruf("/api/fan/delete", "idx=3")
pruefe("fan/delete lehnt leeren Slot ab", st == 400, "-> %d" % st)
st, rumpf = ruf("/api/fan/save", "idx=-1&name=Neu Fan&pwm=18&tach=21")
pruefe("neuer Luefter mit Pins wird angelegt", json_von(rumpf).get("ok") is True,
       rumpf[:60])
st, rumpf = ruf("/api/status")
namen = [f["name"] for f in json_von(rumpf).get("fans", [])]
pruefe("Name wurde sanitisiert (Neu Fan -> neu_fan)", "neu_fan" in namen, str(namen))

print("\n%d Pruefungen, %d Fehler" % (anzahl, len(fehler)))
if fehler:
    for f in fehler:
        print("  FEHLGESCHLAGEN: %s" % f)
    sys.exit(1)
print("Verhaltenstests gruen.")

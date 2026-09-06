#!/usr/bin/env python3
"""Geraeteloser Ersatz fuer die Fan-Controller-Firmware — UI-Entwicklung ohne Hardware.

    python3 tools/mock_api.py      ->  http://127.0.0.1:8077

WICHTIG — die Regel, an der sich dieser Mock messen lassen muss:
Ein Ersatz darf NIE weniger verlangen, weniger koennen oder anders antworten als das
echte System. Ein zu strenger Mock nervt beim Entwickeln, ein zu lascher LUEGT: er
verschiebt Fehler genau dorthin, wo sie am teuersten sind — auf ein Geraet ohne USB.

Deshalb bildet dieser Mock die Handler aus fan_controller/fan_controller.ino nach:
dieselben Routen (und NUR die), dieselbe Eingabepruefung, dieselben Fehlertexte,
dieselben Statuscodes, dieselbe CSRF-Pruefung. tools/test_mock.sh haelt das fest.

Alle Daten sind Platzhalter (RFC-5737-Adressen), kein echtes Setup.
"""
import json
import random
import re
import http.server
from urllib.parse import unquote_plus

PORT = 8077
MAX_FANS = 8            # MAX_FANS
PWM_ALLOWED = [1, 2, 4, 5, 6, 7, 8, 15, 16, 17, 18, 21,
               33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 47, 48]
TACH_ALLOWED = list(PWM_ALLOWED)
FW_VERSION = "5.5.0"
RESERVIERT = ("status", "sys", "info")   # fanNameValid()


# ---------------------------------------------------------------- Firmware-Logik
def pct_from_duty(duty):
    """pctFromDuty() aus fw_util.h."""
    return (duty * 100 + 127) // 255


def duty_from_pct(pct):
    """dutyFromPct() aus fw_util.h."""
    pct = min(pct, 100)
    return (pct * 255 + 50) // 100


def sanitize_name(roh):
    """sanitizeName() aus fan_controller.ino: trim, ' '->'_', klein, nur [a-z0-9_-]."""
    out = []
    for c in roh.strip():
        if c == " ":
            c = "_"
        c = c.lower() if "A" <= c <= "Z" else c
        if c.islower() and c.isascii() or c.isdigit() and c.isascii() or c in "_-":
            out.append(c)
    return "".join(out) or "fan"


def fan_name_valid(s):
    """fanNameValid() aus fw_util.h: 1..19 Zeichen, a-z 0-9 _ -, nicht reserviert."""
    if not 1 <= len(s) <= 19:
        return False
    if not re.fullmatch(r"[a-z0-9_-]+", s):
        return False
    return s not in RESERVIERT


def mqtt_prefix_valid(s):
    """mqttPrefixValid() aus fw_util.h: 1..15 Zeichen, A-Za-z0-9_-."""
    return bool(1 <= len(s) <= 15 and re.fullmatch(r"[A-Za-z0-9_-]+", s))


# ---------------------------------------------------------------- Zustand
def leerer_fan(i):
    return {"index": i, "name": "", "present": False, "pwm": 0, "pct": 0, "rpm": 0,
            "pwmPin": 255, "tachPin": 255, "fault": 0, "validated": False,
            "inv": False, "cmin": 0, "cnote": ""}


FANS = [leerer_fan(i) for i in range(MAX_FANS)]
FANS[0].update(name="fan-a", present=True, pwm=133, pct=52, rpm=724,
               pwmPin=1, tachPin=2, validated=True, cmin=20, cnote="120mm")
FANS[1].update(name="fan-b", present=True, pwm=140, pct=55, rpm=811,
               pwmPin=8, tachPin=15, validated=True)
FANS[2].update(name="fan-c", present=True, pwm=128, pct=50, rpm=747,
               pwmPin=16, tachPin=17, fault=3, validated=True, inv=True)
FANS[3].update(name="fan-d", present=True, pwm=148, pct=58, rpm=858,
               pwmPin=18, tachPin=21, validated=True, cmin=15)

ZUSTAND = {
    "rev": 1, "boot_count": 42, "safe_mode": False, "crash_streak": 0,
    "ota_pending": False, "core1_loops": 1234567, "net_loops": 1234890,
    "mqtt": {"enabled": True, "host": "192.0.2.10", "port": 1883,
             "user": "user", "prefix": "esp", "hadisc": False},
}


def belegt(i):
    return FANS[i]["name"] != ""


def pin_in_use(pin, ausser=-1):
    """pinInUse() — prueft nur belegte Slots (fanPresentIdx)."""
    return any(i != ausser and FANS[i]["present"]
               and pin in (FANS[i]["pwmPin"], FANS[i]["tachPin"])
               for i in range(MAX_FANS))


# ---------------------------------------------------------------- HTTP
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # -- Antwort-Helfer, 1:1 wie die Firmware ------------------------------
    def _send(self, code, ctype, body):
        roh = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(roh)))
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(roh)

    def _json(self, code, obj):
        self._send(code, "application/json; charset=UTF-8", json.dumps(obj))

    def _ok(self):
        """apiOk()"""
        self._json(200, {"ok": True})

    def _err(self, msg):
        """apiErr() — 400 mit {"ok":false,"error":…}"""
        self._json(400, {"ok": False, "error": msg})

    def _404(self):
        """httpSend404()"""
        self._send(404, "text/plain; charset=UTF-8", "Not found")

    def _400(self, msg):
        self._send(400, "text/plain; charset=UTF-8", msg)

    def _403(self, msg):
        """httpSend403() — §SEC-1"""
        self._send(403, "text/plain; charset=UTF-8", msg)

    # -- §SEC-1 CSRF -------------------------------------------------------
    def _origin_ist_selbst(self):
        """originIsSelf(): leerer Origin erlaubt (curl), fremde Authority verboten."""
        origin = self.headers.get("Origin") or self.headers.get("Referer") or ""
        host = self.headers.get("Host") or ""
        if not origin or not host:
            return True
        auth = origin.split("://", 1)[-1].split("/", 1)[0]
        return auth.lower() == host.lower()

    # -- GET ---------------------------------------------------------------
    def do_GET(self):
        pfad = self.path.split("?", 1)[0]
        if pfad == "/api/status":
            for f in FANS:
                if f["present"]:
                    f["rpm"] = max(0, f["rpm"] + random.randint(-15, 15))
            self._json(200, self._status())
        elif pfad == "/log.txt":
            self._send(200, "text/plain; charset=UTF-8",
                       "[T+0001.000s #42] [I] BOOT: mock log\n" * 30)
        elif pfad == "/prevlog.txt":
            self._send(200, "text/plain; charset=UTF-8", "[prev boot] mock tail\n")
        elif pfad == "/":
            with open("ui/index.html", encoding="utf-8") as f:
                self._send(200, "text/html; charset=UTF-8", f.read())
        else:
            self._404()          # die Firmware liefert hier NICHT die Oberflaeche

    def _status(self):
        """sendJsonStatus() — vollstaendig, inkl. der Felder aus docs/API.md."""
        frei_pwm = [p for p in PWM_ALLOWED if not pin_in_use(p)]
        frei_tach = [p for p in TACH_ALLOWED if not pin_in_use(p)]
        return {
            "rev": ZUSTAND["rev"], "fw_version": FW_VERSION,
            "device": "ws-s3eth-MOCK1", "ip": "192.0.2.50",
            "mqtt_connected": ZUSTAND["mqtt"]["enabled"],
            "boot_count": ZUSTAND["boot_count"], "safe_mode": ZUSTAND["safe_mode"],
            "reset_reason": "POWERON", "min_free_heap": 201000,
            "largest_block": 198000, "uptime_s": 361445, "wdt": True,
            "core1_loops": ZUSTAND["core1_loops"], "net_loops": ZUSTAND["net_loops"],
            "net_stack_hwm": 4304, "ota_pending": ZUSTAND["ota_pending"],
            "crash_streak": ZUSTAND["crash_streak"],
            "mqtt": dict(ZUSTAND["mqtt"]),
            "free_pwm": frei_pwm, "free_tach": frei_tach,
            "fans": [f for f in FANS if belegt(f["index"])],
        }

    # -- POST --------------------------------------------------------------
    def do_POST(self):
        pfad = self.path.split("?", 1)[0]
        laenge = int(self.headers.get("Content-Length", 0) or 0)
        roh = self.rfile.read(laenge) if laenge else b""

        if not self._origin_ist_selbst():
            return self._403("cross-origin POST blocked")

        # Routen ohne Body-Auswertung (die Firmware liest ihn gar nicht erst)
        if pfad == "/api/safemode/reset":
            ZUSTAND["safe_mode"] = False
            ZUSTAND["crash_streak"] = 0
            return self._ok()
        if pfad == "/api/reboot":
            ZUSTAND["boot_count"] += 1
            return self._ok()
        if pfad == "/ota":
            if not (self.headers.get("Content-Type") or "").startswith("application/octet-stream"):
                return self._400("Use application/octet-stream")
            if laenge == 0:
                return self._json(400, {"ok": False,
                                        "error": "No Content or chunked unsupported"})
            if laenge > 0x200000:
                return self._json(413, {"ok": False, "error": "too large"})
            ZUSTAND["boot_count"] += 1
            return self._json(200, {"ok": True,
                                    "message": "Firmware erfolgreich aktualisiert",
                                    "reboot": True, "reboot_in": 5})

        handler = {"/api/fan/set": self._fan_set, "/api/fan/save": self._fan_save,
                   "/api/fan/delete": self._fan_delete, "/api/calib": self._calib,
                   "/api/mqtt": self._mqtt_save}.get(pfad)
        if handler is None:
            return self._404()

        # handleFormPost(): Laenge und Content-Type prueffen, bevor der Handler laeuft
        if laenge == 0 or laenge > 4096:
            return self._400("bad length")
        if not (self.headers.get("Content-Type") or "").startswith(
                "application/x-www-form-urlencoded"):
            return self._400("Unsupported Content-Type")
        handler(self._form(roh.decode("utf-8", "replace")))

    @staticmethod
    def _form(body):
        """formGet() — erster Treffer je Schluessel gewinnt, +/%xx dekodiert."""
        d = {}
        for teil in body.split("&"):
            if "=" in teil:
                k, _, v = teil.partition("=")
                d.setdefault(k, unquote_plus(v))
        return d

    # -- API-Handler, 1:1 wie die Firmware ---------------------------------
    def _fan_set(self, f):
        try:
            idx, pct = int(f.get("idx", -1)), int(f.get("pct", -1))
        except ValueError:
            return self._err("bad idx/pct")
        if not (0 <= idx < MAX_FANS) or not FANS[idx]["present"] or not (0 <= pct <= 100):
            return self._err("bad idx/pct")
        FANS[idx]["pct"] = pct
        FANS[idx]["pwm"] = duty_from_pct(pct)
        self._ok()

    def _fan_save(self, f):
        roh_idx = f.get("idx", f.get("fan"))
        if roh_idx is None:
            return self._err("missing idx")
        try:
            idx = int(roh_idx)
        except ValueError:
            return self._err("bad idx")
        neu = idx < 0
        if neu:
            frei = [i for i in range(MAX_FANS) if not belegt(i)]
            if not frei:
                return self._err("no free slot")
            idx = frei[0]
        if not 0 <= idx < MAX_FANS:
            return self._err("bad idx")
        fan = FANS[idx]

        name = f.get("name", fan["name"])
        if not name.strip():
            return self._err("name required")
        sauber = sanitize_name(name)
        if not fan_name_valid(sauber):
            return self._err("invalid name")
        for i in range(MAX_FANS):
            if i != idx and belegt(i) and sanitize_name(FANS[i]["name"]) == sauber:
                return self._err("name in use")

        inv = f["inv"] == "1" if "inv" in f else fan["inv"]
        try:
            pwm = int(f["pwm"]) if "pwm" in f else fan["pwmPin"]
            tach = int(f["tach"]) if "tach" in f else fan["tachPin"]
        except ValueError:
            return self._err("bad idx")
        pwm, tach = max(0, min(255, pwm)), max(0, min(255, tach))

        if pwm != fan["pwmPin"] and (pwm not in PWM_ALLOWED or pin_in_use(pwm, idx)):
            return self._err("pwm pin invalid/busy")
        if tach != fan["tachPin"] and (tach not in TACH_ALLOWED or pin_in_use(tach, idx)):
            return self._err("tach pin invalid/busy")
        if neu and (pwm == 255 or tach == 255):
            return self._err("pins required")
        if pwm != 255 and pwm == tach:
            return self._err("pwm/tach pin gleich")

        fan.update(name=sauber, inv=inv, pwmPin=pwm, tachPin=tach,
                   present=pwm != 255 and tach != 255, validated=True)
        ZUSTAND["rev"] += 1
        self._ok()

    def _fan_delete(self, f):
        try:
            idx = int(f.get("idx", -1))
        except ValueError:
            return self._err("bad idx")
        if not (0 <= idx < MAX_FANS) or not belegt(idx):
            return self._err("bad idx")
        FANS[idx] = leerer_fan(idx)
        ZUSTAND["rev"] += 1
        self._ok()

    def _calib(self, f):
        try:
            idx = int(f.get("idx", -1))
        except ValueError:
            return self._err("bad idx")
        if not (0 <= idx < MAX_FANS) or not FANS[idx]["present"]:
            return self._err("bad idx")
        if "cmin" in f:
            try:
                FANS[idx]["cmin"] = max(0, min(100, int(f["cmin"])))
            except ValueError:
                pass
        if "cnote" in f:
            FANS[idx]["cnote"] = f["cnote"][:39]
        ZUSTAND["rev"] += 1
        self._ok()

    def _mqtt_save(self, f):
        # §SEC-2/F4: Prefix ZUERST pruefen — vor jeder Mutation.
        if "prefix" in f and not mqtt_prefix_valid(f["prefix"]):
            return self._err("prefix: nur A-Za-z0-9_- , 1..15 Zeichen")
        m = ZUSTAND["mqtt"]
        m["enabled"] = f.get("enabled") == "1"
        m["hadisc"] = f.get("hadisc") == "1"
        for schluessel, feld in (("host", "host"), ("user", "user"), ("prefix", "prefix")):
            if schluessel in f:
                m[feld] = f[schluessel]
        if "port" in f:
            try:
                m["port"] = max(1, min(65535, int(f["port"])))
            except ValueError:
                pass
        self._ok()

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    print("Mock-API (Geraeteersatz): http://127.0.0.1:%d" % PORT)
    http.server.HTTPServer(("127.0.0.1", PORT), H).serve_forever()

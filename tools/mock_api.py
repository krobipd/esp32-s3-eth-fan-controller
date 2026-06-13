#!/usr/bin/env python3
"""UI-Entwicklung ohne Geraet: python3 tools/mock_api.py -> http://127.0.0.1:8077"""
import json, random, http.server

FANS = [
  {"index":0,"name":"mac","present":True,"pwm":133,"pct":52,"rpm":724,"pwmPin":40,"tachPin":37,"fault":0,"validated":True,"inv":False,"cmin":20,"cnote":"Noctua"},
  {"index":1,"name":"unifi","present":True,"pwm":140,"pct":55,"rpm":811,"pwmPin":42,"tachPin":35,"fault":0,"validated":True,"inv":False,"cmin":0,"cnote":""},
  {"index":2,"name":"usv","present":True,"pwm":128,"pct":50,"rpm":747,"pwmPin":41,"tachPin":36,"fault":3,"validated":True,"inv":True,"cmin":0,"cnote":""},
  {"index":3,"name":"nas","present":True,"pwm":148,"pct":58,"rpm":858,"pwmPin":47,"tachPin":38,"fault":0,"validated":True,"inv":False,"cmin":15,"cnote":"be quiet"},
]

class H(http.server.BaseHTTPRequestHandler):
    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.end_headers()
        self.wfile.write(body.encode())
    def do_GET(self):
        if self.path.startswith("/api/status"):
            for f in FANS:
                f["rpm"] = max(0, f["rpm"] + random.randint(-15, 15))
            self._send(200, "application/json", json.dumps({
                "rev":1,"device":"ws-s3eth-MOCK","ip":"10.47.88.239","mqtt_connected":True,
                "boot_count":42,"safe_mode":False,"reset_reason":"POWERON",
                "min_free_heap":201000,"largest_block":198000,"uptime_s":361445,
                "wdt":True,"ota_pending":False,"crash_streak":0,
                "mqtt":{"enabled":True,"host":"10.47.88.5","port":1883,"user":"iob","prefix":"esp"},
                "free_pwm":[1,2,8,15,16,17,18,21,33,34,39,48],
                "free_tach":[1,2,8,15,16,17,18,21,33,34,39,48],
                "fans":FANS}))
        elif self.path == "/log.txt":
            self._send(200, "text/plain", "[T+0001.000s #42] [I] BOOT: mock log\n" * 30)
        elif self.path == "/prevlog.txt":
            self._send(200, "text/plain", "[prev boot] mock tail\n")
        else:
            self._send(200, "text/html", open("ui/index.html").read())
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path == "/api/fan/new":
            idx = len(FANS)
            FANS.append({"index":idx,"name":"fan%d"%(idx+1),"present":False,"pwm":0,"pct":0,
                         "rpm":0,"pwmPin":255,"tachPin":255,"fault":0,"validated":False,"inv":False,"cmin":0,"cnote":""})
            self._send(200, "application/json", '{"ok":true,"idx":%d}' % idx)
            return
        self._send(200, "application/json", '{"ok":true}')
    def log_message(self, *a): pass

print("Mock-API: http://127.0.0.1:8077")
http.server.HTTPServer(("127.0.0.1", 8077), H).serve_forever()

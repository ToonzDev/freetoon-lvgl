#!/usr/bin/env python3
"""Fake Homey Pro (2023) local Web API for testing FreeToon's Homey support
(BACKLOG #28) in the sim or on a real Toon, without a Homey.

Serves the SYNTHETIC fixtures in tests/fixtures/ only. Never point it at a
real user's export: those hold third-party passwords and keys and must not
be shared or committed (real exports belong in the git-ignored tests/local/).

  python3 homey_mock.py                      # 127.0.0.1:18080, for the sim
  python3 homey_mock.py --bind 0.0.0.0       # reachable from a Toon on the LAN
  python3 homey_mock.py --key mykey --delay 0.5

On the Toon: Settings -> Homey, address <pc-ip>:18080, API key = --key
(default below). In the sim: HOMEY_SIM_KEY=<key> and enable_homey=1 +
homey_host=127.0.0.1:18080 in the cfg.

Supported:
  GET  /api/manager/devices/device/        live state (changes with PUTs)
  GET  /api/manager/zones/zone/
  GET  /api/manager/flow/flow/
  GET  /api/manager/flow/advancedflow/
  PUT  /api/manager/devices/device/<id>/capability/<cap>   {"value": X}
  POST /api/manager/flow/{flow|advancedflow}/<id>/trigger
Anything without "Authorization: Bearer <key>" gets 401. Each request is
printed to stdout.
"""
import argparse
import http.server
import json
import os
import sys
import time

DEFAULT_KEY = "11111111-2222-3333-4444-555555555555:aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee:0123abcd"
HERE = os.path.dirname(os.path.abspath(__file__))

ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("--bind", default="127.0.0.1")
ap.add_argument("--port", type=int, default=18080)
ap.add_argument("--key", default=DEFAULT_KEY)
ap.add_argument("--fixtures", default=os.path.join(HERE, "fixtures"))
ap.add_argument("--delay", type=float, default=0.3, help="seconds a PUT takes (a real Homey is not instant)")
args = ap.parse_args()


def load(name):
    with open(os.path.join(args.fixtures, name), encoding="utf-8") as f:
        return json.load(f)


DEV = load("homey_devices.json")
STATIC = {
    "/api/manager/zones/zone/": load("homey_zones.json"),
    "/api/manager/flow/flow/": load("homey_flows.json"),
    "/api/manager/flow/advancedflow/": load("homey_advancedflows.json"),
}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class Handler(http.server.BaseHTTPRequestHandler):
    def _authed(self):
        if self.headers.get("Authorization") == "Bearer " + args.key:
            return True
        log(self.command, self.path, "-> 401 (wrong or missing key)")
        self._reply(401, {"error": "Unauthorized"})
        return False

    def _reply(self, code, obj=None):
        data = b"" if obj is None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        return self.rfile.read(n).decode("utf-8", "replace")

    def do_GET(self):
        if not self._authed():
            return
        if self.path == "/api/manager/devices/device/":
            return self._reply(200, DEV)
        if self.path in STATIC:
            return self._reply(200, STATIC[self.path])
        log("GET", self.path, "-> 404")
        self._reply(404, {"error": "Not Found"})

    def do_PUT(self):
        if not self._authed():
            return
        body = self._body()
        p = self.path.split("/")  # ['', api, manager, devices, device, <id>, capability, <cap>]
        if len(p) == 8 and p[1:5] == ["api", "manager", "devices", "device"] and p[6] == "capability":
            dev = DEV.get(p[5])
            cap = dev and dev.get("capabilitiesObj", {}).get(p[7])
            if cap is not None and cap.get("setable"):
                try:
                    value = json.loads(body)["value"]
                except (ValueError, KeyError, TypeError):
                    log("PUT", p[5], p[7], body, "-> 400")
                    return self._reply(400, {"error": "bad body"})
                time.sleep(args.delay)
                if cap.get("getable", True):
                    cap["value"] = value
                log("PUT", dev.get("name"), p[7], "=", json.dumps(value), "-> 200")
                return self._reply(200, {})
        log("PUT", self.path, body, "-> 404")
        self._reply(404, {"error": "Not Found"})

    def do_POST(self):
        if not self._authed():
            return
        body = self._body()
        p = self.path.split("/")  # ['', api, manager, flow, <kind>, <id>, trigger]
        if len(p) == 7 and p[1:4] == ["api", "manager", "flow"] and p[6] == "trigger":
            kind = "/api/manager/flow/%s/" % p[4]
            flows = STATIC.get(kind, {})
            if p[5] in flows:
                log("POST trigger", p[4], flows[p[5]].get("name"), "-> 204")
                return self._reply(204)
        log("POST", self.path, body, "-> 404")
        self._reply(404, {"error": "Not Found"})

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    srv = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)
    log("fake Homey on %s:%d (%d devices, key %s...)" % (args.bind, args.port, len(DEV), args.key[:8]))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        sys.exit(0)

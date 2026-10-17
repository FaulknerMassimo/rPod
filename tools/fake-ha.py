#!/usr/bin/env python3
"""A fake Home Assistant for the sim and the web UI: the REST API rPod uses
(src/ha/ha.h) -- /api/config, /api/states, /api/states/<id> and
/api/services/<domain>/<service> -- behind a bearer token, over a handful of
devices whose state the services really change.

    python3 tools/fake-ha.py [port] [token]      (8123, "rpod-test-token-0123456789")

Then, in the web UI, Settings > Home Assistant: http://localhost:8123 and the
token -- or for the sim alone:

    printf 'http://localhost:8123\nrpod-test-token-0123456789\n' > ~/.local/state/rpod-sim/homeassistant

Environment: FAKE_HA_DELAY=seconds holds every reply back (a slow server),
FAKE_HA_FAIL=1 answers every service call with a 500.
"""

import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8123
TOKEN = sys.argv[2] if len(sys.argv) > 2 else "rpod-test-token-0123456789"
DELAY = float(os.environ.get("FAKE_HA_DELAY", "0"))
FAIL = os.environ.get("FAKE_HA_FAIL") == "1"

LOCK = threading.Lock()


def ent(entity_id, state, **attrs):
    return {"entity_id": entity_id, "state": state, "attributes": attrs}


STATES = {
    e["entity_id"]: e
    for e in [
        ent("light.living_room", "on", friendly_name="Living Room", brightness=180,
            supported_color_modes=["color_temp", "xy"]),
        ent("light.kitchen", "off", friendly_name="Kitchen", brightness=None,
            supported_color_modes=["brightness"]),
        ent("light.hallway", "on", friendly_name="Hallway", supported_color_modes=["onoff"]),
        ent("light.bedroom", "unavailable", friendly_name="Bedroom"),
        ent("switch.coffee_machine", "off", friendly_name="Coffee Machine"),
        ent("switch.garden_pump", "on", friendly_name="Garden Pump"),
        ent("input_boolean.guest_mode", "off", friendly_name="Guest Mode"),
        ent("fan.office", "off", friendly_name="Office Fan"),
        ent("cover.garage_door", "closed", friendly_name="Garage Door"),
        ent("cover.living_blinds", "open", friendly_name="Living Room Blinds"),
        ent("lock.front_door", "locked", friendly_name="Front Door"),
        ent("climate.house", "heat", friendly_name="House", temperature=21,
            current_temperature=19.5, hvac_modes=["off", "heat", "cool"]),
        ent("scene.movie_night", "2026-10-01T20:00:00+00:00", friendly_name="Movie Night"),
        ent("scene.good_morning", "unknown", friendly_name="Good Morning"),
        ent("script.goodnight", "off", friendly_name="Goodnight"),
        ent("automation.porch_light", "on", friendly_name="Porch Light at Sunset"),
        ent("button.doorbell_test", "unknown", friendly_name="Test Doorbell"),
        ent("sensor.outside_temperature", "11.4", friendly_name="Outside Temperature",
            unit_of_measurement="°C"),
        ent("sensor.power", "412", friendly_name="Power", unit_of_measurement="W"),
        ent("binary_sensor.front_door_open", "off", friendly_name="Front Door Open"),
    ]
}


def service(domain, name, body):
    eid = body.get("entity_id")
    e = STATES.get(eid)
    if e is None or not eid.startswith(domain + "."):
        return False
    a = e["attributes"]
    if e["state"] == "unavailable":
        return False
    if name == "turn_on":
        if domain in ("scene", "script"):
            e["state"] = "on" if domain == "script" else e["state"]
            return True
        e["state"] = "on"
        if "brightness_pct" in body:
            a["brightness"] = round(body["brightness_pct"] * 255 / 100)
        elif domain == "light" and a.get("brightness") is None and "brightness" in a:
            a["brightness"] = 255
    elif name == "turn_off":
        e["state"] = "off"
        if domain == "light" and "brightness" in a:
            a["brightness"] = None
    elif name == "toggle":
        e["state"] = "off" if e["state"] == "on" else "on"
    elif name == "open_cover":
        e["state"] = "open"
    elif name == "close_cover":
        e["state"] = "closed"
    elif name == "lock":
        e["state"] = "locked"
    elif name == "unlock":
        e["state"] = "unlocked"
    elif name == "press":
        pass
    elif name == "set_temperature":
        a["temperature"] = body["temperature"]
    else:
        return False
    return True


class Handler(BaseHTTPRequestHandler):
    server_version = "FakeHA"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

    def send(self, code, payload):
        data = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def authed(self):
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            self.send(401, {"message": "401: Unauthorized"})
            return False
        return True

    def do_GET(self):
        if DELAY:
            time.sleep(DELAY)
        if not self.authed():
            return
        with LOCK:
            if self.path == "/api/":
                self.send(200, {"message": "API running."})
            elif self.path == "/api/config":
                self.send(200, {"location_name": "Fake Home", "version": "2026.10.0",
                                "unit_system": {"temperature": "°C"}})
            elif self.path == "/api/states":
                self.send(200, list(STATES.values()))
            elif self.path.startswith("/api/states/"):
                e = STATES.get(self.path[len("/api/states/"):])
                self.send(200, e) if e else self.send(404, {"message": "Entity not found."})
            else:
                self.send(404, {"message": "404: Not Found"})

    def do_POST(self):
        if DELAY:
            time.sleep(DELAY)
        if not self.authed():
            return
        n = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return self.send(400, {"message": "bad json"})
        parts = self.path.split("/")
        if len(parts) == 5 and parts[1] == "api" and parts[2] == "services":
            if FAIL:
                return self.send(500, {"message": "boom"})
            with LOCK:
                ok = service(parts[3], parts[4], body)
            sys.stderr.write("  service %s.%s %s -> %s\n" % (parts[3], parts[4], body, ok))
            return self.send(200 if ok else 400, [])
        self.send(404, {"message": "404: Not Found"})


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    sys.stderr.write("fake Home Assistant on http://127.0.0.1:%d, token %s\n" % (PORT, TOKEN))
    srv.serve_forever()

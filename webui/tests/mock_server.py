#!/usr/bin/env python3
"""Browser fixture for the repository-owned operator console."""

from __future__ import annotations

import gzip
import json
import pathlib
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[1]

TELEMETRY = {
    "schema": "tams.fluidnc.telemetry.v1",
    "sequence": 42,
    "machine": {"model": "Maijker XZACT Mini Lathe", "state": "Idle", "alarm": "None"},
    "execution": {
        "state": "READY",
        "coordinate_system": "G54",
        "distance_mode": "ABSOLUTE",
        "feed_mode": "UNITS_PER_MINUTE",
    },
    "positions": {
        "x": {"available": True, "machine": 12.345, "work": 2.345, "homed": True, "limit_active": False},
        "z": {"available": True, "machine": -45.67, "work": -5.67, "homed": True, "limit_active": False},
        "c": {"available": True, "machine": 0.0, "work": 0.0, "homed": False, "limit_active": False},
    },
    "spindle": {
        "shared_chuck": True,
        "mode": "IDLE",
        "state": "OFF",
        "commanded_rpm": 0,
        "measured_rpm": 84.2,
        "speed_mode": "FIXED_RPM",
        "diameter_mode": "DIAMETER",
        "encoder": {
            "configured": True,
            "capture_active": True,
            "pulses_per_revolution": 1000,
            "has_measured_rpm": True,
            "has_index": False,
            "has_angular_position": True,
            "angular_position_revolution": 0.387,
            "pulse_count": 2387,
            "index_count": 0,
            "revolution_count": 2,
            "last_index_pulses": 0,
            "last_pulse_age_ms": 8,
            "has_direction": True,
            "direction": "CW",
            "stale": False,
            "fault": False,
        },
    },
    "turret": {
        "configured": True,
        "station_count": 5,
        "current_station": 2,
        "target_station": 0,
        "software_position_known": True,
        "mechanically_confirmed": False,
        "sensor_configured": False,
        "last_error": "ok",
    },
    "assets": {
        "cutting_tools": [
            {
                "station": 2,
                "geometry_x_mm": 1.25,
                "geometry_z_mm": -0.5,
                "wear_x_mm": 0,
                "wear_z_mm": 0,
                "nose_radius_mm": 0.4,
                "orientation": 1,
            }
        ]
    },
}

SETTINGS = {
    "cmd": "400",
    "status": "ok",
    "data": [
        {"F": "Flash/Settings", "P": "Report/Status", "H": "Report/Status", "T": "I", "V": 1, "M": 0, "S": 3},
        {"F": "Flash/Settings", "P": "Config/Filename", "H": "Config/Filename", "T": "S", "V": "XZACt.yaml"},
        {
            "F": "Running/Config",
            "P": "/lathe/enable_threading",
            "H": "Enable threading",
            "T": "B",
            "V": "0",
            "O": [{"False": 0}, {"True": 1}],
        },
        {
            "F": "Running/Config",
            "P": "/axes/x/motor0/limit_neg_pin",
            "H": "X negative limit pin",
            "T": "P",
            "V": "gpio.36:low",
            "W": 0,
        },
        {
            "F": "Running/Config",
            "P": "/spindle/linearization",
            "H": "Spindle linearization",
            "T": "S",
            "V": "0=0.00% 1000=25.00% 4000=100.00%",
            "M": 0,
            "S": 255,
        },
    ],
}


class Handler(BaseHTTPRequestHandler):
    def send_json(self, value: object, status: int = 200, cookie: str | None = None) -> None:
        body = json.dumps(value, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if cookie:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/":
            import sys

            sys.path.insert(0, str(ROOT))
            import build_webui

            body = gzip.decompress(build_webui.build())
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if parsed.path == "/api/v1/console/session":
            self.send_json(
                {"locked": True, "csrf_token": "fixture", "scope": "browser_tab"},
                cookie="TAMSCONSOLE=fixture; HttpOnly; SameSite=Strict; Path=/",
            )
            return
        if parsed.path == "/api/v1/lathe/status":
            self.send_json(TELEMETRY)
            return
        if parsed.path == "/api/v1/settings":
            self.send_json(SETTINGS)
            return
        if parsed.path == "/files":
            self.send_json(
                {
                    "files": [{"name": "XZACt.yaml", "shortname": "XZACt.yaml", "size": 1234, "datetime": ""}],
                    "path": "",
                    "total": "192 KB",
                    "used": "64 KB",
                    "occupation": 33,
                    "status": "Ok",
                }
            )
            return
        if parsed.path == "/XZACt.yaml":
            body = b"name: XZA Lathe\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if parsed.path == "/api/v1/firmware/devices":
            self.send_json(
                {
                    "controller": {
                        "online": True,
                        "device_id": "fluidnc-fixture",
                        "version": "fixture",
                        "inactive_partition": "ota_1",
                    },
                    "m5dial": {
                        "paired": True,
                        "online": True,
                        "ambiguous": False,
                        "device_id": "fluiddial-fixture",
                        "fingerprint": "ab" * 32,
                        "ip": "192.0.2.5",
                        "hardware_role": "m5dial_hmi",
                        "version": "fixture",
                        "health": "healthy",
                    },
                    "trust_configured": True,
                    "safe": True,
                    "safety_reason": "read-only fixture",
                    "maintenance_lock": False,
                }
            )
            return
        if parsed.path == "/api/v1/firmware/receipts":
            self.send_json([])
            return
        self.send_json({"error": "not found"}, 404)

    def do_POST(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/v1/console/unlock":
            self.send_json({"locked": False, "control_token": "fixture-control"})
            return
        if parsed.path == "/api/v1/console/lock":
            self.send_json({"locked": True})
            return
        if parsed.path == "/files":
            length = int(self.headers.get("Content-Length", "0"))
            if length:
                self.rfile.read(length)
            self.send_json({"status": "Ok"})
            return
        if parsed.path.startswith("/api/v1/lathe/"):
            self.send_json({"status": "accepted"})
            return
        self.send_json({"error": "not implemented in fixture"}, 501)

    def do_PUT(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/v1/settings":
            self.send_json({"cmd": "401", "status": "ok", "message": "setting updated"})
            return
        self.send_json({"error": "not implemented in fixture"}, 501)

    def log_message(self, *_: object) -> None:
        pass


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", 18765), Handler).serve_forever()

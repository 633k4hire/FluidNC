#!/usr/bin/env python3
"""Read-only browser fixture for the repository-owned FluidNC Web Console."""

from __future__ import annotations

import gzip
import json
import pathlib
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[1]


def response_for_command(command: str) -> dict:
    if command == "[ESP421]":
        return {
            "cmd": "421",
            "status": "ok",
            "data": [
                {"id": "Lathe enabled", "value": "true"},
                {"id": "Threading enabled", "value": "false"},
            ],
        }
    if command == "[ESP426]":
        return {"ok": True, "mode": "IDLE", "message": "read-only fixture"}
    if command == "[ESP425]":
        return {
            "schema": "tams.fluidnc.telemetry.v1",
            "machine": {"state": "Idle", "alarm": "None"},
            "execution": {
                "state": "READY",
                "coordinate_system": "G54",
                "distance_mode": "ABSOLUTE",
                "feed_mode": "UNITS_PER_MINUTE",
            },
            "positions": {
                "x": {"available": True, "machine": 12.345, "homed": True, "limit_active": False},
                "z": {"available": True, "machine": -45.67, "homed": True, "limit_active": False},
                "c": {"available": True, "machine": 0.0, "homed": True, "limit_active": False},
            },
            "spindle": {
                "shared_chuck": True,
                "mode": "IDLE",
                "state": "OFF",
                "commanded_rpm": 0,
                "measured_rpm": None,
                "speed_mode": "FIXED_RPM",
                "diameter_mode": "DIAMETER",
                "encoder": {
                    "configured": False,
                    "has_index": False,
                    "angular_position_revolution": None,
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
                "sensor_configured": False,
                "last_error": "ok",
            },
        }
    return {"error": "unsupported read-only fixture command"}


class Handler(BaseHTTPRequestHandler):
    def send_json(self, value: object, status: int = 200) -> None:
        body = json.dumps(value, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
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
        if parsed.path == "/login":
            self.send_json({"status": "Ok", "authentication_lvl": "admin", "user": "admin"})
            return
        if parsed.path == "/command":
            command = urllib.parse.parse_qs(parsed.query).get("cmd", [""])[0]
            self.send_json(response_for_command(command))
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
                    "admin_password_hardened": True,
                    "safe": True,
                    "safety_reason": "read-only fixture",
                    "csrf_token": "fixture",
                    "maintenance_lock": False,
                }
            )
            return
        if parsed.path == "/api/v1/firmware/receipts":
            self.send_json([])
            return
        self.send_json({"error": "not found"}, 404)

    def log_message(self, *_: object) -> None:
        pass


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", 18765), Handler).serve_forever()

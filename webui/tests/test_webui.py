from __future__ import annotations

import gzip
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SERVER = ROOT.parent / "FluidNC" / "src" / "WebUI" / "WebUIServer.cpp"
CLIENT = ROOT.parent / "FluidNC" / "src" / "WebUI" / "DialFirmwareClient.cpp"
PROTOCOL = ROOT.parent / "FluidNC" / "src" / "Protocol.cpp"


class WebUiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import sys
        sys.path.insert(0, str(ROOT))
        import build_webui

        cls.payload = build_webui.build()
        cls.html = gzip.decompress(cls.payload).decode("utf-8")
        cls.server = SERVER.read_text(encoding="utf-8")
        cls.client = CLIENT.read_text(encoding="utf-8")
        cls.protocol = PROTOCOL.read_text(encoding="utf-8")

    def test_is_small_enough_for_littlefs_budget(self):
        self.assertLess(len(self.payload), 100_000)
        data_dir = ROOT.parent / "FluidNC" / "data"
        legacy = data_dir / "index-legacy.html.gz"
        if legacy.exists():
            static_bytes = sum(path.stat().st_size for path in data_dir.iterdir() if path.is_file())
            self.assertLess(static_bytes + (16 * 2048), 196_608)

    def test_all_required_surfaces_are_present(self):
        for text in ("Dashboard", "Controls", "Tooling", "Firmware", "Files", "Diagnostics",
                     "FluidNC Controller", "M5Dial / FluidDial", "Five-position turret",
                     "C / Spindle", "Encoder / threading"):
            self.assertIn(text, self.html)

    def test_lathe_capability_and_shared_chuck_queries_are_used(self):
        self.assertIn("[ESP421]", self.html)
        self.assertIn("[ESP426]", self.html)
        self.assertIn("[ESP425]", self.html)

    def test_machine_controls_use_typed_api_not_command_console(self):
        self.assertIn("/api/v1/lathe/${type}", self.html)
        self.assertIn("/api/v1/lathe/turret/confirm", self.html)
        self.assertNotIn("M61Q", self.html)
        self.assertNotIn("$J=", self.html)

    def test_firmware_uses_dedicated_api(self):
        self.assertIn("/api/v1/firmware/packages/validate", self.html)
        self.assertIn("/api/v1/firmware/deployments", self.html)
        self.assertNotIn("/updatefw", self.html)

    def test_pairing_and_progress_surfaces_are_present(self):
        self.assertIn("Administrator sign in", self.html)
        self.assertIn("/api/v1/firmware/pair/start", self.html)
        self.assertIn("press green on the m5dial", self.html.lower())
        for stage in ("Target verification", "Image verification", "Reconnect", "Receipt"):
            self.assertIn(stage, self.html)

    def test_typed_lathe_routes_have_server_side_gates(self):
        for route in ("jog", "spindle", "spindle-stop", "turret", "turret/confirm",
                      "chuck", "tool", "touch-off"):
            self.assertIn(f'/api/v1/lathe/{route}', self.server)
        self.assertIn("firmwareMutationAuthorized(request, false)", self.server)
        self.assertIn("latheControlSafety", self.server)
        self.assertIn("firmware maintenance lock rejects machine-control", self.server)
        self.assertIn('Lathe::enabled() ? "index.html" : "index-legacy.html"', self.server)

    def test_maijker_authentication_is_request_scoped(self):
        self.assertIn("is_authenticated(AsyncWebServerRequest* request)", self.server)
        self.assertNotIn("_webserver->hasArg", self.server)
        self.assertIn("HttpOnly; SameSite=Strict; Path=/", self.server)
        self.assertIn("create_csrf_token", self.server)
        self.assertIn("authentication_admin_password_is_default", self.server)
        self.assertIn("New password", self.html)

    def test_dial_relay_is_bounded_and_identity_authenticated(self):
        self.assertIn("FirmwareRelayChunkSize  = 4096", self.server)
        self.assertIn("length > 8192", self.client)
        self.assertIn("authenticated M5Dial identity mismatch", self.client)
        self.assertIn("M5Dial response authentication failed", self.client)
        self.assertIn("M5Dial rolled back to the previous application", self.client)

    def test_receipts_are_capped_and_persisted(self):
        self.assertIn("firmware-receipts.jsonl", self.server)
        self.assertIn("firmwareReceipts.size() > 16", self.server)
        self.assertIn("stdfs::rename", self.server)
        self.assertIn("firmware-receipts.bak", self.server)
        self.assertIn("FirmwareReceiptMaxBytes = 2048", self.server)

    def test_maintenance_lock_reaches_all_line_oriented_channels(self):
        self.assertIn("firmwareMaintenanceActive()", self.protocol)
        self.assertIn("firmwareCommandAllowedDuringMaintenance", self.protocol)
        self.assertIn("Error::AnotherInterfaceBusy", self.protocol)


if __name__ == "__main__":
    unittest.main()

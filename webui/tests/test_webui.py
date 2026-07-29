from __future__ import annotations

import gzip
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SERVER = ROOT.parent / "FluidNC" / "src" / "WebUI" / "WebUIServer.cpp"
CLIENT = ROOT.parent / "FluidNC" / "src" / "WebUI" / "DialFirmwareClient.cpp"
PROTOCOL = ROOT.parent / "FluidNC" / "src" / "Protocol.cpp"
JSON_GENERATOR = ROOT.parent / "FluidNC" / "src" / "Configuration" / "JsonGenerator.cpp"


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
        cls.json_generator = JSON_GENERATOR.read_text(encoding="utf-8")

    def test_is_small_enough_for_littlefs_budget(self):
        self.assertLess(len(self.payload), 100_000)
        data_dir = ROOT.parent / "FluidNC" / "data"
        legacy = data_dir / "index-legacy.html.gz"
        if legacy.exists():
            static_bytes = sum(path.stat().st_size for path in data_dir.iterdir() if path.is_file())
            self.assertLess(static_bytes + (16 * 2048), 196_608)

    def test_operator_surfaces_are_present(self):
        for text in (
            "Dashboard",
            "Controls",
            "Tooling",
            "Settings",
            "Firmware",
            "Files",
            "Diagnostics",
            "X / Z Jog",
            "Spindle / C",
            "Flash Settings",
            "Config Items",
            "Five-position Turret",
        ):
            self.assertIn(text, self.html)

    def test_radial_jog_and_visual_spindle_are_distinct(self):
        self.assertIn('class="radial-jog"', self.html)
        self.assertIn('data-axis="X"', self.html)
        self.assertIn('data-axis="Z"', self.html)
        self.assertIn('class="rpm-gauge"', self.html)
        self.assertIn('id="spindle-slider"', self.html)
        self.assertIn('id="c-motion-panel"', self.html)

    def test_status_and_machine_controls_use_typed_apis(self):
        self.assertIn("/api/v1/lathe/status", self.html)
        self.assertIn("/api/v1/lathe/${type}", self.html)
        self.assertNotIn("/command?cmd=", self.html)
        self.assertNotIn("$J=", self.html)
        self.assertNotIn("legacy-command", self.html)

    def test_operator_flow_has_no_accounts_or_confirmation_dialogs(self):
        for obsolete in (
            "Administrator sign in",
            "Sign in",
            "login-dialog",
            "confirm-dialog",
            "confirmAction",
            "window.confirm",
            "alert(",
            "prompt(",
        ):
            self.assertNotIn(obsolete, self.html)
        self.assertIn("/api/v1/console/session", self.html)
        self.assertIn("/api/v1/console/unlock", self.html)
        self.assertIn("/api/v1/console/lock", self.html)
        self.assertIn("TAMSCONSOLE=", self.server)
        self.assertIn("X-TAMS-Control-Token", self.server)
        self.assertIn("HttpOnly; SameSite=Strict; Path=/", self.server)
        self.assertIn("std::array<std::string, 8> controls", self.server)
        self.assertIn("revokeConsoleControl", self.server)

    def test_browser_lock_and_theme_are_local_operator_controls(self):
        self.assertIn('id="lock-toggle"', self.html)
        self.assertIn('id="theme-toggle"', self.html)
        self.assertIn('localStorage.setItem("xza-theme"', self.html)
        self.assertIn('state.control=""', self.html)
        self.assertIn('jsonFetch("/api/v1/console/lock",{method:"POST",headers:writeHeaders()})', self.html)
        self.assertNotIn('sessionStorage.setItem("tams-csrf"', self.html)
        self.assertNotIn("setTimeout(lock", self.html)

    def test_settings_and_files_use_existing_typed_surfaces(self):
        self.assertIn("/api/v1/settings", self.html)
        self.assertIn("Object.entries(option)[0]", self.html)
        self.assertIn("Save to DLC32", self.html)
        self.assertIn("Saved on DLC32", self.html)
        self.assertIn("[ESP400]json=yes", self.server)
        self.assertIn("[ESP401]P=", self.server)
        self.assertIn('action=delete&path=/', self.html)
        self.assertIn("FormData()", self.html)
        self.assertIn('_encoder.begin_webui(_currentPath, "P", value.name())', self.json_generator)
        self.assertIn('_encoder.member("W", int32_t(0))', self.json_generator)
        self.assertIn("std::vector<speedEntry>& value", self.json_generator)
        self.assertIn("std::vector<float>& value", self.json_generator)

    def test_firmware_uses_dedicated_signed_api(self):
        self.assertIn("/api/v1/firmware/packages/validate", self.html)
        self.assertIn("/api/v1/firmware/deployments", self.html)
        self.assertNotIn("/updatefw", self.html)

    def test_pairing_uses_inline_code_and_center_button(self):
        self.assertIn("/api/v1/firmware/pair/start", self.html)
        self.assertIn("center dial button", self.html.lower())
        self.assertIn("press the M5Dial center button", self.server)
        for stage in ("Target verification", "Image verification", "Reconnect", "Receipt"):
            self.assertIn(stage, self.html)

    def test_typed_routes_have_server_side_gates(self):
        for route in (
            "status",
            "home",
            "jog",
            "jog-cancel",
            "spindle",
            "spindle-stop",
            "turret",
            "turret/confirm",
            "chuck",
            "tool",
            "touch-off",
            "coordinate-system",
        ):
            self.assertIn(f"/api/v1/lathe/{route}", self.server)
        self.assertIn("consoleControlAuthorized(request)", self.server)
        self.assertNotIn("latheControlSafety", self.server)
        self.assertNotIn("latheHomeSafety", self.server)
        self.assertIn('strncpy(command, "$H=XZ"', self.server)
        self.assertIn("firmware maintenance lock rejects machine-control", self.server)
        self.assertIn('Lathe::enabled() ? "index.html" : "index-legacy.html"', self.server)

    def test_work_coordinates_are_explicit_and_selectable(self):
        self.assertIn('id="coordinate-system"', self.html)
        self.assertIn('id="apply-coordinate-system"', self.html)
        self.assertIn('typedAction("coordinate-system"', self.html)
        self.assertIn("G59.3", self.html)
        self.assertIn("invalid work coordinate system", self.server)

    def test_typed_post_commands_enter_the_command_channel(self):
        function = self.server.split("void WebUI_Server::synchronousCommand(", 1)[1].split(
            "std::string getSession", 1
        )[0]
        self.assertIn("executeCommandBackground(line)", function)
        self.assertNotIn("request->method() == HTTP_GET", function)
        self.assertIn("typed command exceeds FluidNC line capacity", function)

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

# Maijker FluidNC Web Console

This directory is the source of the repository-owned `fluidnc.local` interface.
It deliberately avoids a Node toolchain: `build_webui.py` combines the HTML,
CSS, and JavaScript and writes deterministic gzip output to
`FluidNC/data/index.html.gz`.

Build and check the LittleFS budget:

```powershell
python webui\build_webui.py
python webui\tests\test_webui.py
```

The server selects the Maijker console only when `Lathe::enabled()` is true.
Other machines receive the pinned original generic WebUI from
`FluidNC/data/index-legacy.html.gz`. The build script reproduces that exact blob
from its pinned Git object instead of treating generated gzip as editable
source. All lathe control buttons call typed, server-gated APIs; firmware
deployment does not use the command console.

The Settings page is backed by FluidNC's ESP400/ESP401 metadata contract. It
renders the persistent flash settings and the live configuration tree,
including scalar values, enumerations, IP addresses, macros, speed maps, float
tables, and current pin assignments. Runtime-supported values have direct Set
controls. Pins are visible but deliberately marked for YAML editing because
FluidNC's runtime setting layer cannot replace initialized pin objects. The
Files page reads and writes the controller's real LittleFS files, so the YAML
editor is the persistent route for pin assignments and other structured
configuration.

## Secure M5Dial deployment

The Firmware page validates a complete signed `.tamsfw` envelope, then relays
only its application bytes to the exact cryptographically paired M5Dial in
4 KiB chunks. FluidNC never retains the complete dial image. Firmware mutations
require the current browser tab's operator-unlock token, the same-origin CSRF
token, a current machine-safe predicate, and the exclusive maintenance lock.
There are no usernames, passwords, reauthentication prompts, or browser
confirmation dialogs in the Maijker console. Each page load starts Locked;
unlock tokens live only in JavaScript memory and are independently revocable
per tab. Normal lathe actions remain subject to FluidNC's native state, limit,
alarm, typed-command, and hardware checks.

Pairing opens a bounded pairing window in the normally running FluidDial
application. FluidNC and FluidDial derive a shared secret with ephemeral P-256
ECDH, display the same six-digit code, and do not persist the pair until the
operator presses the M5Dial center control. mDNS is discovery only;
authenticated health must prove the stored device ID and identity-key
fingerprint.

Production release builds must inject `TAMS_FW_PRODUCTION_KEY_ID` and
`TAMS_FW_PRODUCTION_KEY_PEM`. Recovery-key macros are separate. No private key
belongs in this repository. With no production trust root configured, package
validation fails closed.

Receipts are kept as an atomic, capped 16-entry JSONL file on LittleFS and are
downloadable from the Firmware page. The controller revalidates authenticated
identity and health after reboot and records successful, failed, aborted, or
rolled-back outcomes. `test_webui.py` enforces the generated-UI and full
LittleFS budgets instead of documenting a stale byte count.

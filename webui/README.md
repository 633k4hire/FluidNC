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

## Secure M5Dial deployment

The Firmware page validates a complete signed `.tamsfw` envelope, then relays
only its application bytes to the exact cryptographically paired M5Dial in
4 KiB chunks. FluidNC never retains the complete dial image. Firmware mutations
require an administrator session, CSRF token, recent password authentication,
a current machine-safe predicate, and the exclusive maintenance lock.
The built-in administrator password must be replaced with a 12-16 character
password before firmware mutations or typed lathe controls are accepted.

Pairing starts only while the operator has opened the M5Dial OTA scene. FluidNC
and FluidDial derive a shared secret with ephemeral P-256 ECDH, display the same
six-digit code, and do not persist the pair until the operator confirms it on
the dial. mDNS is discovery only; authenticated health must prove the stored
device ID and identity-key fingerprint.

Production release builds must inject `TAMS_FW_PRODUCTION_KEY_ID` and
`TAMS_FW_PRODUCTION_KEY_PEM`. Recovery-key macros are separate. No private key
belongs in this repository. With no production trust root configured, package
validation fails closed.

Receipts are kept as an atomic, capped 16-entry JSONL file on LittleFS and are
downloadable from the Firmware page. The controller revalidates authenticated
identity and health after reboot and records successful, failed, aborted, or
rolled-back outcomes. The current generated lathe UI is 12,438 bytes gzip; with
the 120,268-byte generic UI and other static data, the 192 KiB partition retains
20,332 bytes after reserving 32 KiB for receipts.

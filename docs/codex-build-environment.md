# Codex Build Environment

This repository can be built from Codex Desktop with the workstation's single
approved PlatformIO installation at `C:\Users\metzg\.platformio`. The helper
scripts in `tools/codex` configure that exact core and reject a repo-local
`.pio-core`.

Run once:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\codex\Setup-CodexEnv.ps1
```

For each new terminal session:

```powershell
. .\tools\codex\Enter-CodexEnv.ps1
```

The setup script installs:

- a repo-local `w64devkit` GCC toolchain under `.codex-tools/`
- embedded WebUI npm dependencies under `embedded/node_modules/`

It does not install, upgrade, or create PlatformIO. Platforms, frameworks,
packages, compilers, and caches used by PlatformIO remain under the single
approved `C:\Users\metzg\.platformio` core.

Useful verification commands after entering the environment:

```powershell
pio test -e tests
pio run -e wifi
pio run -e wifi_s3
python .\embedded\build.py
```

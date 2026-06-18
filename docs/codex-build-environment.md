# Codex Build Environment

This repository can be built from Codex Desktop without relying on global
PlatformIO state by using the repo-local setup scripts in `tools/codex`.

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
- repo-local PlatformIO package/cache state under `.pio-core/`
- embedded WebUI npm dependencies under `embedded/node_modules/`

Useful verification commands after entering the environment:

```powershell
pio test -e tests
pio run -e wifi
pio run -e wifi_s3
python .\embedded\build.py
```

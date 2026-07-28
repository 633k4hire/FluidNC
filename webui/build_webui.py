#!/usr/bin/env python3
"""Build the deterministic, self-contained Maijker FluidNC WebUI."""

from __future__ import annotations

import gzip
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "src"
OUTPUT = ROOT.parent / "FluidNC" / "data" / "index.html.gz"
LEGACY_OUTPUT = ROOT.parent / "FluidNC" / "data" / "index-legacy.html.gz"
# Exact generic ESP3D WebUI blob from the pinned FluidNC base. It remains the
# fallback for non-lathe configurations; the repository-owned source below
# builds only the Maijker lathe console.
LEGACY_BLOB = "92ff96661c07e1b906120eb2672fed643bf4fabd"


def build() -> bytes:
    html = (SOURCE / "index.html").read_text(encoding="utf-8")
    css = (SOURCE / "app.css").read_text(encoding="utf-8")
    js = (SOURCE / "app.js").read_text(encoding="utf-8")
    if html.count("/*__APP_CSS__*/") != 1 or html.count("/*__APP_JS__*/") != 1:
        raise ValueError("index.html must contain each build marker exactly once")
    combined = html.replace("/*__APP_CSS__*/", css).replace("/*__APP_JS__*/", js)
    return gzip.compress(combined.encode("utf-8"), compresslevel=9, mtime=0)


def main() -> int:
    payload = build()
    OUTPUT.write_bytes(payload)
    print(f"wrote {OUTPUT} ({len(payload)} bytes gzip)")
    legacy = subprocess.check_output(
        ["git", "cat-file", "blob", LEGACY_BLOB], cwd=ROOT.parent
    )
    LEGACY_OUTPUT.write_bytes(legacy)
    print(f"wrote {LEGACY_OUTPUT} ({len(legacy)} bytes pinned generic WebUI)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

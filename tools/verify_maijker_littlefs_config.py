#!/usr/bin/env python3
"""Fail if the deployed Maijker configuration copies diverge."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONFIG_PATHS = (
    ROOT / "FluidNC" / "data" / "config.yaml",
    ROOT / "FluidNC" / "data" / "XZACt.yaml",
    ROOT / "example_configs" / "maijker_xzact_mini_lathe.yaml",
)


def main() -> None:
    payloads = {path: path.read_bytes() for path in CONFIG_PATHS}
    canonical = payloads[CONFIG_PATHS[0]]

    mismatches = [str(path.relative_to(ROOT)) for path, data in payloads.items() if data != canonical]
    if mismatches:
        raise SystemExit(
            "Maijker configuration copies are not byte-identical: "
            + ", ".join(mismatches)
        )

    required = (
        b"board: MKS-DLC32 V2.1\n",
        b"name: XZACt_MiniLathe\n",
        b"  shared_chuck: true\n",
        b"  station_count: 5\n",
    )
    missing = [value.decode().strip() for value in required if value not in canonical]
    if missing:
        raise SystemExit("Maijker configuration is missing required deployment values: " + ", ".join(missing))

    print("Maijker LittleFS configuration aliases verified.")


if __name__ == "__main__":
    main()

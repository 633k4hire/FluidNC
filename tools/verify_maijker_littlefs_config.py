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
    # Text mode normalizes CRLF/LF so validation works on Windows and CI.
    payloads = {path: path.read_text(encoding="utf-8") for path in CONFIG_PATHS}
    canonical = payloads[CONFIG_PATHS[0]]

    mismatches = [str(path.relative_to(ROOT)) for path, data in payloads.items() if data != canonical]
    if mismatches:
        raise SystemExit(
            "Maijker configuration copies are not byte-identical: "
            + ", ".join(mismatches)
        )

    required = (
        "board: MKS-DLC32 V2.1\n",
        "name: XZACt_MiniLathe\n",
        "  shared_chuck: true\n",
        "  station_count: 5\n",
    )
    missing = [value.strip() for value in required if value not in canonical]
    if missing:
        raise SystemExit("Maijker configuration is missing required deployment values: " + ", ".join(missing))

    expected_counts = {
        "    steps_per_mm: 640\n": 2,
        "    steps_per_mm: 4.444444\n": 1,
    }
    wrong_counts = [
        f"{value.strip()} (expected {expected}, found {canonical.count(value)})"
        for value, expected in expected_counts.items()
        if canonical.count(value) != expected
    ]
    if wrong_counts:
        raise SystemExit("Maijker configuration has incorrect axis scaling: " + ", ".join(wrong_counts))

    motion_profile = (
        "    max_rate_mm_per_min: 243000\n",
        "    acceleration_mm_per_sec2: 9000\n",
        "  minimum_rpm: 50.0\n",
        "  maximum_rpm: 675.0\n",
        "  acceleration_rpm_per_sec: 1500.0\n",
        "  deceleration_rpm_per_sec: 100.0\n",
    )
    missing_profile = [value.strip() for value in motion_profile if value not in canonical]
    if missing_profile:
        raise SystemExit("Maijker configuration has an incomplete C motion profile: " + ", ".join(missing_profile))

    print("Maijker LittleFS configuration aliases verified.")


if __name__ == "__main__":
    main()

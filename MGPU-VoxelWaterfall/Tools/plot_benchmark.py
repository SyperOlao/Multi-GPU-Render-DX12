#!/usr/bin/env python3
"""Build article tables from MGPU-VoxelWaterfall benchmark artifacts."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("status,reason\nNOT_MEASURED,no valid paired rows\n", encoding="utf-8")
        return
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def build_tables(root: Path, output: Path) -> int:
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("status") == "BLOCKED":
        write_csv(output / "summary_table.csv", [])
        return 3

    paired = read_csv(root / "paired_runs.csv")
    valid = [row for row in paired if row.get("valid") == "true"]
    rows: list[dict[str, object]] = []
    for row in valid:
        rows.append(
            {
                "session_id": row.get("session_id", ""),
                "pair_id": row.get("pair_id", ""),
                "block_id": row.get("block_id", ""),
                "repetition": row.get("repetition", ""),
                "single_mode": row.get("single_mode", ""),
                "multi_mode": row.get("multi_mode", ""),
                "single_mean_cpu_frame_ms": row.get("single_mean_cpu_frame_ms", ""),
                "multi_mean_cpu_frame_ms": row.get("multi_mean_cpu_frame_ms", ""),
                "paired_difference_ms": row.get("paired_difference_ms", ""),
                "speedup": row.get("speedup", ""),
                "two_device_nominal_efficiency": row.get("two_device_nominal_efficiency", ""),
            }
        )
    write_csv(output / "paired_runs_table.csv", rows)
    return 0 if rows else 2


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    return build_tables(args.input, args.output)


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Build article tables only from hostile analysis_summary.v2.json."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_verified_summary(root: Path) -> dict:
    summary_path = root / "analysis_summary.v2.json"
    checksum_path = root / "analysis_summary.v2.sha256"
    if not summary_path.exists():
        raise RuntimeError("missing analysis_summary.v2.json; run analyze_benchmark.py first")
    if not checksum_path.exists():
        raise RuntimeError("missing analysis_summary.v2.sha256")
    expected = checksum_path.read_text(encoding="utf-8").split()[0]
    actual = sha256_file(summary_path)
    if expected != actual:
        raise RuntimeError("analysis_summary.v2.json checksum mismatch")
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    if summary.get("schema") != "mgpu_voxel_analysis_summary.v2":
        raise RuntimeError("analysis summary schema mismatch")
    if summary.get("status") != "PASS":
        raise RuntimeError("analysis summary is not PASS")
    return summary


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("status,reason\nNOT_MEASURED,no valid hostile-analysis pairs\n", encoding="utf-8")
        return
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def build_tables(root: Path, output: Path) -> int:
    summary = load_verified_summary(root)
    rows: list[dict[str, object]] = []
    for row in summary.get("recomputed_pairs", []):
        primary = row.get("endpoint_metrics", {}).get(summary.get("primary_endpoint", "present_to_present_ms"), {})
        rows.append({
            "session_id": row.get("session_id", ""),
            "pair_id": row.get("pair_id", ""),
            "block_id": row.get("block_id", ""),
            "repetition": row.get("repetition", ""),
            "single_config_id": row.get("single_config_id", ""),
            "multi_config_id": row.get("multi_config_id", ""),
            "single_mode": row.get("single_mode", ""),
            "multi_mode": row.get("multi_mode", ""),
            "endpoint": summary.get("primary_endpoint", "present_to_present_ms"),
            "single_mean_ms": row.get("single_mean_ms", primary.get("single", "")),
            "multi_mean_ms": row.get("multi_mean_ms", primary.get("multi", "")),
            "paired_difference_ms": row.get("paired_difference_ms", ""),
            "log_speedup": row.get("log_speedup", ""),
            "speedup": row.get("speedup", ""),
        })
    write_csv(output / "paired_runs_table.csv", rows)
    stats = summary.get("statistics", {})
    write_csv(output / "summary_table.csv", [{
        "run_id": summary.get("run_id", ""),
        "suite": summary.get("suite", ""),
        "primary_endpoint": summary.get("primary_endpoint", ""),
        "pair_count": stats.get("pair_count", 0),
        "difference_mean_ms": stats.get("difference_ms_ci95", {}).get("mean"),
        "difference_lower_ms": stats.get("difference_ms_ci95", {}).get("lower"),
        "difference_upper_ms": stats.get("difference_ms_ci95", {}).get("upper"),
        "log_speedup_mean": stats.get("log_speedup_ci95", {}).get("mean"),
        "log_speedup_lower": stats.get("log_speedup_ci95", {}).get("lower"),
        "log_speedup_upper": stats.get("log_speedup_ci95", {}).get("upper"),
        "speedup_mean": stats.get("speedup_ci95", {}).get("mean"),
        "speedup_lower": stats.get("speedup_ci95", {}).get("lower"),
        "speedup_upper": stats.get("speedup_ci95", {}).get("upper"),
        "ci_status": stats.get("speedup_ci95", {}).get("status"),
    }])
    return 0 if rows else 2


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        return build_tables(args.input, args.output)
    except Exception as exc:
        print(f"plot_status=FAIL reason={exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

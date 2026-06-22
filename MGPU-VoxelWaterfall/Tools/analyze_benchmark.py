#!/usr/bin/env python3
"""Offline benchmark artifact validator for MGPU-VoxelWaterfall."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
import tempfile
import unittest
from pathlib import Path


T_CRITICAL_95 = {
    1: 12.706204736,
    2: 4.302652730,
    3: 3.182446305,
    4: 2.776445105,
    5: 2.570581836,
    6: 2.446911851,
    7: 2.364624252,
    8: 2.306004135,
    9: 2.262157163,
    10: 2.228138852,
}


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = p * (len(ordered) - 1)
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return ordered[lo]
    frac = pos - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def mean(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def stddev(values: list[float]) -> float:
    return statistics.stdev(values) if len(values) > 1 else 0.0


def student_t_ci95_half_width(values: list[float]) -> float:
    if len(values) < 2:
        return 0.0
    df = len(values) - 1
    t = T_CRITICAL_95.get(df, 1.959963985)
    return t * stddev(values) / math.sqrt(len(values))


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def load_artifacts(root: Path) -> tuple[dict, dict, list[dict[str, str]], list[dict[str, str]], list[dict[str, str]]]:
    manifest_path = root / "manifest.json"
    environment_path = root / "environment.json"
    runs_path = root / "runs.csv"
    raw_path = root / "raw_frames.csv"
    invalid_path = root / "invalid_records.csv"
    visual_json_path = root / "voxel_visual_validation.json"
    visual_csv_path = root / "voxel_visual_validation.csv"
    preflight_path = root / "two_adapter_preflight.json"

    for path in (
        manifest_path,
        environment_path,
        runs_path,
        raw_path,
        invalid_path,
        visual_json_path,
        visual_csv_path,
        preflight_path,
    ):
        require(path.exists(), f"missing artifact: {path}")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    environment = json.loads(environment_path.read_text(encoding="utf-8"))
    runs = read_csv(runs_path)
    raw = read_csv(raw_path)
    invalid = read_csv(invalid_path)
    return manifest, environment, runs, raw, invalid


def aggregate_runs(runs: list[dict[str, str]]) -> list[dict[str, object]]:
    valid_runs = [row for row in runs if row.get("valid") == "true"]
    groups: dict[tuple[str, str, str], list[dict[str, str]]] = {}
    for row in valid_runs:
        key = (row["pair_id"], row["requested_mode"], row["actual_mode"])
        groups.setdefault(key, []).append(row)

    aggregates: list[dict[str, object]] = []
    for (pair_id, requested_mode, actual_mode), rows in sorted(groups.items()):
        cpu = [float(row["mean_cpu_frame_ms"]) for row in rows]
        critical = [float(row["critical_path_gpu_ms"]) for row in rows]
        aggregates.append(
            {
                "pair_id": pair_id,
                "requested_mode": requested_mode,
                "actual_mode": actual_mode,
                "run_count": len(rows),
                "mean_cpu_frame_ms": mean(cpu),
                "median_cpu_frame_ms": statistics.median(cpu),
                "stddev_cpu_frame_ms": stddev(cpu),
                "p95_cpu_frame_ms": percentile(cpu, 0.95),
                "p99_cpu_frame_ms": percentile(cpu, 0.99),
                "min_cpu_frame_ms": min(cpu),
                "max_cpu_frame_ms": max(cpu),
                "coefficient_of_variation": stddev(cpu) / mean(cpu) if mean(cpu) > 0.0 else 0.0,
                "cpu_frame_ci95_half_width_ms": student_t_ci95_half_width(cpu),
                "critical_path_gpu_ms": mean(critical),
            }
        )

    by_pair_mode = {(row["pair_id"], row["requested_mode"]): row for row in aggregates}
    for row in aggregates:
        mode = row["requested_mode"]
        if mode == "MultiGpuFull":
            baseline = by_pair_mode.get((row["pair_id"], "SingleGpuFull"))
        elif mode == "MultiGpuTemporalDecimation":
            baseline = by_pair_mode.get((row["pair_id"], "SingleGpuTemporalDecimation"))
        else:
            baseline = row
        if baseline and row["mean_cpu_frame_ms"]:
            row["paired_single_multi_difference_ms"] = baseline["mean_cpu_frame_ms"] - row["mean_cpu_frame_ms"]
            row["paired_speedup"] = baseline["mean_cpu_frame_ms"] / row["mean_cpu_frame_ms"]
            row["two_device_nominal_efficiency"] = row["paired_speedup"] / 2.0
        else:
            row["paired_single_multi_difference_ms"] = None
            row["paired_speedup"] = None
            row["two_device_nominal_efficiency"] = None
    return aggregates


def validate(root: Path) -> dict[str, object]:
    manifest, environment, runs, raw, invalid = load_artifacts(root)
    require(manifest.get("schema") == "mgpu_voxel_benchmark_manifest.v1", "manifest schema mismatch")
    require(environment.get("schema") == "mgpu_voxel_environment.v1", "environment schema mismatch")
    require(environment.get("run_id") == manifest.get("run_id"), "environment run_id does not match manifest")

    if manifest.get("status") == "BLOCKED":
        require(invalid, "blocked suite must include invalid_records.csv reason")
        return {
            "status": "BLOCKED",
            "reason": manifest.get("reason", ""),
            "aggregates": [],
            "invalid_count": len(invalid),
        }

    require(runs, "non-blocked suite has no runs.csv rows")
    require(raw, "non-blocked suite has no raw_frames.csv rows")
    aggregates = aggregate_runs(runs)
    require(aggregates, "non-blocked suite has no valid aggregate runs")
    return {
        "status": "PASS",
        "reason": "",
        "aggregates": aggregates,
        "invalid_count": len(invalid),
    }


class AnalysisTests(unittest.TestCase):
    def test_student_t_df2(self) -> None:
        values = [10.0, 12.0, 14.0]
        self.assertAlmostEqual(student_t_ci95_half_width(values), 4.302652730 * 2.0 / math.sqrt(3), places=6)

    def test_percentile(self) -> None:
        self.assertAlmostEqual(percentile([1, 2, 3, 4], 0.95), 3.85)
        self.assertEqual(percentile([7], 0.99), 7)

    def test_pairing_and_invalid(self) -> None:
        runs = [
            {"pair_id": "p", "requested_mode": "SingleGpuFull", "actual_mode": "SingleGpuFull", "valid": "true",
             "mean_cpu_frame_ms": "10", "critical_path_gpu_ms": "8"},
            {"pair_id": "p", "requested_mode": "MultiGpuFull", "actual_mode": "MultiGpuFull", "valid": "true",
             "mean_cpu_frame_ms": "5", "critical_path_gpu_ms": "4"},
            {"pair_id": "p", "requested_mode": "MultiGpuFull", "actual_mode": "Skipped", "valid": "false",
             "mean_cpu_frame_ms": "1", "critical_path_gpu_ms": "1"},
        ]
        aggregates = aggregate_runs(runs)
        multi = next(row for row in aggregates if row["requested_mode"] == "MultiGpuFull")
        self.assertEqual(multi["paired_speedup"], 2.0)
        self.assertEqual(multi["two_device_nominal_efficiency"], 1.0)

    def test_blocked_artifact_validation(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "manifest.json").write_text(
                json.dumps({"schema": "mgpu_voxel_benchmark_manifest.v1", "run_id": "r", "status": "BLOCKED",
                            "reason": "gate"}),
                encoding="utf-8",
            )
            (root / "environment.json").write_text(
                json.dumps({"schema": "mgpu_voxel_environment.v1", "run_id": "r"}),
                encoding="utf-8",
            )
            (root / "runs.csv").write_text("schema,suite,run_id\n", encoding="utf-8")
            (root / "raw_frames.csv").write_text("frame_index,suite\n", encoding="utf-8")
            (root / "invalid_records.csv").write_text(
                "schema,suite,run_id,pair_id,requested_mode,actual_mode,repetition,reason\n"
                "mgpu_voxel_invalid_records.v1,Smoke,r,suite_gate,,,0,gate\n",
                encoding="utf-8",
            )
            (root / "voxel_visual_validation.json").write_text("{}", encoding="utf-8")
            (root / "voxel_visual_validation.csv").write_text("case,status\n", encoding="utf-8")
            (root / "two_adapter_preflight.json").write_text("{}", encoding="utf-8")
            result = validate(root)
            self.assertEqual(result["status"], "BLOCKED")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, help="benchmark artifact directory")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(AnalysisTests)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if result.wasSuccessful() else 1

    if not args.input:
        parser.error("--input is required unless --self-test is used")

    try:
        result = validate(args.input)
    except Exception as exc:
        print(f"analysis_status=FAIL reason={exc}", file=sys.stderr)
        return 2

    output = args.input / "analysis_summary.json"
    output.write_text(json.dumps(result, indent=2, sort_keys=True), encoding="utf-8")
    print(f"analysis_status={result['status']} output={output}")
    return 0 if result["status"] == "PASS" else 3


if __name__ == "__main__":
    raise SystemExit(main())

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


def require_nonempty_evidence_file(root: Path, relative_path: str) -> None:
    require(relative_path and not Path(relative_path).is_absolute(), f"invalid evidence path: {relative_path!r}")
    path = root / relative_path
    require(path.exists(), f"missing evidence file: {path}")
    require(path.stat().st_size > 0, f"empty evidence file: {path}")
    if path.suffix.lower() == ".csv":
        require(read_csv(path), f"evidence CSV has no data rows: {path}")
    elif path.suffix.lower() == ".json":
        json.loads(path.read_text(encoding="utf-8"))


def validate_evidence_files(root: Path, manifest: dict) -> None:
    evidence_files = manifest.get("evidence_files", [])
    require(isinstance(evidence_files, list), "manifest evidence_files must be a list")
    require(evidence_files, "COMPLETE suite manifest must declare evidence_files")
    for relative_path in evidence_files:
        require(isinstance(relative_path, str), "manifest evidence_files entries must be strings")
        require_nonempty_evidence_file(root, relative_path)


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


def mode_family(mode: str) -> str | None:
    if mode in ("SingleGpuFull", "MultiGpuFull"):
        return "Full"
    if mode in ("SingleGpuTemporalDecimation", "MultiGpuTemporalDecimation"):
        return "Temporal"
    return None


def is_single(mode: str) -> bool:
    return mode in ("SingleGpuFull", "SingleGpuTemporalDecimation")


def is_multi(mode: str) -> bool:
    return mode in ("MultiGpuFull", "MultiGpuTemporalDecimation")


def count_key(row: dict[str, str]) -> tuple[str, str, str]:
    return (
        row.get("actual_total_count") or row.get("total_voxels", ""),
        row.get("actual_static_count") or row.get("actual_static_voxels", ""),
        row.get("actual_dynamic_count") or row.get("actual_dynamic_voxels", ""),
    )


def run_key(row: dict[str, str]) -> tuple[str, str, str, str, str, str, str]:
    return (
        row.get("session_id", ""),
        row.get("pair_id", ""),
        row.get("requested_mode", ""),
        row.get("actual_mode", ""),
        *count_key(row),
    )


def block_key(row: dict[str, str]) -> tuple[str, str, str, str, str, str, str]:
    block = row.get("block_id") or f"{row.get('pair_id', '')}:rep{row.get('repetition', '')}"
    return (
        row.get("session_id", ""),
        row.get("pair_id", ""),
        block,
        row.get("repetition", ""),
        *count_key(row),
    )


def aggregate_runs(runs: list[dict[str, str]]) -> list[dict[str, object]]:
    valid_runs = [row for row in runs if row.get("valid") == "true"]
    groups: dict[tuple[str, str, str, str, str, str, str], list[dict[str, str]]] = {}
    for row in valid_runs:
        key = run_key(row)
        groups.setdefault(key, []).append(row)

    aggregates: list[dict[str, object]] = []
    for (session_id, pair_id, requested_mode, actual_mode, actual_total, actual_static, actual_dynamic), rows in sorted(groups.items()):
        cpu = [float(row["mean_cpu_frame_ms"]) for row in rows]
        critical = [float(row["critical_path_gpu_ms"]) for row in rows]
        aggregates.append(
            {
                "session_id": session_id,
                "pair_id": pair_id,
                "requested_mode": requested_mode,
                "actual_mode": actual_mode,
                "actual_total_count": actual_total,
                "actual_static_count": actual_static,
                "actual_dynamic_count": actual_dynamic,
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

    singles_by_block: dict[tuple[str, str, str, str, str, str, str, str], dict[str, str]] = {}
    multis_by_block: dict[tuple[str, str, str, str, str, str, str, str], dict[str, str]] = {}
    for row in valid_runs:
        family = mode_family(row.get("requested_mode", ""))
        if not family:
            continue
        key = (*block_key(row), family)
        if is_single(row.get("requested_mode", "")):
            singles_by_block[key] = row
        elif is_multi(row.get("requested_mode", "")):
            multis_by_block[key] = row

    paired_by_group: dict[tuple[str, str, str], list[dict[str, float]]] = {}
    for key, multi in multis_by_block.items():
        single = singles_by_block.get(key)
        if not single:
            continue
        single_ms = float(single["mean_cpu_frame_ms"])
        multi_ms = float(multi["mean_cpu_frame_ms"])
        if single_ms <= 0.0 or multi_ms <= 0.0:
            continue
        group_key = (multi.get("session_id", ""), multi["pair_id"], multi["requested_mode"])
        paired_by_group.setdefault(group_key, []).append(
            {
                "difference_ms": single_ms - multi_ms,
                "log_speedup": math.log(single_ms / multi_ms),
                "speedup": single_ms / multi_ms,
            }
        )

    for row in aggregates:
        mode = row["requested_mode"]
        if is_multi(str(mode)):
            pairs = paired_by_group.get((str(row["session_id"]), str(row["pair_id"]), str(mode)), [])
            diffs = [entry["difference_ms"] for entry in pairs]
            logs = [entry["log_speedup"] for entry in pairs]
            row["paired_run_count"] = len(pairs)
            row["paired_single_multi_difference_ms"] = mean(diffs)
            row["paired_difference_ci95_half_width_ms"] = student_t_ci95_half_width(diffs)
            row["paired_log_speedup_mean"] = mean(logs)
            row["paired_log_speedup_ci95_half_width"] = student_t_ci95_half_width(logs)
            row["paired_speedup"] = math.exp(mean(logs)) if logs else None
            row["two_device_nominal_efficiency"] = row["paired_speedup"] / 2.0 if row["paired_speedup"] else None
        else:
            row["paired_run_count"] = 0
            row["paired_single_multi_difference_ms"] = None
            row["paired_difference_ci95_half_width_ms"] = None
            row["paired_log_speedup_mean"] = None
            row["paired_log_speedup_ci95_half_width"] = None
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

    require(
        manifest.get("status") == "COMPLETE",
        f"non-blocked suite manifest status must be COMPLETE, got {manifest.get('status')!r}",
    )
    require(runs, "non-blocked suite has no runs.csv rows")
    require(raw, "non-blocked suite has no raw_frames.csv rows")
    validate_evidence_files(root, manifest)
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

    def test_block_level_pairing_uses_matching_repetition(self) -> None:
        runs = [
            {"session_id": "s", "pair_id": "p", "block_id": "b0", "repetition": "0",
             "requested_mode": "SingleGpuFull", "actual_mode": "SingleGpuFull", "valid": "true",
             "mean_cpu_frame_ms": "20", "critical_path_gpu_ms": "8"},
            {"session_id": "s", "pair_id": "p", "block_id": "b0", "repetition": "0",
             "requested_mode": "MultiGpuFull", "actual_mode": "MultiGpuFull", "valid": "true",
             "mean_cpu_frame_ms": "10", "critical_path_gpu_ms": "4"},
            {"session_id": "s", "pair_id": "p", "block_id": "b1", "repetition": "1",
             "requested_mode": "SingleGpuFull", "actual_mode": "SingleGpuFull", "valid": "true",
             "mean_cpu_frame_ms": "9", "critical_path_gpu_ms": "8"},
            {"session_id": "s", "pair_id": "p", "block_id": "b2", "repetition": "1",
             "requested_mode": "MultiGpuFull", "actual_mode": "MultiGpuFull", "valid": "true",
             "mean_cpu_frame_ms": "3", "critical_path_gpu_ms": "4"},
        ]
        aggregates = aggregate_runs(runs)
        multi = next(row for row in aggregates if row["requested_mode"] == "MultiGpuFull")
        self.assertEqual(multi["paired_run_count"], 1)
        self.assertAlmostEqual(multi["paired_speedup"], 2.0)

    def test_pairing_requires_matching_actual_counts(self) -> None:
        runs = [
            {"session_id": "s", "pair_id": "p", "block_id": "b0", "repetition": "0",
             "requested_mode": "SingleGpuFull", "actual_mode": "SingleGpuFull", "valid": "true",
             "actual_total_count": "125000", "actual_static_count": "100000", "actual_dynamic_count": "25000",
             "mean_cpu_frame_ms": "20", "critical_path_gpu_ms": "8"},
            {"session_id": "s", "pair_id": "p", "block_id": "b0", "repetition": "0",
             "requested_mode": "MultiGpuFull", "actual_mode": "MultiGpuFull", "valid": "true",
             "actual_total_count": "123556", "actual_static_count": "98556", "actual_dynamic_count": "25000",
             "mean_cpu_frame_ms": "10", "critical_path_gpu_ms": "4"},
        ]
        aggregates = aggregate_runs(runs)
        multi = next(row for row in aggregates if row["requested_mode"] == "MultiGpuFull")
        self.assertEqual(multi["paired_run_count"], 0)
        self.assertIsNone(multi["paired_speedup"])

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

    def test_running_manifest_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "manifest.json").write_text(
                json.dumps({"schema": "mgpu_voxel_benchmark_manifest.v1", "run_id": "r", "status": "RUNNING"}),
                encoding="utf-8",
            )
            (root / "environment.json").write_text(
                json.dumps({"schema": "mgpu_voxel_environment.v1", "run_id": "r"}),
                encoding="utf-8",
            )
            (root / "runs.csv").write_text(
                "schema,suite,run_id,pair_id,requested_mode,actual_mode,repetition,valid,mean_cpu_frame_ms,critical_path_gpu_ms\n"
                "mgpu_voxel_runs.v1,Smoke,r,p,SingleGpuFull,SingleGpuFull,0,true,10,8\n",
                encoding="utf-8",
            )
            (root / "raw_frames.csv").write_text(
                "frame_index,suite,run_id,requested_mode,actual_mode,cpu_frame_ms\n"
                "0,Smoke,r,SingleGpuFull,SingleGpuFull,10\n",
                encoding="utf-8",
            )
            (root / "invalid_records.csv").write_text(
                "schema,suite,run_id,pair_id,requested_mode,actual_mode,repetition,reason\n",
                encoding="utf-8",
            )
            (root / "voxel_visual_validation.json").write_text("{}", encoding="utf-8")
            (root / "voxel_visual_validation.csv").write_text("case,status\n", encoding="utf-8")
            (root / "two_adapter_preflight.json").write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "manifest status must be COMPLETE"):
                validate(root)

    def test_complete_manifest_rejects_empty_claimed_evidence_csv(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "manifest.json").write_text(
                json.dumps({
                    "schema": "mgpu_voxel_benchmark_manifest.v1",
                    "run_id": "r",
                    "status": "COMPLETE",
                    "evidence_files": ["telemetry.csv"],
                }),
                encoding="utf-8",
            )
            (root / "environment.json").write_text(
                json.dumps({"schema": "mgpu_voxel_environment.v1", "run_id": "r"}),
                encoding="utf-8",
            )
            (root / "runs.csv").write_text(
                "schema,suite,run_id,pair_id,requested_mode,actual_mode,repetition,valid,mean_cpu_frame_ms,critical_path_gpu_ms\n"
                "mgpu_voxel_runs.v1,Smoke,r,p,SingleGpuFull,SingleGpuFull,0,true,10,8\n",
                encoding="utf-8",
            )
            (root / "raw_frames.csv").write_text(
                "frame_index,suite,run_id,requested_mode,actual_mode,cpu_frame_ms\n"
                "0,Smoke,r,SingleGpuFull,SingleGpuFull,10\n",
                encoding="utf-8",
            )
            (root / "invalid_records.csv").write_text(
                "schema,suite,run_id,pair_id,requested_mode,actual_mode,repetition,reason\n",
                encoding="utf-8",
            )
            (root / "voxel_visual_validation.json").write_text("{}", encoding="utf-8")
            (root / "voxel_visual_validation.csv").write_text("case,status\nc,PASS\n", encoding="utf-8")
            (root / "two_adapter_preflight.json").write_text("{}", encoding="utf-8")
            (root / "telemetry.csv").write_text("schema,run_id\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "evidence CSV has no data rows"):
                validate(root)


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

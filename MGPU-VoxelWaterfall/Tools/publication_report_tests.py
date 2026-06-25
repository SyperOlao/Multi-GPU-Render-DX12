#!/usr/bin/env python3
"""Regression tests for publication report fail-closed behavior."""

from __future__ import annotations

import json
import csv
import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).with_name("generate_publication_report.py")


class PublicationReportTests(unittest.TestCase):
    def read_csv_rows(self, path: Path) -> list[dict[str, str]]:
        with path.open(encoding="utf-8", newline="") as handle:
            return list(csv.DictReader(handle))

    def write_blocked_artifact(self, root: Path) -> None:
        root.mkdir(parents=True, exist_ok=True)
        (root / "analysis_summary.v2.json").write_text(json.dumps({
            "schema": "mgpu_voxel_analysis_summary.v2",
            "status": "FAIL",
            "hypothesis_results": {
                "H1": {"result": "SUPPORT", "statistics": {"paired_n": 999}},
            },
        }), encoding="utf-8")
        (root / "analysis_summary.v2.sha256").write_text(
            "0000000000000000000000000000000000000000000000000000000000000000  analysis_summary.v2.json\n",
            encoding="utf-8",
        )

    def write_pass_artifact(self, root: Path, *, missing_ci: bool = False) -> None:
        root.mkdir(parents=True, exist_ok=True)
        h1_stats = {
            "endpoint": "present_to_present_ms",
            "paired_n": 3,
            "mean_difference": 2.5,
            "median_difference": 2.25,
            "mean_log_speedup": 0.3,
            "log_speedup_ci95": {"status": "MEASURED", "lower": 0.1, "upper": 0.5},
            "speedup_ci95": {"status": "MEASURED", "mean": 1.35, "lower": 1.1, "upper": 1.65},
            "coefficient_of_variation": 0.12,
        }
        if not missing_ci:
            h1_stats["difference_ci95"] = {"status": "MEASURED", "lower": 1.5, "upper": 3.5}
        h2_stats = {
            "endpoint": "secondary_compute_ms",
            "paired_n": 3,
            "mean_difference": 1.25,
            "median_difference": 1.0,
            "difference_ci95": {"status": "MEASURED", "lower": 0.25, "upper": 2.0},
            "mean_log_speedup": 0.2,
            "log_speedup_ci95": {"status": "MEASURED", "lower": 0.05, "upper": 0.35},
            "speedup_ci95": {"status": "MEASURED", "mean": 1.22, "lower": 1.05, "upper": 1.42},
            "coefficient_of_variation": 0.2,
        }
        summary = {
            "schema": "mgpu_voxel_analysis_summary.v2",
            "status": "PASS",
            "suite": "Smoke",
            "run_id": "run-pass",
            "hypothesis_results": {
                "H1": {
                    "endpoint": "present_to_present_ms",
                    "effect_direction": "MULTI_LOWER",
                    "result": "SUPPORT",
                    "statistics": h1_stats,
                },
                "H2": {
                    "endpoint": "secondary_compute_ms",
                    "effect_direction": "TEMPORAL_LOWER",
                    "result": "SUPPORT",
                    "logical_work_equal": True,
                    "approximation_fidelity_pass": True,
                    "statistics": h2_stats,
                },
                "H3": {"result": "NOT_MEASURED"},
                "RQ3": {"result": "NOT_MEASURED"},
            },
        }
        path = root / "analysis_summary.v2.json"
        path.write_text(json.dumps(summary, sort_keys=True), encoding="utf-8")
        checksum = hashlib.sha256(path.read_bytes()).hexdigest()
        (root / "analysis_summary.v2.sha256").write_text(
            f"{checksum}  analysis_summary.v2.json\n", encoding="utf-8")

    def test_blocked_artifact_does_not_create_positive_report(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            smoke = root / "smoke"
            full = root / "full"
            out = root / "report"
            self.write_blocked_artifact(smoke)
            self.write_blocked_artifact(full)
            result = subprocess.run(
                [sys.executable, str(TOOL), "--smoke", str(smoke), "--full", str(full), "--output", str(out), "--strict"],
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(result.returncode, 0)
            report = (out / "RESULTS.md").read_text(encoding="utf-8")
            self.assertIn("BLOCKED", report)
            self.assertIn("NOT_MEASURED", report)
            self.assertNotIn("SUPPORT", report)
            self.assertFalse((out / "h1_paired_endpoint_forest.svg").exists())

    def test_pass_report_uses_current_v2_endpoint_schema(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            smoke = root / "smoke"
            full = root / "full"
            out = root / "report"
            self.write_pass_artifact(smoke)
            self.write_pass_artifact(full)
            result = subprocess.run(
                [sys.executable, str(TOOL), "--smoke", str(smoke), "--full", str(full), "--output", str(out), "--strict"],
                text=True,
                capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            h1_rows = self.read_csv_rows(out / "h1_paired_endpoint_table.csv")
            h2_rows = self.read_csv_rows(out / "h2_temporal_ablation_table.csv")
            self.assertEqual(h1_rows[0]["mean_difference_ms"], "2.5")
            self.assertEqual(h1_rows[0]["median_difference_ms"], "2.25")
            self.assertEqual(h1_rows[0]["difference_ci_lower_ms"], "1.5")
            self.assertEqual(h1_rows[0]["difference_ci_upper_ms"], "3.5")
            self.assertEqual(h1_rows[0]["effect_direction"], "MULTI_LOWER")
            self.assertEqual(h2_rows[0]["mean_difference_ms"], "1.25")
            self.assertEqual(h2_rows[0]["median_difference_ms"], "1.0")
            self.assertEqual(h2_rows[0]["effect_direction"], "TEMPORAL_LOWER")
            self.assertTrue((out / "h1_paired_endpoint_forest.svg").exists())

    def test_support_is_removed_when_measured_ci_is_lost(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            smoke = root / "smoke"
            full = root / "full"
            out = root / "report"
            self.write_pass_artifact(smoke, missing_ci=True)
            self.write_pass_artifact(full, missing_ci=True)
            result = subprocess.run(
                [sys.executable, str(TOOL), "--smoke", str(smoke), "--full", str(full), "--output", str(out), "--strict"],
                text=True,
                capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            h1_rows = self.read_csv_rows(out / "h1_paired_endpoint_table.csv")
            self.assertNotEqual(h1_rows[0]["result"], "SUPPORT")
            self.assertEqual(h1_rows[0]["difference_ci_status"], "NOT_MEASURED")


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=2))

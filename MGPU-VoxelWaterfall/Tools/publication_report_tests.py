#!/usr/bin/env python3
"""Regression tests for publication report fail-closed behavior."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).with_name("generate_publication_report.py")


class PublicationReportTests(unittest.TestCase):
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


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=2))

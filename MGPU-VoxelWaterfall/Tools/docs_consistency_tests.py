#!/usr/bin/env python3
"""Check documentation constants against source/tool constants."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
SOURCE_CPP = ROOT / "Source.cpp"
ANALYZER = ROOT / "Tools" / "analyze_benchmark.py"
DATA_DICTIONARY = ROOT / "Docs" / "schema" / "data_dictionary.md"
REPRODUCTION = ROOT / "Docs" / "Reproduction.md"
METHODOLOGY = ROOT / "Docs" / "Methodology.md"
RESEARCH_PROVENANCE = ROOT / "Source" / "Benchmark" / "ResearchProvenance.cpp"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def source_files() -> list[Path]:
    return [
        SOURCE_CPP,
        ANALYZER,
        ROOT / "Tools" / "plot_benchmark.py",
        ROOT / "Tools" / "generate_publication_report.py",
        ROOT / "Source" / "Benchmark" / "ResearchArtifactWriter.cpp",
        ROOT / "Source" / "Validation" / "VoxelVisualValidationRunner.cpp",
        ROOT / "Source" / "Validation" / "VoxelVisualValidationRunner.h",
        ROOT / "Source" / "Validation" / "TwoAdapterVerification.cpp",
        ROOT / "InternalBuild" / "build.ps1",
        ROOT / "Research" / "run_publication_suite.ps1",
        ROOT / "InternalBuild" / "test.ps1",
    ]


class DocsConsistencyTests(unittest.TestCase):
    def test_cli_flags_documented(self) -> None:
        source = read(SOURCE_CPP)
        flags = sorted(set(re.findall(r'"(--[A-Za-z0-9-]+=?)(?:"|,)', source)))
        docs = read(DATA_DICTIONARY) + "\n" + read(REPRODUCTION)
        missing = [flag for flag in flags if flag not in docs]
        self.assertFalse(missing, f"undocumented CLI flags: {missing}")

    def test_schema_strings_documented(self) -> None:
        schemas: set[str] = set()
        for path in source_files():
            schemas.update(re.findall(r"mgpu_[A-Za-z0-9_\.]+v[0-9]", read(path)))
        docs = read(DATA_DICTIONARY)
        missing = sorted(schema for schema in schemas if schema not in docs)
        self.assertFalse(missing, f"undocumented schema strings: {missing}")

    def test_status_enums_documented(self) -> None:
        source = read(RESEARCH_PROVENANCE)
        statuses = sorted(set(re.findall(r'return "(PENDING|RUNNING|COMPLETE|BLOCKED|INVALID|CANCELLED|INTERRUPTED)"', source)))
        docs = read(DATA_DICTIONARY) + "\n" + read(METHODOLOGY)
        missing = [status for status in statuses if status not in docs]
        self.assertFalse(missing, f"undocumented status enums: {missing}")

    def test_canonical_artifact_filenames_documented(self) -> None:
        analyzer = read(ANALYZER)
        filenames = sorted(set(re.findall(r'"([A-Za-z0-9_.*<>-]+(?:\\.v2)?\\.(?:json|csv|sha256|txt))"', analyzer)))
        required = [
            "manifest.json",
            "environment.json",
            "voxel_visual_validation.json",
            "voxel_visual_validation.csv",
            "two_adapter_preflight.json",
            "raw_frames.csv",
            "runs.csv",
            "paired_runs.csv",
            "invalid_records.csv",
            "telemetry.csv",
            "analysis_summary.v2.json",
            "analysis_summary.v2.sha256",
        ]
        filenames = sorted(set(filenames + required))
        docs = read(DATA_DICTIONARY) + "\n" + read(REPRODUCTION)
        missing = [name for name in filenames if name not in docs]
        self.assertFalse(missing, f"undocumented artifact filenames: {missing}")


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=2))

#!/usr/bin/env python3
"""Check documentation constants against source/tool constants."""

from __future__ import annotations

import re
import json
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
RENDER_PIPELINE_CPP = ROOT / "Source" / "Rendering" / "RenderPipeline.cpp"
RESULTS_NOTEBOOK = ROOT / "Research" / "notebooks" / "results_analysis.ipynb"


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


def extract_cpp_function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function signature: {signature}")
    brace = source.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing function body: {signature}")
    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise AssertionError(f"unterminated function body: {signature}")


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

    def test_render_pipeline_normal_submit_paths_do_not_cpu_wait(self) -> None:
        source = read(RENDER_PIPELINE_CPP)
        submit_signatures = [
            "void RenderPipeline::SubmitPrimaryBasePass",
            "void RenderPipeline::SubmitSecondaryVoxelPass",
            "void RenderPipeline::SubmitSecondaryLocalToSharedCopyPass",
            "void RenderPipeline::SubmitPrimarySharedToLocalCopyPass",
            "void RenderPipeline::SubmitFinalCompositeAndPresentPass",
        ]
        for signature in submit_signatures:
            body = extract_cpp_function_body(source, signature)
            self.assertNotIn("WaitForFenceValue", body, f"{signature} contains a CPU fence wait")
            self.assertIsNone(
                re.search(r"->\s*Flush\s*\(", body),
                f"{signature} contains a queue/device CPU flush",
            )

    def test_secondary_copy_keeps_gpu_side_render_fence_wait(self) -> None:
        source = read(RENDER_PIPELINE_CPP)
        body = extract_cpp_function_body(source, "void RenderPipeline::SubmitSecondaryLocalToSharedCopyPass")
        self.assertIn("context.SecondaryCopyQueue->Wait", body)
        self.assertIn("context.SecondaryRenderFence", body)
        self.assertIn("context.SecondaryRenderFenceValue", body)

    def test_results_notebook_integrity(self) -> None:
        notebook = json.loads(read(RESULTS_NOTEBOOK))
        self.assertEqual(notebook.get("nbformat"), 4)
        self.assertIsInstance(notebook.get("cells"), list)
        forbidden_magics = []
        old_summary_keys = []
        for index, cell in enumerate(notebook["cells"]):
            source = cell.get("source", "")
            if isinstance(source, list):
                source = "".join(source)
            self.assertNotIn("???", source)
            if cell.get("cell_type") == "code":
                if re.search(r"(?m)^\\s*%%|^\\s*%sql\\b|^\\s*%load_ext\\b", source):
                    forbidden_magics.append(index)
                compile(source, f"{RESULTS_NOTEBOOK.name}:cell{index}", "exec")
            if "difference_ms_ci95" in source or "mean_difference_ms" in source:
                old_summary_keys.append(index)
        self.assertFalse(forbidden_magics, f"unknown notebook magics in cells: {forbidden_magics}")
        self.assertFalse(old_summary_keys, f"old summary keys in notebook cells: {old_summary_keys}")


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=2))

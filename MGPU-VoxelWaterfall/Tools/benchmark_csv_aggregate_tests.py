#!/usr/bin/env python3
"""Reference tests for BenchmarkCsvWriter aggregate semantics."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Source" / "Benchmark" / "BenchmarkCsvWriter.cpp"


def aggregate_totals(rows: list[dict[str, int]]) -> tuple[int, int]:
    executed = 0
    logical = 0
    for row in rows:
        executed += row["TotalExecutedFixedSteps"]
        logical += row["TotalLogicalUpdatedVoxelCount"]
    return executed, logical


class BenchmarkCsvAggregateTests(unittest.TestCase):
    def test_first_repetition_is_not_counted_twice(self) -> None:
        rows = [
            {"TotalExecutedFixedSteps": 10, "TotalLogicalUpdatedVoxelCount": 100},
            {"TotalExecutedFixedSteps": 20, "TotalLogicalUpdatedVoxelCount": 200},
            {"TotalExecutedFixedSteps": 30, "TotalLogicalUpdatedVoxelCount": 300},
        ]
        self.assertEqual(aggregate_totals(rows), (60, 600))

    def test_cpp_aggregate_zeros_accumulators_after_front_copy(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        match = re.search(
            r"result\.AverageSecondaryLod2Count\s*=.*?"
            r"result\.TotalExecutedFixedSteps\s*=\s*0;\s*"
            r"result\.TotalLogicalUpdatedVoxelCount\s*=\s*0;\s*"
            r"for\s*\(const auto& row : rows\)",
            text,
            re.S,
        )
        self.assertIsNotNone(match, "Aggregate must zero totals after copying rows.front()")


if __name__ == "__main__":
    unittest.main(verbosity=2)

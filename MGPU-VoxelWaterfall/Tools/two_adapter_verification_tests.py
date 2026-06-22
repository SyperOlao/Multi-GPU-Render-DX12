#!/usr/bin/env python3
"""GPU-free negative tests for two-adapter runtime evidence semantics."""

from __future__ import annotations

import copy
import unittest


def valid_evidence() -> dict:
    return {
        "primary_luid": "1:2",
        "secondary_luid": "3:4",
        "requested_mode": "MultiGpuFull",
        "actual_mode": "MultiGpuFull",
        "secondary_partition_voxels": 100,
        "secondary_compute_dispatch_count": 12,
        "secondary_graphics_draw_count": 12,
        "pipeline_statistics": {
            "ia_primitives": 100,
            "vs_invocations": 100,
            "ps_invocations": 100,
        },
        "command_list_submissions": {
            "secondary_graphics": 12,
            "local_to_shared": 12,
            "shared_to_local": 12,
        },
        "expected": {
            "color_local_to_shared": 1024,
            "depth_local_to_shared": 2048,
            "color_shared_to_local": 1024,
            "depth_shared_to_local": 2048,
        },
        "measured": {
            "color_local_to_shared": 1024,
            "depth_local_to_shared": 2048,
            "color_shared_to_local": 1024,
            "depth_shared_to_local": 2048,
        },
        "resources_complete": True,
        "fences_complete": True,
        "queues_calibrated": True,
        "composite_submitted": True,
        "particle_transfer_bytes": 0,
        "build_hash": "build-a",
        "shader_hash": "shader-a",
        "config_hash": "config-a",
        "expected_build_hash": "build-a",
        "expected_shader_hash": "shader-a",
        "expected_config_hash": "config-a",
        "driver_identity_complete": True,
        "resolution_formats_match": True,
    }


def validate(e: dict) -> list[str]:
    reasons: list[str] = []
    if e["primary_luid"] == e["secondary_luid"]:
        reasons.append("same LUID")
    if e["requested_mode"] != "MultiGpuFull" or e["actual_mode"] != "MultiGpuFull":
        reasons.append("fallback")
    if e["secondary_partition_voxels"] <= 0:
        reasons.append("empty secondary partition")
    if e["secondary_compute_dispatch_count"] <= 0:
        reasons.append("missing GPU1 compute")
    if e["secondary_graphics_draw_count"] <= 0:
        reasons.append("missing GPU1 graphics")
    stats = e["pipeline_statistics"]
    if stats["ia_primitives"] <= 0 or stats["vs_invocations"] <= 0 or stats["ps_invocations"] <= 0:
        reasons.append("zero pipeline stats")
    submissions = e["command_list_submissions"]
    if submissions["local_to_shared"] <= 0:
        reasons.append("missing local_to_shared")
    if submissions["shared_to_local"] <= 0:
        reasons.append("missing shared_to_local")
    for key, expected in e["expected"].items():
        if e["measured"][key] != expected:
            reasons.append(f"wrong byte count: {key}")
    if not e["resources_complete"]:
        reasons.append("missing resource identity")
    if not e["fences_complete"] or not e["queues_calibrated"]:
        reasons.append("missing fence/calibration")
    if not e["composite_submitted"]:
        reasons.append("missing composite")
    if e["particle_transfer_bytes"] != 0:
        reasons.append("particle transfer")
    if (
        e["build_hash"] != e["expected_build_hash"]
        or e["shader_hash"] != e["expected_shader_hash"]
        or e["config_hash"] != e["expected_config_hash"]
        or not e["driver_identity_complete"]
        or not e["resolution_formats_match"]
    ):
        reasons.append("stale provenance")
    return reasons


class TwoAdapterVerificationTests(unittest.TestCase):
    def assert_fails(self, mutator, expected: str):
        evidence = valid_evidence()
        mutator(evidence)
        reasons = validate(evidence)
        self.assertTrue(reasons)
        self.assertTrue(any(expected in reason for reason in reasons), reasons)

    def test_valid_fixture_passes(self):
        self.assertEqual(validate(valid_evidence()), [])

    def test_missing_local_to_shared_fails(self):
        self.assert_fails(
            lambda e: e["command_list_submissions"].__setitem__("local_to_shared", 0),
            "local_to_shared",
        )

    def test_missing_shared_to_local_fails(self):
        self.assert_fails(
            lambda e: e["command_list_submissions"].__setitem__("shared_to_local", 0),
            "shared_to_local",
        )

    def test_wrong_byte_count_fails(self):
        self.assert_fails(
            lambda e: e["measured"].__setitem__("depth_shared_to_local", 1),
            "wrong byte count",
        )

    def test_pipeline_stats_zero_fails_even_when_draw_flag_true(self):
        def mutate(e):
            e["secondary_graphics_draw_count"] = 1
            e["pipeline_statistics"] = {
                "ia_primitives": 0,
                "vs_invocations": 0,
                "ps_invocations": 0,
            }

        self.assert_fails(mutate, "zero pipeline stats")

    def test_same_luid_fails(self):
        self.assert_fails(lambda e: e.__setitem__("secondary_luid", e["primary_luid"]), "same LUID")

    def test_fallback_fails(self):
        self.assert_fails(lambda e: e.__setitem__("actual_mode", "SingleGpuFull"), "fallback")

    def test_stale_shader_or_config_hash_fails(self):
        self.assert_fails(lambda e: e.__setitem__("shader_hash", "shader-b"), "stale provenance")
        self.assert_fails(lambda e: e.__setitem__("config_hash", "config-b"), "stale provenance")


if __name__ == "__main__":
    unittest.main(verbosity=2)

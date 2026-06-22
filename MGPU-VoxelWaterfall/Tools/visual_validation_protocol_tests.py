#!/usr/bin/env python3
"""GPU-free reference tests for visual validation protocol/mapping v2."""

from __future__ import annotations

import copy
import hashlib
import json
import unittest


def canonical_json(value: dict) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def protocol_hash(protocol: dict) -> str:
    return hashlib.sha256(canonical_json(protocol).encode("utf-8")).hexdigest()


def make_protocol() -> dict:
    return {
        "protocol_version": "mgpu_voxel_visual_validation_protocol.v2",
        "generator_version": "mgpu_voxel_visual_validation_generator.v2",
        "tolerances": {
            "color": 0.025,
            "depth": 0.02,
            "max_color_mismatch_percent": 0.5,
            "max_depth_mismatch_percent": 0.5,
            "max_combined_mismatch_percent": 0.5,
            "valid_depth_max": 1000000.0,
        },
        "runtime": {
            "build_sha256": "build-a",
            "shader_set_sha256": "shader-a",
            "adapter_pair": "1:2->3:4",
            "driver_primary": "4318:123",
            "driver_secondary": "4318:456",
            "render_width": 1920,
            "render_height": 1080,
            "sample_count": 1,
            "color_format": "R8G8B8A8_UNORM",
            "depth_format": "R32_FLOAT",
        },
        "cases": [
            {
                "case_id": "Full|Low|share0.50|lod_off|temporal1|equivalence|steady_240",
                "kind": "implementation_equivalence",
                "config_key": "Full|family=Full|preset=Low|label=100000|static=100000|dynamic=25000|share=0.50|lod=off|interval=1",
                "mode_family": "Full",
                "checkpoint_id": "steady_240",
                "checkpoint_index": 0,
                "reference_mode": "SingleGpuFull",
                "candidate_mode": "MultiGpuFull",
                "requested_label_count": 100000,
                "requested_static_budget": 100000,
                "requested_dynamic_budget": 25000,
                "actual_static_count": 98556,
                "actual_dynamic_count": 25000,
                "actual_total_count": 123556,
                "secondary_share": 0.5,
                "lod_enabled": False,
                "temporal_interval": 1,
                "camera_hash": "camera-a",
                "reference_config_hash": "single-full-hash",
                "candidate_config_hash": "multi-full-hash",
                "config_hash": "multi-full-hash",
                "render_width": 1920,
                "render_height": 1080,
                "sample_count": 1,
                "color_format": "R8G8B8A8_UNORM",
                "depth_format": "R32_FLOAT",
                "view_matrix": [1.0] * 16,
                "projection_matrix": [2.0] * 16,
                "randomization_seed": 0x5EED2026,
            }
        ],
    }


def covered(protocol: dict, rows: list[dict], configs: list[dict]) -> tuple[bool, str]:
    expected_protocol_hash = protocol_hash(protocol)
    for cfg in configs:
        matches = [
            row
            for row in rows
            if row.get("passed") is True
            and row.get("protocol_hash") == expected_protocol_hash
            and row.get("mode_name") == cfg["mode_name"]
            and row.get("config_hash") == cfg["resolved_config_hash"]
            and row.get("camera_hash")
            and row.get("render_width") == cfg["render_width"]
            and row.get("render_height") == cfg["render_height"]
            and row.get("color_format") == cfg["color_format"]
            and row.get("depth_format") == cfg["depth_format"]
            and row.get("sample_count") == cfg["sample_count"]
            and row.get("temporal_interval") == cfg["temporal_interval"]
        ]
        if not matches:
            return False, f"missing matching case for {cfg['config_id']}"
        if (cfg["temporal_interval"] > 1 or cfg["lod_enabled"]) and not any(
            row.get("kind") == "approximation_fidelity" for row in matches
        ):
            return False, f"missing fidelity case for {cfg['config_id']}"
    return True, ""


class VisualValidationProtocolTests(unittest.TestCase):
    def assert_hash_changes(self, mutator):
        base = make_protocol()
        changed = copy.deepcopy(base)
        mutator(changed)
        self.assertNotEqual(protocol_hash(base), protocol_hash(changed))

    def test_hash_changes_for_protocol_inputs(self):
        for mutator in [
            lambda p: p["tolerances"].__setitem__("max_color_mismatch_percent", 0.25),
            lambda p: p["cases"].append(copy.deepcopy(p["cases"][0])),
            lambda p: p["cases"][0].__setitem__("camera_hash", "camera-b"),
            lambda p: p["cases"][0].__setitem__("randomization_seed", 123),
            lambda p: p["cases"][0].__setitem__("actual_static_count", 42),
            lambda p: p["cases"][0].__setitem__("secondary_share", 0.75),
            lambda p: p["cases"][0].__setitem__("temporal_interval", 4),
        ]:
            self.assert_hash_changes(mutator)

    def test_order_independent_json_keys_same_hash(self):
        base = make_protocol()
        shuffled = json.loads(json.dumps(base))
        shuffled = {
            "cases": shuffled["cases"],
            "runtime": shuffled["runtime"],
            "generator_version": shuffled["generator_version"],
            "tolerances": shuffled["tolerances"],
            "protocol_version": shuffled["protocol_version"],
        }
        self.assertEqual(protocol_hash(base), protocol_hash(shuffled))

    def test_missing_matching_case_blocks_suite(self):
        protocol = make_protocol()
        ok, reason = covered(protocol, [], [{
            "config_id": "cfg",
            "mode_name": "MultiGpuFull",
            "resolved_config_hash": "multi-full-hash",
            "render_width": 1920,
            "render_height": 1080,
            "sample_count": 1,
            "color_format": "R8G8B8A8_UNORM",
            "depth_format": "R32_FLOAT",
            "temporal_interval": 1,
            "lod_enabled": False,
        }])
        self.assertFalse(ok)
        self.assertIn("missing matching case", reason)

    def test_full_temporal_interval_4_cannot_use_interval_2_case(self):
        protocol = make_protocol()
        h = protocol_hash(protocol)
        ok, reason = covered(protocol, [{
            "passed": True,
            "kind": "approximation_fidelity",
            "protocol_hash": h,
            "mode_name": "MultiGpuTemporalDecimation",
            "config_hash": "temporal-hash",
            "camera_hash": "camera-a",
            "render_width": 1920,
            "render_height": 1080,
            "sample_count": 1,
            "color_format": "R8G8B8A8_UNORM",
            "depth_format": "R32_FLOAT",
            "temporal_interval": 2,
        }], [{
            "config_id": "temporal4",
            "mode_name": "MultiGpuTemporalDecimation",
            "resolved_config_hash": "temporal-hash",
            "render_width": 1920,
            "render_height": 1080,
            "sample_count": 1,
            "color_format": "R8G8B8A8_UNORM",
            "depth_format": "R32_FLOAT",
            "temporal_interval": 4,
            "lod_enabled": False,
        }])
        self.assertFalse(ok)
        self.assertIn("missing matching case", reason)

    def test_dimension_or_format_mismatch_blocks(self):
        protocol = make_protocol()
        h = protocol_hash(protocol)
        row = {
            "passed": True,
            "kind": "implementation_equivalence",
            "protocol_hash": h,
            "mode_name": "MultiGpuFull",
            "config_hash": "multi-full-hash",
            "camera_hash": "camera-a",
            "render_width": 1280,
            "render_height": 1080,
            "sample_count": 1,
            "color_format": "R8G8B8A8_UNORM",
            "depth_format": "R32_FLOAT",
            "temporal_interval": 1,
        }
        ok, _ = covered(protocol, [row], [{
            "config_id": "cfg",
            "mode_name": "MultiGpuFull",
            "resolved_config_hash": "multi-full-hash",
            "render_width": 1920,
            "render_height": 1080,
            "sample_count": 1,
            "color_format": "R8G8B8A8_UNORM",
            "depth_format": "R32_FLOAT",
            "temporal_interval": 1,
            "lod_enabled": False,
        }])
        self.assertFalse(ok)

    def test_each_benchmark_config_gets_own_case_metrics(self):
        protocol = make_protocol()
        h = protocol_hash(protocol)
        rows = [
            {
                "passed": True,
                "kind": "implementation_equivalence",
                "protocol_hash": h,
                "mode_name": "SingleGpuFull",
                "config_hash": "single-full-hash",
                "camera_hash": "camera-a",
                "render_width": 1920,
                "render_height": 1080,
                "sample_count": 1,
                "color_format": "R8G8B8A8_UNORM",
                "depth_format": "R32_FLOAT",
                "temporal_interval": 1,
                "color_mae": 0.001,
            },
            {
                "passed": True,
                "kind": "implementation_equivalence",
                "protocol_hash": h,
                "mode_name": "MultiGpuFull",
                "config_hash": "multi-full-hash",
                "camera_hash": "camera-a",
                "render_width": 1920,
                "render_height": 1080,
                "sample_count": 1,
                "color_format": "R8G8B8A8_UNORM",
                "depth_format": "R32_FLOAT",
                "temporal_interval": 1,
                "color_mae": 0.002,
            },
        ]
        configs = [
            {
                "config_id": "single",
                "mode_name": "SingleGpuFull",
                "resolved_config_hash": "single-full-hash",
                "render_width": 1920,
                "render_height": 1080,
                "sample_count": 1,
                "color_format": "R8G8B8A8_UNORM",
                "depth_format": "R32_FLOAT",
                "temporal_interval": 1,
                "lod_enabled": False,
            },
            {
                "config_id": "multi",
                "mode_name": "MultiGpuFull",
                "resolved_config_hash": "multi-full-hash",
                "render_width": 1920,
                "render_height": 1080,
                "sample_count": 1,
                "color_format": "R8G8B8A8_UNORM",
                "depth_format": "R32_FLOAT",
                "temporal_interval": 1,
                "lod_enabled": False,
            },
        ]
        self.assertEqual(covered(protocol, rows, configs), (True, ""))
        self.assertNotEqual(rows[0]["color_mae"], rows[1]["color_mae"])


if __name__ == "__main__":
    unittest.main(verbosity=2)

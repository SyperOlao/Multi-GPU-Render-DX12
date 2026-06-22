#!/usr/bin/env python3
"""Hostile offline validator for MGPU-VoxelWaterfall benchmark artifacts.

This analyzer treats producer-generated valid/status/summary fields as claims.
It recomputes validity, run summaries, pair summaries, checksums, and inference
from first-principles evidence.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import statistics
import sys
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SCHEMA = {
    "manifest": {"mgpu_voxel_benchmark_manifest.v2", "mgpu_voxel_benchmark_manifest.v1"},
    "environment": {"mgpu_voxel_environment.v2", "mgpu_voxel_environment.v1"},
    "validation_json": {"mgpu_voxel_visual_validation.v2", None},
    "validation_csv": {"mgpu_voxel_visual_validation.v2", None, ""},
    "two_adapter_json": {"mgpu_voxel_two_adapter_preflight.v2", None},
    "execution_manifest": {"mgpu_voxel_execution_manifest.v2", None},
    "raw_frames": {"mgpu_voxel_raw_frame.v2", None, ""},
    "runs": {"mgpu_voxel_runs.v2", "mgpu_voxel_runs.v1", ""},
    "paired_runs": {"mgpu_voxel_paired_runs.v2", "mgpu_voxel_paired_runs.v1", ""},
    "invalid_records": {"mgpu_voxel_invalid_records.v2", "mgpu_voxel_invalid_records.v1", ""},
    "telemetry": {"mgpu_voxel_run_telemetry.v2", "mgpu_voxel_run_telemetry.v1", ""},
}

MODES = {
    "SingleGpuFull",
    "MultiGpuFull",
    "SingleGpuTemporalDecimation",
    "MultiGpuTemporalDecimation",
}
MULTI_MODES = {"MultiGpuFull", "MultiGpuTemporalDecimation"}
SINGLE_MODES = {"SingleGpuFull", "SingleGpuTemporalDecimation"}
STATUS = {"PENDING", "RUNNING", "COMPLETE", "BLOCKED", "INVALID", "CANCELLED", "INTERRUPTED"}
EPS = 1.0e-5
PRIMARY_ENDPOINT = "present_to_present_ms"
CPU_ENDPOINTS = ["present_to_present_ms", "cpu_submission_ms", "cpu_total_frame_ms"]
GPU_ENDPOINTS = ["critical_path_gpu_ms", "gpu_work_sum_ms"]
PAIRED_ENDPOINTS = CPU_ENDPOINTS + GPU_ENDPOINTS

REQUIRED_PROVENANCE_FIELDS = {
    "build.executable_sha256",
    "build.shader_bytecode_set_sha256",
    "adapter.luid_pair",
    "validation.protocol_sha256",
    "validation.case_config_sha256",
    "validation.camera_sha256",
    "render.resolution",
    "render.color_format",
    "render.depth_format",
    "render.sample_count",
}

NUMERIC_RUN_FIELDS = [
    "mean_present_to_present_ms",
    "mean_cpu_submission_ms",
    "mean_cpu_total_frame_ms",
    "critical_path_gpu_ms",
    "gpu_work_sum_ms",
    "primary_compute_ms",
    "primary_graphics_ms",
    "secondary_compute_ms",
    "secondary_graphics_ms",
    "transfer_ms",
    "composite_ms",
]


class ValidationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise ValidationError(message)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_json(path: Path) -> dict[str, Any]:
    require(path.exists(), f"missing artifact: {path.name}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        fail(f"invalid JSON {path.name}: {exc}")
    require(isinstance(data, dict), f"{path.name} must be a JSON object")
    return data


def read_csv(path: Path) -> list[dict[str, str]]:
    require(path.exists(), f"missing artifact: {path.name}")
    with path.open(newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    return [normalize_csv_row(row) for row in rows]


CSV_FIELD_ALIASES = {
    "visual_validation_case_id": "validation_case_id",
    "visual_validation_protocol_hash": "validation_protocol_hash",
    "visual_validation_config_hash": "validation_config_hash",
    "visual_validation_camera_hash": "validation_camera_hash",
    "spatial_lod_policy": "spatial_lod",
    "seed": "randomization_seed",
}


def normalize_csv_row(row: dict[str, str]) -> dict[str, str]:
    normalized = dict(row)
    for source, target in CSV_FIELD_ALIASES.items():
        if source in normalized and normalized[source] != "":
            normalized[target] = normalized[source]
    return normalized


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def bool_value(value: Any, field: str) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered == "true":
            return True
        if lowered == "false":
            return False
    fail(f"{field} must be boolean")


def int_value(row: dict[str, Any], field: str, *, minimum: int | None = None) -> int:
    require(field in row and str(row[field]) != "", f"missing integer field {field}")
    try:
        value = int(str(row[field]), 0)
    except Exception:
        fail(f"{field} must be an integer")
    if minimum is not None:
        require(value >= minimum, f"{field} below minimum {minimum}")
    return value


def float_value(row: dict[str, Any], field: str, *, minimum: float | None = None,
                positive: bool = False) -> float:
    require(field in row and str(row[field]) != "", f"missing numeric field {field}")
    try:
        value = float(str(row[field]))
    except Exception:
        fail(f"{field} must be numeric")
    require(math.isfinite(value), f"{field} must be finite")
    if minimum is not None:
        require(value >= minimum, f"{field} below minimum {minimum}")
    if positive:
        require(value > 0.0, f"{field} must be positive")
    return value


def numeric_value(row: dict[str, Any], field: str, *, aliases: tuple[str, ...] = (),
                  minimum: float | None = None, positive: bool = False) -> float:
    for candidate in (field, *aliases):
        if candidate in row and str(row[candidate]) != "":
            return float_value(row, candidate, minimum=minimum, positive=positive)
    fail(f"missing numeric field {field}")


def optional_float(row: dict[str, Any], field: str, default: float = 0.0) -> float:
    if field not in row or str(row[field]) == "":
        return default
    return float_value(row, field, minimum=0.0)


def schema_value(row: dict[str, Any]) -> str | None:
    value = row.get("schema")
    return str(value) if value is not None else None


def validate_schema(name: str, row: dict[str, Any]) -> None:
    require(schema_value(row) in SCHEMA[name], f"{name} schema mismatch: {schema_value(row)!r}")


def provenance_fields(document: dict[str, Any], label: str) -> dict[str, str]:
    provenance = document.get("provenance")
    require(isinstance(provenance, dict), f"{label} missing provenance object")
    fields = provenance.get("fields")
    require(isinstance(fields, dict), f"{label} provenance.fields must be object")
    unknown_critical = {
        key for key in fields
        if key.startswith(("critical.", "build.", "adapter.", "validation.", "render.", "workload.", "runtime."))
        and key not in REQUIRED_PROVENANCE_FIELDS
        and not key.startswith(("workload.", "runtime."))
    }
    require(not unknown_critical, f"{label} unknown critical provenance fields: {sorted(unknown_critical)}")
    return {str(key): str(value) for key, value in fields.items()}


def mode_family(mode: str) -> str:
    require(mode in MODES, f"unknown mode {mode!r}")
    return "Temporal" if "Temporal" in mode else "Full"


def expected_case_id(config: dict[str, Any]) -> str:
    family = "temporal" if "Temporal" in str(config["mode"]) else "full"
    lod = "lod_on" if bool_value(config.get("spatial_lod_enabled", config.get("spatial_lod") == "ThreeLevel"),
                                  "spatial_lod_enabled") else "lod_off"
    return f"{family}_{lod}"


def canonical_config_hash(config: dict[str, Any]) -> str:
    keys = [
        "config_id", "session_id", "pair_id", "block_id", "mode", "preset",
        "requested_static_budget_label", "requested_static_budget", "requested_dynamic_budget",
        "secondary_share", "spatial_lod", "temporal_interval", "repetition",
        "randomization_seed",
    ]
    payload = {key: config.get(key, "") for key in keys}
    return sha256_text(json.dumps(payload, sort_keys=True, separators=(",", ":")))


def mean(values: list[float]) -> float:
    return statistics.fmean(values) if values else math.nan


def percentile(values: list[float], p: float) -> float:
    require(values, "percentile requires values")
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = p * (len(ordered) - 1)
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] * (hi - pos) + ordered[hi] * (pos - lo)


def stddev(values: list[float]) -> float | None:
    return statistics.stdev(values) if len(values) > 1 else None


def _betacf(a: float, b: float, x: float) -> float:
    qab = a + b
    qap = a + 1.0
    qam = a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    if abs(d) < 1e-300:
        d = 1e-300
    d = 1.0 / d
    h = d
    for m in range(1, 200):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        if abs(d) < 1e-300:
            d = 1e-300
        c = 1.0 + aa / c
        if abs(c) < 1e-300:
            c = 1e-300
        d = 1.0 / d
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        if abs(d) < 1e-300:
            d = 1e-300
        c = 1.0 + aa / c
        if abs(c) < 1e-300:
            c = 1e-300
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 3e-14:
            break
    return h


def regularized_beta(a: float, b: float, x: float) -> float:
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    bt = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) +
                  a * math.log(x) + b * math.log1p(-x))
    if x < (a + 1.0) / (a + b + 2.0):
        return bt * _betacf(a, b, x) / a
    return 1.0 - bt * _betacf(b, a, 1.0 - x) / b


def student_t_cdf(t: float, df: int) -> float:
    require(df > 0, "Student-t df must be positive")
    x = df / (df + t * t)
    ib = regularized_beta(df / 2.0, 0.5, x)
    return 1.0 - 0.5 * ib if t >= 0.0 else 0.5 * ib


def student_t_ppf(p: float, df: int) -> float:
    require(0.0 < p < 1.0, "Student-t probability out of range")
    require(df > 0, "Student-t df must be positive")
    if p == 0.5:
        return 0.0
    sign = 1.0
    target = p
    if p < 0.5:
        sign = -1.0
        target = 1.0 - p
    lo, hi = 0.0, 1.0
    while student_t_cdf(hi, df) < target:
        hi *= 2.0
        require(hi < 1.0e6, "Student-t quantile did not converge")
    for _ in range(100):
        mid = (lo + hi) * 0.5
        if student_t_cdf(mid, df) < target:
            lo = mid
        else:
            hi = mid
    return sign * (lo + hi) * 0.5


def ci95(values: list[float]) -> dict[str, float | None | str]:
    if len(values) < 2:
        return {"mean": mean(values) if values else None, "lower": None, "upper": None, "status": "NOT_MEASURED"}
    m = mean(values)
    s = statistics.stdev(values)
    half = student_t_ppf(0.975, len(values) - 1) * s / math.sqrt(len(values))
    return {"mean": m, "lower": m - half, "upper": m + half, "status": "MEASURED"}


def coefficient_of_variation(values: list[float]) -> float | None:
    if len(values) < 2:
        return None
    m = mean(values)
    if abs(m) <= EPS:
        return None
    return statistics.stdev(values) / abs(m)


@dataclass(frozen=True)
class ArtifactSet:
    root: Path
    manifest: dict[str, Any]
    environment: dict[str, Any]
    validation_json: dict[str, Any]
    validation_csv: list[dict[str, str]]
    two_adapter: dict[str, Any]
    execution_manifests: list[dict[str, Any]]
    raw_frames: list[dict[str, str]]
    runs: list[dict[str, str]]
    paired_runs: list[dict[str, str]]
    invalid_records: list[dict[str, str]]
    telemetry: list[dict[str, str]]
    checksums: dict[str, str]


def load_artifacts(root: Path) -> ArtifactSet:
    files = {
        "manifest.json": root / "manifest.json",
        "environment.json": root / "environment.json",
        "voxel_visual_validation.json": root / "voxel_visual_validation.json",
        "voxel_visual_validation.csv": root / "voxel_visual_validation.csv",
        "two_adapter_preflight.json": root / "two_adapter_preflight.json",
        "raw_frames.csv": root / "raw_frames.csv",
        "runs.csv": root / "runs.csv",
        "paired_runs.csv": root / "paired_runs.csv",
        "invalid_records.csv": root / "invalid_records.csv",
        "telemetry.csv": root / "telemetry.csv",
    }
    checksums: dict[str, str] = {}
    for name, path in files.items():
        require(path.exists(), f"missing artifact: {name}")
        checksums[name] = sha256_file(path)
    executions = []
    for path in sorted(root.glob("VoxelBenchmark_*_manifest.json")):
        checksums[path.name] = sha256_file(path)
        execution = read_json(path)
        execution["_path_name"] = path.name
        executions.append(execution)
    return ArtifactSet(
        root=root,
        manifest=read_json(files["manifest.json"]),
        environment=read_json(files["environment.json"]),
        validation_json=read_json(files["voxel_visual_validation.json"]),
        validation_csv=read_csv(files["voxel_visual_validation.csv"]),
        two_adapter=read_json(files["two_adapter_preflight.json"]),
        execution_manifests=executions,
        raw_frames=read_csv(files["raw_frames.csv"]),
        runs=read_csv(files["runs.csv"]),
        paired_runs=read_csv(files["paired_runs.csv"]),
        invalid_records=read_csv(files["invalid_records.csv"]),
        telemetry=read_csv(files["telemetry.csv"]),
        checksums=checksums,
    )


def validate_top_level(artifacts: ArtifactSet) -> tuple[str, str, dict[str, str]]:
    manifest = artifacts.manifest
    validate_schema("manifest", manifest)
    validate_schema("environment", artifacts.environment)
    run_id = str(manifest.get("run_id", ""))
    suite = str(manifest.get("suite", ""))
    require(run_id, "manifest run_id is required")
    require(suite in {"Smoke", "Full"}, "manifest suite must be Smoke or Full")
    require(str(manifest.get("status", "")) in STATUS, "manifest status enum mismatch")
    require(artifacts.environment.get("run_id") == run_id, "environment run_id mismatch")
    require(artifacts.environment.get("suite", suite) == suite, "environment suite mismatch")
    fields = provenance_fields(manifest, "manifest")
    missing = REQUIRED_PROVENANCE_FIELDS - fields.keys()
    require(not missing, f"manifest provenance missing fields: {sorted(missing)}")
    for name, rows in (
        ("validation_csv", artifacts.validation_csv),
        ("runs", artifacts.runs),
        ("paired_runs", artifacts.paired_runs),
        ("invalid_records", artifacts.invalid_records),
        ("telemetry", artifacts.telemetry),
    ):
        require(rows, f"{name} must contain at least one data row")
        for row in rows:
            if "schema" in row:
                validate_schema(name, row)
            if "run_id" in row:
                require(row["run_id"] == run_id, f"{name} run_id mismatch")
            if "suite" in row:
                require(row["suite"] == suite, f"{name} suite mismatch")
    return run_id, suite, fields


def validate_validation_artifacts(artifacts: ArtifactSet, manifest_fields: dict[str, str],
                                  configs: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    visual = artifacts.validation_json
    validate_schema("validation_json", visual)
    require(visual.get("aggregate_status") == "PASS", "visual validation aggregate is not PASS")
    require(bool_value(visual.get("aggregate_passed", True), "aggregate_passed"), "visual aggregate_passed is false")
    visual_fields = provenance_fields(visual, "validation")
    for field in REQUIRED_PROVENANCE_FIELDS & visual_fields.keys():
        if field in manifest_fields:
            require(visual_fields[field] == manifest_fields[field], f"stale validation provenance: {field}")
    for field in ("build.executable_sha256", "build.shader_bytecode_set_sha256",
                  "validation.protocol_sha256", "validation.case_config_sha256",
                  "validation.camera_sha256"):
        require(visual_fields.get(field) == manifest_fields.get(field), f"stale validation {field}")
    cases = visual.get("cases", [])
    require(isinstance(cases, list) and cases, "validation cases are required")
    by_id: dict[str, dict[str, Any]] = {}
    for case in cases:
        require(isinstance(case, dict), "validation case must be object")
        case_id = str(case.get("case_id", ""))
        require(case_id and case_id not in by_id, f"missing or duplicate validation case {case_id!r}")
        require(case.get("status") == "PASS", f"validation case {case_id} is not PASS")
        require(bool_value(case.get("passed", True), f"{case_id}.passed"), f"validation case {case_id} passed=false")
        require(not bool_value(case.get("blocked", False), f"{case_id}.blocked"),
                f"validation case {case_id} is blocked")
        require(str(case.get("validation_kind", "")) in {"implementation_equivalence", "approximation_fidelity"},
                f"validation case {case_id} kind invalid")
        require(str(case.get("protocol_hash", "")) == manifest_fields["validation.protocol_sha256"],
                f"validation case {case_id} protocol hash stale")
        require(str(case.get("camera_hash", "")) != "", f"validation case {case_id} camera hash missing")
        require(str(case.get("reference_config_hash", "")) != "" or
                str(case.get("candidate_config_hash", "")) != "",
                f"validation case {case_id} lacks reference/candidate config hash")
        by_id[case_id] = case
    csv_cases = {row.get("case_id", ""): row for row in artifacts.validation_csv}
    for case_id, row in csv_cases.items():
        if case_id:
            require(row.get("status") == "PASS", f"validation CSV case {case_id} is not PASS")
    return by_id


def case_modes_match(case: dict[str, Any]) -> bool:
    for prefix in ("single", "multi", "reference", "candidate"):
        requested = str(case.get(f"requested_{prefix}_mode", ""))
        actual = str(case.get(f"actual_{prefix}_mode", ""))
        if requested and actual and requested != actual:
            return False
    return True


def validate_frame_visual_evidence(row: dict[str, str],
                                   config: dict[str, Any],
                                   manifest_fields: dict[str, str],
                                   validation_cases: dict[str, dict[str, Any]]) -> tuple[dict[str, Any], str]:
    case_id = row.get("validation_case_id", "")
    require(case_id, "validation case id missing")
    case = validation_cases.get(case_id)
    require(case is not None, f"validation case not found: {case_id}")
    require(case.get("status") == "PASS", f"validation case {case_id} is not PASS")
    require(bool_value(case.get("passed", False), f"{case_id}.passed"), f"validation case {case_id} passed=false")
    require(not bool_value(case.get("blocked", False), f"{case_id}.blocked"), f"validation case {case_id} blocked")
    require(case_modes_match(case), f"validation case {case_id} requested/actual mode mismatch")
    require(row.get("validation_protocol_hash", "") == str(case.get("protocol_hash", "")),
            "validation protocol hash mismatch")
    require(row.get("validation_protocol_hash", "") == manifest_fields["validation.protocol_sha256"],
            "validation protocol hash stale")
    require(row.get("validation_camera_hash", "") == str(case.get("camera_hash", "")),
            "validation camera hash mismatch")
    config_hash = row.get("validation_config_hash", "")
    require(config_hash != "", "validation config hash missing")
    require(config_hash in {str(case.get("reference_config_hash", "")),
                            str(case.get("candidate_config_hash", ""))},
            "validation config hash does not match case reference/candidate")
    resolved_hash = row.get("resolved_config_hash", "")
    validation_kind = str(case.get("validation_kind", ""))
    if validation_kind == "approximation_fidelity":
        require(str(case.get("candidate_config_hash", "")) == resolved_hash,
                "approximation fidelity candidate_config_hash mismatch")
    elif validation_kind == "implementation_equivalence":
        require(resolved_hash in {str(case.get("reference_config_hash", "")),
                                  str(case.get("candidate_config_hash", ""))},
                "implementation equivalence resolved_config_hash mismatch")
    else:
        fail(f"validation case {case_id} kind invalid")
    return case, validation_kind


def validate_two_adapter(artifacts: ArtifactSet, manifest_fields: dict[str, str]) -> None:
    two = artifacts.two_adapter
    validate_schema("two_adapter_json", two)
    require(two.get("status") == "PASS", "two-adapter status is not PASS")
    fields = provenance_fields(two, "two-adapter")
    for field in ("build.executable_sha256", "adapter.luid_pair"):
        require(fields.get(field) == manifest_fields.get(field), f"stale two-adapter {field}")
    pair = two.get("pair", {})
    require(isinstance(pair, dict), "two-adapter pair object required")
    require(bool_value(pair.get("distinct_luid", True), "pair.distinct_luid"), "two-adapter LUIDs are not distinct")
    runtime = two.get("runtime", {})
    require(isinstance(runtime, dict), "two-adapter runtime object required")
    require(bool_value(runtime.get("passed", False), "runtime.passed"), "two-adapter runtime.passed is false")
    require(not bool_value(runtime.get("fallback", False), "runtime.fallback"), "two-adapter runtime fallback occurred")
    for field in ("color_local_to_shared_bytes", "depth_local_to_shared_bytes",
                  "color_shared_to_local_bytes", "depth_shared_to_local_bytes"):
        require(int_value(runtime, field, minimum=1) > 0, f"two-adapter {field} is zero")
    for field in ("total_secondary_compute_dispatch_count", "max_secondary_graphics_draw_count",
                  "max_secondary_rendered_voxel_count"):
        require(int_value(runtime, field, minimum=1) > 0, f"two-adapter {field} is zero")
    require(int_value(runtime, "particle_transfer_bytes", minimum=0) == 0,
            "two-adapter particle_transfer_bytes must be zero")
    calibrations = runtime.get("queue_calibrations", [])
    require(isinstance(calibrations, list) and calibrations, "queue calibrations are required")
    for calibration in calibrations:
        require(bool_value(calibration.get("valid", False), "queue_calibration.valid"),
                f"invalid queue calibration: {calibration.get('queue', '')}")


def normalize_config(config: dict[str, Any], manifest: dict[str, Any]) -> dict[str, Any]:
    required = {
        "config_id", "pair_id", "session_id", "block_id", "mode", "preset",
        "requested_static_budget", "requested_dynamic_budget",
        "secondary_share", "spatial_lod", "temporal_interval", "repetition",
        "order_index", "block_order_index", "pair_member_order",
    }
    missing = required - config.keys()
    require(not missing, f"manifest config missing fields: {sorted(missing)}")
    require(config["mode"] in MODES, f"config mode invalid: {config['mode']}")
    normalized = dict(config)
    normalized["requested_static_budget_label"] = int(config.get(
        "requested_static_budget_label", config.get("requested_label_count", config.get("requested_static_budget", 0))))
    normalized["randomization_seed"] = int(config.get("randomization_seed", manifest.get("randomization_seed", 0)))
    normalized["warmup_frames"] = int(config.get("warmup_frames", manifest.get("warmup_frames", 0)))
    normalized["measured_frames"] = int(config.get("measured_frames", manifest.get("measured_frames", 0)))
    if "spatial_lod_enabled" not in normalized:
        normalized["spatial_lod_enabled"] = str(config.get("spatial_lod", "")).lower() not in {"", "off", "false", "0"}
    normalized["expected_case_id"] = str(config.get("validation_case_id", expected_case_id(config)))
    normalized["expected_config_hash"] = str(config.get("resolved_config_hash") or canonical_config_hash(normalized))
    return normalized


def validate_matrix_and_executions(artifacts: ArtifactSet, run_id: str, suite: str) -> dict[tuple[str, int], dict[str, Any]]:
    raw_configs = artifacts.manifest.get("configs")
    require(isinstance(raw_configs, list) and raw_configs, "manifest configs are required")
    configs: dict[tuple[str, int], dict[str, Any]] = {}
    for raw_config in raw_configs:
        require(isinstance(raw_config, dict), "manifest config must be object")
        config = normalize_config(raw_config, artifacts.manifest)
        key = (str(config["config_id"]), int(config["repetition"]))
        require(key not in configs, f"duplicate config in manifest: {key}")
        configs[key] = config
    executions: dict[tuple[str, int], dict[str, Any]] = {}
    for execution in artifacts.execution_manifests:
        validate_schema("execution_manifest", execution)
        require(execution.get("run_id") == run_id, "execution manifest run_id mismatch")
        require(execution.get("suite") == suite, "execution manifest suite mismatch")
        require(execution.get("status") == "COMPLETE", f"execution not COMPLETE: {execution.get('config_id')}")
        key = (str(execution.get("config_id", "")), int_value(execution, "repetition", minimum=0))
        require(key not in executions, f"duplicate execution manifest: {key}")
        require(key in configs, f"stale/extra execution manifest: {key}")
        config = configs[key]
        for field, exe_field in (
            ("pair_id", "pair_id"),
            ("session_id", "session_id"),
            ("block_id", "block_id"),
            ("order_index", "randomized_order_index"),
            ("block_order_index", "block_order_index"),
            ("pair_member_order", "pair_member_order"),
            ("randomization_seed", "randomization_seed"),
        ):
            require(str(execution.get(exe_field, "")) == str(config[field]), f"execution {key} {exe_field} mismatch")
        require(int_value(execution, "warmup_frames", minimum=0) == config["warmup_frames"],
                f"execution {key} warmup mismatch")
        require(int_value(execution, "measured_frames", minimum=1) == config["measured_frames"],
                f"execution {key} measured mismatch")
        if "resolved_config_hash" in execution and execution["resolved_config_hash"]:
            config["expected_config_hash"] = str(execution["resolved_config_hash"])
        executions[key] = execution
    require(set(executions) == set(configs), "missing or duplicate execution manifests")
    return configs


def queue_calibration_valid(row: dict[str, str]) -> bool:
    keys = [key for key in row if key.endswith("_calibration_valid")]
    require(keys, "raw frame lacks queue calibration fields")
    for key in keys:
        if row.get(key, "").strip():
            require(bool_value(row[key], key), f"{key} is false")
    monotonic = [key for key in row if key.endswith("_calibration_monotonic")]
    for key in monotonic:
        if row.get(key, "").strip():
            require(bool_value(row[key], key), f"{key} is false")
    return True


def recompute_frame_validity(row: dict[str, str], config: dict[str, Any],
                             manifest_fields: dict[str, str],
                             validation_cases: dict[str, dict[str, Any]]) -> tuple[bool, str]:
    try:
        requested = row.get("requested_mode", "")
        actual = row.get("actual_mode", "")
        require(requested == actual, "requested_mode != actual_mode")
        require(requested == config["mode"], "raw requested mode does not match config")
        require(not row.get("fallback_reason", ""), "fallback_reason is non-empty")
        queue_calibration_valid(row)
        require(int_value(row, "dropped_steps", minimum=0) == 0, "dropped_steps != 0")
        require(int_value(row, "dropped_simulation_steps", minimum=0) == 0, "dropped_simulation_steps != 0")
        require(int_value(row, "requested_fixed_steps", minimum=0) == int_value(row, "executed_fixed_steps", minimum=0),
                "requested/executed fixed steps mismatch")
        require(int_value(row, "actual_static_voxels", minimum=0) >= 0, "bad static count")
        require(int_value(row, "actual_dynamic_voxels", minimum=0) >= 0, "bad dynamic count")
        require(row.get("resolved_config_hash", "") != "", "resolved_config_hash missing")
        require(row["resolved_config_hash"] == config["expected_config_hash"], "resolved_config_hash mismatch")
        validation_case, validation_kind = validate_frame_visual_evidence(
            row, config, manifest_fields, validation_cases)
        row["_matched_validation_case_id"] = str(validation_case.get("case_id", ""))
        row["_matched_validation_kind"] = validation_kind
        row["_matched_validation_camera_hash"] = str(validation_case.get("camera_hash", ""))
        row["_implementation_equivalence_pass"] = (
            "true" if validation_kind == "implementation_equivalence" else "false")
        row["_approximation_fidelity_pass"] = (
            "true" if validation_kind == "approximation_fidelity" else "false")
        require(bool_value(row.get("visual_validation_has_result", "true"), "visual_validation_has_result"),
                "visual validation result missing")
        require(bool_value(row.get("visual_validation_passed", "true"), "visual_validation_passed"),
                "visual validation failed")
        numeric_value(row, "present_to_present_ms", aliases=("cpu_frame_ms",), positive=True)
        numeric_value(row, "cpu_submission_ms", positive=True)
        numeric_value(row, "cpu_total_frame_ms", minimum=0.0)
        backpressure = numeric_value(row, "frame_resource_backpressure_ms", aliases=("cpu_wait_ms",), minimum=0.0)
        submission = numeric_value(row, "cpu_submission_ms", positive=True)
        total_cpu = numeric_value(row, "cpu_total_frame_ms", minimum=0.0)
        require(abs((backpressure + submission) - total_cpu) <= max(EPS, total_cpu * 1.0e-4),
                "cpu_total_frame_ms != backpressure + submission")
        for field in ("critical_path_gpu_ms", "gpu_work_sum_ms"):
            float_value(row, field, positive=True)
        for field in ("primary_compute_ms", "primary_base_graphics_ms", "transfer_ms", "composite_ms"):
            if row.get(field, ""):
                float_value(row, field, minimum=0.0)
        if requested in MULTI_MODES:
            require(int_value(row, "secondary_partition_voxels", minimum=1) > 0,
                    "multi secondary partition empty")
            require(int_value(row, "secondary_draw_calls", minimum=1) > 0,
                    "multi secondary draw calls zero")
            require(float_value(row, "secondary_compute_ms", minimum=0.0) > 0.0,
                    "multi secondary compute is zero")
            require(float_value(row, "secondary_graphics_ms", minimum=0.0) > 0.0,
                    "multi secondary graphics is zero")
            require(float_value(row, "secondary_local_to_shared_copy_ms", minimum=0.0) > 0.0,
                    "multi secondary local-to-shared copy is zero")
            require(float_value(row, "primary_shared_to_local_copy_ms", minimum=0.0) > 0.0,
                    "multi primary shared-to-local copy is zero")
            require(int_value(row, "color_transfer_bytes", minimum=1) > 0,
                    "multi color transfer bytes zero")
            require(int_value(row, "depth_transfer_bytes", minimum=1) > 0,
                    "multi depth transfer bytes zero")
            require(int_value(row, "render_output_transfer_bytes", minimum=1) > 0,
                    "multi render output bytes zero")
        require(int_value(row, "particle_transfer_bytes", minimum=0) == 0, "particle transfer bytes nonzero")
        return True, ""
    except ValidationError as exc:
        return False, str(exc)


def constant_frame_value(frames: list[dict[str, str]], field: str, *,
                         default: Any = "", required: bool = False) -> Any:
    values = [row.get(field, "") for row in frames if row.get(field, "") != ""]
    if not values:
        require(not required, f"missing constant frame field {field}")
        return default
    first = values[0]
    require(all(value == first for value in values), f"{field} varies within run")
    return first


def config_value(config: dict[str, Any], *names: str, default: Any = "") -> Any:
    for name in names:
        if name in config and config[name] not in ("", None):
            return config[name]
    return default


def recompute_runs(raw_frames: list[dict[str, str]], configs: dict[tuple[str, int], dict[str, Any]],
                   manifest_fields: dict[str, str],
                   validation_cases: dict[str, dict[str, Any]]) -> tuple[dict[tuple[str, int], dict[str, Any]], list[str]]:
    frames_by_config: dict[tuple[str, int], list[dict[str, str]]] = {key: [] for key in configs}
    invalid_reasons: list[str] = []
    for row in raw_frames:
        validate_schema("raw_frames", row)
        key = (row.get("config_id", ""), int_value(row, "repetition", minimum=0))
        require(key in configs, f"raw frame references unknown config: {key}")
        config = configs[key]
        valid, reason = recompute_frame_validity(row, config, manifest_fields, validation_cases)
        row["_hostile_valid"] = "true" if valid else "false"
        row["_hostile_reason"] = reason
        if not valid:
            invalid_reasons.append(f"{key}: {reason}")
        frames_by_config[key].append(row)
    runs: dict[tuple[str, int], dict[str, Any]] = {}
    for key, config in configs.items():
        frames = frames_by_config.get(key, [])
        require(len(frames) == config["measured_frames"],
                f"wrong frame count for {key}: got {len(frames)}, expected {config['measured_frames']}")
        valid_frames = [row for row in frames if row["_hostile_valid"] == "true"]
        first_invalid = next((row["_hostile_reason"] for row in frames if row["_hostile_valid"] != "true"), "")
        require(len(valid_frames) == len(frames),
                f"invalid raw frame in COMPLETE suite: {key}: {first_invalid}")
        present = [numeric_value(row, "present_to_present_ms", aliases=("cpu_frame_ms",), positive=True)
                   for row in valid_frames]
        cpu_submission = [numeric_value(row, "cpu_submission_ms", positive=True) for row in valid_frames]
        cpu_total = [numeric_value(row, "cpu_total_frame_ms", minimum=0.0) for row in valid_frames]
        critical = [float_value(row, "critical_path_gpu_ms", positive=True) for row in valid_frames]
        gpu_work = [float_value(row, "gpu_work_sum_ms", positive=True) for row in valid_frames]
        transfer = [optional_float(row, "transfer_ms") for row in valid_frames]
        composite = [optional_float(row, "composite_ms") for row in valid_frames]
        secondary_compute = [optional_float(row, "secondary_compute_ms") for row in valid_frames]
        primary_submitted = [optional_float(row, "primary_submitted_voxels") for row in valid_frames]
        secondary_submitted = [optional_float(row, "secondary_submitted_voxels") for row in valid_frames]
        first = valid_frames[0]
        run_id = constant_frame_value(valid_frames, "run_id", required=True)
        session_id = constant_frame_value(valid_frames, "session_id", required=True)
        pair_id = constant_frame_value(valid_frames, "pair_id", required=True)
        block_id = constant_frame_value(valid_frames, "block_id", required=True)
        requested_mode = constant_frame_value(valid_frames, "requested_mode", required=True)
        actual_mode = constant_frame_value(valid_frames, "actual_mode", required=True)
        resolved_hash = constant_frame_value(valid_frames, "resolved_config_hash", required=True)
        validation_case_id = constant_frame_value(
            valid_frames, "validation_case_id", default=config["expected_case_id"])
        validation_protocol_hash = constant_frame_value(
            valid_frames, "validation_protocol_hash", default=manifest_fields["validation.protocol_sha256"])
        validation_config_hash = constant_frame_value(
            valid_frames, "validation_config_hash", default=manifest_fields["validation.case_config_sha256"])
        validation_camera_hash = constant_frame_value(
            valid_frames, "validation_camera_hash", default=manifest_fields["validation.camera_sha256"])
        matched_validation_case_id = constant_frame_value(
            valid_frames, "_matched_validation_case_id", default=validation_case_id)
        matched_validation_kind = constant_frame_value(
            valid_frames, "_matched_validation_kind", required=True)
        matched_validation_camera_hash = constant_frame_value(
            valid_frames, "_matched_validation_camera_hash", default=validation_camera_hash)
        implementation_equivalence_pass = bool_value(
            constant_frame_value(valid_frames, "_implementation_equivalence_pass", default="false"),
            "implementation_equivalence_pass")
        approximation_fidelity_pass = bool_value(
            constant_frame_value(valid_frames, "_approximation_fidelity_pass", default="false"),
            "approximation_fidelity_pass")
        render_width = int_value({"render_width": constant_frame_value(valid_frames, "render_width", required=True)},
                                 "render_width", minimum=1)
        render_height = int_value({"render_height": constant_frame_value(valid_frames, "render_height", required=True)},
                                  "render_height", minimum=1)
        actual_static_count = int_value(
            {"actual_static_voxels": constant_frame_value(valid_frames, "actual_static_voxels", required=True)},
            "actual_static_voxels", minimum=0)
        actual_dynamic_count = int_value(
            {"actual_dynamic_voxels": constant_frame_value(valid_frames, "actual_dynamic_voxels", required=True)},
            "actual_dynamic_voxels", minimum=0)
        exported_total_voxels = constant_frame_value(valid_frames, "total_voxels", default="")
        if exported_total_voxels != "":
            require(int_value({"total_voxels": exported_total_voxels}, "total_voxels", minimum=0) ==
                    actual_static_count + actual_dynamic_count,
                    "total_voxels does not match actual static + dynamic counts")
        randomization_seed = int_value(
            {"randomization_seed": constant_frame_value(valid_frames, "randomization_seed", required=True)},
            "randomization_seed", minimum=0)
        runs[key] = {
            "run_id": run_id,
            "session_id": session_id,
            "config_id": key[0],
            "pair_id": pair_id,
            "block_id": block_id,
            "repetition": key[1],
            "requested_mode": requested_mode,
            "actual_mode": actual_mode,
            "mode_family": mode_family(requested_mode),
            "preset": str(config_value(config, "preset")),
            "requested_static_budget_label": config_value(
                config, "requested_static_budget_label", "requested_label_count", "requested_static_budget"),
            "requested_static_budget": config_value(config, "requested_static_budget"),
            "requested_dynamic_budget": config_value(config, "requested_dynamic_budget"),
            "secondary_share": config_value(config, "secondary_share"),
            "spatial_lod": config_value(config, "spatial_lod", default=""),
            "spatial_lod_enabled": bool_value(config_value(config, "spatial_lod_enabled", default=False),
                                               "spatial_lod_enabled"),
            "temporal_interval": int(config_value(config, "temporal_interval", default=1)),
            "temporal_policy": str(config_value(config, "temporal_policy", "temporal_mode", default="")),
            "partition_strategy": str(config_value(config, "partition_strategy", default="")),
            "partition_chunk_size": str(config_value(config, "partition_chunk_size", "chunk_size", default="")),
            "resolved_config_hash": resolved_hash,
            "randomization_seed": randomization_seed,
            "validation_case_id": validation_case_id,
            "validation_protocol_hash": validation_protocol_hash,
            "validation_config_hash": validation_config_hash,
            "validation_camera_hash": validation_camera_hash,
            "implementation_equivalence_pass": implementation_equivalence_pass,
            "approximation_fidelity_pass": approximation_fidelity_pass,
            "matched_validation_case_id": matched_validation_case_id,
            "matched_validation_kind": matched_validation_kind,
            "matched_validation_camera_hash": matched_validation_camera_hash,
            "render_width": render_width,
            "render_height": render_height,
            "actual_total_count": actual_static_count + actual_dynamic_count,
            "actual_static_count": actual_static_count,
            "actual_dynamic_count": actual_dynamic_count,
            "total_logical_updated_voxels": sum(int_value(row, "logical_updated_voxel_count", minimum=0)
                                                for row in valid_frames),
            "total_executed_fixed_steps": sum(int_value(row, "executed_fixed_steps", minimum=0)
                                              for row in valid_frames),
            "measured_frame_count": len(frames),
            "valid_frame_count": len(valid_frames),
            "invalid_frame_count": len(frames) - len(valid_frames),
            "mean_present_to_present_ms": mean(present),
            "median_present_to_present_ms": statistics.median(present),
            "p95_present_to_present_ms": percentile(present, 0.95),
            "p99_present_to_present_ms": percentile(present, 0.99),
            "stddev_present_to_present_ms": stddev(present),
            "mean_cpu_submission_ms": mean(cpu_submission),
            "median_cpu_submission_ms": statistics.median(cpu_submission),
            "p95_cpu_submission_ms": percentile(cpu_submission, 0.95),
            "p99_cpu_submission_ms": percentile(cpu_submission, 0.99),
            "stddev_cpu_submission_ms": stddev(cpu_submission),
            "mean_cpu_total_frame_ms": mean(cpu_total),
            "median_cpu_total_frame_ms": statistics.median(cpu_total),
            "stddev_cpu_total_frame_ms": stddev(cpu_total),
            "critical_path_gpu_ms": mean(critical),
            "gpu_work_sum_ms": mean(gpu_work),
            "transfer_ms": mean(transfer),
            "composite_ms": mean(composite),
            "secondary_compute_ms": mean(secondary_compute),
            "primary_submitted_voxels": mean(primary_submitted),
            "secondary_submitted_voxels": mean(secondary_submitted),
        }
    return runs, invalid_reasons


def cross_check_runs(producer: list[dict[str, str]], recomputed: dict[tuple[str, int], dict[str, Any]]) -> None:
    by_key: dict[tuple[str, int], dict[str, str]] = {}
    for row in producer:
        validate_schema("runs", row)
        key = (row.get("config_id") or row.get("requested_mode", ""), int_value(row, "repetition", minimum=0))
        if row.get("config_id"):
            by_key[key] = row
    if not by_key:
        by_key = {(row.get("requested_mode", ""), int_value(row, "repetition", minimum=0)): row for row in producer}
    require(len(producer) == len(recomputed), "runs.csv row count mismatch")
    for key, expected in recomputed.items():
        row = by_key.get(key)
        if row is None:
            candidates = [r for r in producer if r.get("requested_mode") == expected["requested_mode"] and
                          int_value(r, "repetition", minimum=0) == key[1]]
            require(len(candidates) == 1, f"missing producer run row for {key}")
            row = candidates[0]
        require(row.get("valid", "false") == "true", f"producer run claims invalid for {key}")
        for field in ("measured_frame_count", "valid_frame_count", "invalid_frame_count"):
            require(int_value(row, field, minimum=0) == int(expected[field]), f"runs.csv {field} mismatch for {key}")
        numeric_checks = (
            ("mean_present_to_present_ms", ("mean_cpu_frame_ms",)),
            ("mean_cpu_submission_ms", ()),
            ("mean_cpu_total_frame_ms", ()),
            ("critical_path_gpu_ms", ()),
        )
        for field, aliases in numeric_checks:
            if field not in row and aliases and aliases[0] not in row:
                continue
            require(abs(numeric_value(row, field, aliases=aliases, positive=True) - float(expected[field])) <= EPS,
                    f"runs.csv {field} mismatch for {key}")


def pair_key(run: dict[str, Any]) -> tuple[Any, ...]:
    return (
        run["run_id"], run["session_id"], run["pair_id"], run["block_id"], run["repetition"],
        run["randomization_seed"], run["mode_family"], run["preset"],
        run["requested_static_budget_label"], run["requested_static_budget"],
        run["requested_dynamic_budget"], run["secondary_share"], run["spatial_lod_enabled"],
        run["spatial_lod"], run["temporal_interval"], run["temporal_policy"],
        run["partition_strategy"], run["partition_chunk_size"],
        run["render_width"], run["render_height"],
        run["actual_total_count"], run["actual_static_count"], run["actual_dynamic_count"],
        run["total_logical_updated_voxels"], run["total_executed_fixed_steps"],
        run["validation_protocol_hash"], run["validation_camera_hash"],
    )


def validate_single_multi_pair(single: dict[str, Any], multi: dict[str, Any]) -> None:
    allowed = {
        "SingleGpuFull": "MultiGpuFull",
        "SingleGpuTemporalDecimation": "MultiGpuTemporalDecimation",
    }
    require(single["requested_mode"] in allowed, f"invalid single mode for pair: {single['requested_mode']}")
    require(multi["requested_mode"] == allowed[single["requested_mode"]],
            f"{single['requested_mode']} cannot pair with {multi['requested_mode']}")
    for run in (single, multi):
        require(run["requested_mode"] == run["actual_mode"],
                f"requested_mode != actual_mode for {run['config_id']}")
        require(str(run.get("resolved_config_hash", "")) != "",
                f"resolved_config_hash missing for {run['config_id']}")
    invariant_fields = (
        "run_id", "session_id", "pair_id", "block_id", "repetition", "randomization_seed",
        "mode_family", "preset", "requested_static_budget_label", "requested_static_budget",
        "requested_dynamic_budget", "secondary_share", "spatial_lod_enabled", "spatial_lod",
        "temporal_interval", "temporal_policy", "partition_strategy", "partition_chunk_size",
        "render_width", "render_height", "actual_total_count", "actual_static_count",
        "actual_dynamic_count", "total_logical_updated_voxels", "total_executed_fixed_steps",
        "validation_protocol_hash", "validation_camera_hash",
    )
    for field in invariant_fields:
        require(single[field] == multi[field], f"pair invariant mismatch for {field}")


def recompute_pairs(runs: dict[tuple[str, int], dict[str, Any]]) -> list[dict[str, Any]]:
    singles: dict[tuple[Any, ...], dict[str, Any]] = {}
    multis: dict[tuple[Any, ...], dict[str, Any]] = {}
    for run in runs.values():
        if run["requested_mode"] in SINGLE_MODES:
            key = pair_key(run)
            require(key not in singles, f"duplicate Single run for pair key: {run['config_id']}")
            singles[key] = run
        elif run["requested_mode"] in MULTI_MODES:
            key = pair_key(run)
            require(key not in multis, f"duplicate Multi run for pair key: {run['config_id']}")
            multis[key] = run
    pairs: list[dict[str, Any]] = []
    for key, multi in sorted(multis.items(), key=lambda item: str(item[0])):
        single = singles.get(key)
        require(single is not None, f"missing valid matching Single/Multi pair for {multi['config_id']}")
        validate_single_multi_pair(single, multi)
        endpoint_metrics: dict[str, dict[str, float]] = {}
        for endpoint in PAIRED_ENDPOINTS:
            single_value = float(single[endpoint if endpoint in GPU_ENDPOINTS or endpoint in {"transfer_ms", "composite_ms"}
                                       else f"mean_{endpoint}"])
            multi_value = float(multi[endpoint if endpoint in GPU_ENDPOINTS or endpoint in {"transfer_ms", "composite_ms"}
                                      else f"mean_{endpoint}"])
            require(single_value > 0.0 and multi_value > 0.0, f"nonpositive endpoint {endpoint} for pair")
            endpoint_metrics[endpoint] = {
                "single": single_value,
                "multi": multi_value,
                "difference": single_value - multi_value,
                "log_speedup": math.log(single_value / multi_value),
                "speedup": single_value / multi_value,
            }
        for endpoint in ("transfer_ms", "composite_ms"):
            single_value = float(single[endpoint])
            multi_value = float(multi[endpoint])
            endpoint_metrics[endpoint] = {
                "single": single_value,
                "multi": multi_value,
                "difference": single_value - multi_value,
                "log_speedup": math.log(single_value / multi_value)
                if single_value > 0.0 and multi_value > 0.0 else None,
                "speedup": single_value / multi_value
                if single_value > 0.0 and multi_value > 0.0 else None,
            }
        primary = endpoint_metrics[PRIMARY_ENDPOINT]
        pairs.append({
            "session_id": multi["session_id"],
            "pair_id": multi["pair_id"],
            "block_id": multi["block_id"],
            "repetition": multi["repetition"],
            "single_config_id": single["config_id"],
            "multi_config_id": multi["config_id"],
            "single_mode": single["requested_mode"],
            "multi_mode": multi["requested_mode"],
            "endpoint_metrics": endpoint_metrics,
            "single_mean_ms": primary["single"],
            "multi_mean_ms": primary["multi"],
            "paired_difference_ms": primary["difference"],
            "log_speedup": primary["log_speedup"],
            "speedup": primary["speedup"],
            "logical_work_equal": single["total_logical_updated_voxels"] == multi["total_logical_updated_voxels"],
            "executed_steps_equal": single["total_executed_fixed_steps"] == multi["total_executed_fixed_steps"],
            "implementation_equivalence_pass": bool(single["implementation_equivalence_pass"]) and
                                               bool(multi["implementation_equivalence_pass"]),
            "approximation_fidelity_pass": bool(single["approximation_fidelity_pass"]) and
                                           bool(multi["approximation_fidelity_pass"]),
            "single_primary_submitted_voxels": single["primary_submitted_voxels"],
            "multi_primary_submitted_voxels": multi["primary_submitted_voxels"],
            "multi_secondary_submitted_voxels": multi["secondary_submitted_voxels"],
        })
    require(pairs, "no valid complete pairs")
    return pairs


def summarize_endpoint(pairs: list[dict[str, Any]], endpoint: str) -> dict[str, Any]:
    diffs = [float(pair["endpoint_metrics"][endpoint]["difference"]) for pair in pairs]
    logs = [float(pair["endpoint_metrics"][endpoint]["log_speedup"]) for pair in pairs]
    diff_ci = ci95(diffs)
    log_ci = ci95(logs)
    speedup_ci = {
        "mean": math.exp(log_ci["mean"]) if log_ci["mean"] is not None else None,
        "lower": math.exp(log_ci["lower"]) if log_ci["lower"] is not None else None,
        "upper": math.exp(log_ci["upper"]) if log_ci["upper"] is not None else None,
        "status": log_ci["status"],
    }
    direction = "NOT_MEASURED"
    if diff_ci["status"] == "MEASURED" and diff_ci["lower"] is not None and diff_ci["upper"] is not None:
        if diff_ci["lower"] > 0.0:
            direction = "MULTI_LOWER"
        elif diff_ci["upper"] < 0.0:
            direction = "MULTI_HIGHER"
        else:
            direction = "INCONCLUSIVE"
    return {
        "endpoint": endpoint,
        "paired_n": len(pairs),
        "mean_difference_ms": diff_ci["mean"],
        "median_difference_ms": statistics.median(diffs) if diffs else None,
        "difference_ms_ci95": diff_ci,
        "mean_log_speedup": log_ci["mean"],
        "log_speedup_ci95": log_ci,
        "speedup_ci95": speedup_ci,
        "effect_direction": direction,
        "coefficient_of_variation": coefficient_of_variation(diffs),
    }


def summarize_pairs(pairs: list[dict[str, Any]]) -> dict[str, Any]:
    endpoints = {endpoint: summarize_endpoint(pairs, endpoint) for endpoint in PAIRED_ENDPOINTS}
    primary = endpoints[PRIMARY_ENDPOINT]
    return {
        "primary_endpoint": PRIMARY_ENDPOINT,
        "pair_count": len(pairs),
        "endpoints": endpoints,
        "difference_ms_ci95": primary["difference_ms_ci95"],
        "log_speedup_ci95": primary["log_speedup_ci95"],
        "speedup_ci95": primary["speedup_ci95"],
        "multiplicity_control": {
            "confirmatory": "predeclared H1 uses dedicated SingleGpuFull -> MultiGpuFull present_to_present_ms contrasts",
            "exploratory_per_config": "Holm/FDR control required before per-config claims; this summary marks per-config rows exploratory",
        },
    }


H1_MATCH_FIELDS = (
    "run_id", "session_id", "repetition", "randomization_seed", "preset",
    "requested_static_budget_label", "requested_static_budget", "requested_dynamic_budget",
    "secondary_share", "spatial_lod_enabled", "spatial_lod", "temporal_interval",
    "temporal_policy", "partition_strategy", "partition_chunk_size", "render_width",
    "render_height", "actual_total_count", "actual_static_count", "actual_dynamic_count",
    "total_logical_updated_voxels", "total_executed_fixed_steps", "validation_protocol_hash",
    "validation_camera_hash",
)

H2_MATCH_FIELDS = (
    "run_id", "session_id", "repetition", "randomization_seed", "preset",
    "requested_static_budget_label", "requested_static_budget", "requested_dynamic_budget",
    "secondary_share", "spatial_lod_enabled", "spatial_lod", "partition_strategy",
    "partition_chunk_size", "render_width", "render_height", "actual_total_count",
    "actual_static_count", "actual_dynamic_count", "validation_protocol_hash",
    "validation_camera_hash",
)

H3_MATCH_FIELDS = (
    "run_id", "session_id", "repetition", "randomization_seed", "requested_mode",
    "preset", "requested_static_budget_label", "requested_static_budget",
    "requested_dynamic_budget", "secondary_share", "temporal_interval", "temporal_policy",
    "partition_strategy", "partition_chunk_size", "render_width", "render_height",
    "validation_protocol_hash", "validation_camera_hash",
)


def run_match_key(run: dict[str, Any], fields: tuple[str, ...]) -> tuple[Any, ...]:
    return tuple(run.get(field, "") for field in fields)


def require_unique_group(groups: dict[tuple[Any, ...], dict[str, Any]],
                         key: tuple[Any, ...],
                         run: dict[str, Any],
                         label: str) -> None:
    require(key not in groups, f"duplicate {label} contrast run for key: {run['config_id']}")
    groups[key] = run


def run_metric(run: dict[str, Any], endpoint: str) -> float:
    if endpoint == "present_to_present_ms":
        return float(run["mean_present_to_present_ms"])
    return float(run[endpoint])


def submitted_total(run: dict[str, Any]) -> float:
    return float(run["primary_submitted_voxels"]) + float(run["secondary_submitted_voxels"])


def is_approximation_fidelity_pass(run: dict[str, Any]) -> bool:
    return bool(run.get("approximation_fidelity_pass")) and \
        str(run.get("matched_validation_kind", "")) == "approximation_fidelity"


def build_h1_contrasts(runs: dict[tuple[str, int], dict[str, Any]]) -> list[dict[str, Any]]:
    singles: dict[tuple[Any, ...], dict[str, Any]] = {}
    multis: dict[tuple[Any, ...], dict[str, Any]] = {}
    for run in runs.values():
        if run["requested_mode"] == "SingleGpuFull":
            require_unique_group(singles, run_match_key(run, H1_MATCH_FIELDS), run, "H1 SingleGpuFull")
        elif run["requested_mode"] == "MultiGpuFull":
            require_unique_group(multis, run_match_key(run, H1_MATCH_FIELDS), run, "H1 MultiGpuFull")
    contrasts: list[dict[str, Any]] = []
    for key, multi in sorted(multis.items(), key=lambda item: str(item[0])):
        single = singles.get(key)
        if single is None:
            continue
        require(single["requested_mode"] == single["actual_mode"], f"H1 fallback in {single['config_id']}")
        require(multi["requested_mode"] == multi["actual_mode"], f"H1 fallback in {multi['config_id']}")
        single_value = run_metric(single, PRIMARY_ENDPOINT)
        multi_value = run_metric(multi, PRIMARY_ENDPOINT)
        contrasts.append({
            "single_config_id": single["config_id"],
            "multi_config_id": multi["config_id"],
            "single_mode": single["requested_mode"],
            "multi_mode": multi["requested_mode"],
            "difference": single_value - multi_value,
            "log_speedup": math.log(single_value / multi_value),
            "speedup": single_value / multi_value,
            "validation_complete": bool(single["implementation_equivalence_pass"]) and
                                   bool(multi["implementation_equivalence_pass"]),
        })
    return contrasts


def build_h1_contrasts_from_pairs(pairs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    contrasts: list[dict[str, Any]] = []
    for pair in pairs:
        if pair["single_mode"] != "SingleGpuFull" or pair["multi_mode"] != "MultiGpuFull":
            continue
        metrics = pair["endpoint_metrics"][PRIMARY_ENDPOINT]
        contrasts.append({
            "single_config_id": pair["single_config_id"],
            "multi_config_id": pair["multi_config_id"],
            "single_mode": pair["single_mode"],
            "multi_mode": pair["multi_mode"],
            "difference": metrics["difference"],
            "log_speedup": metrics["log_speedup"],
            "speedup": metrics["speedup"],
            "validation_complete": bool(pair["implementation_equivalence_pass"]),
        })
    return contrasts


def build_h2_contrasts(runs: dict[tuple[str, int], dict[str, Any]]) -> tuple[list[dict[str, Any]], list[str]]:
    full: dict[tuple[Any, ...], dict[str, Any]] = {}
    temporal: dict[tuple[Any, ...], dict[str, Any]] = {}
    for run in runs.values():
        if run["requested_mode"] == "MultiGpuFull":
            require_unique_group(full, run_match_key(run, H2_MATCH_FIELDS), run, "H2 MultiGpuFull")
        elif run["requested_mode"] == "MultiGpuTemporalDecimation":
            require_unique_group(temporal, run_match_key(run, H2_MATCH_FIELDS), run, "H2 MultiGpuTemporalDecimation")
    contrasts: list[dict[str, Any]] = []
    missing: list[str] = []
    for key, full_run in full.items():
        if key not in temporal:
            missing.append(f"missing MultiGpuTemporalDecimation mate for {full_run['config_id']}")
    for key, temporal_run in temporal.items():
        if key not in full:
            missing.append(f"missing MultiGpuFull mate for {temporal_run['config_id']}")
    for key, temporal_run in sorted(temporal.items(), key=lambda item: str(item[0])):
        full_run = full.get(key)
        if full_run is None:
            continue
        require(full_run["requested_mode"] == full_run["actual_mode"], f"H2 fallback in {full_run['config_id']}")
        require(temporal_run["requested_mode"] == temporal_run["actual_mode"],
                f"H2 fallback in {temporal_run['config_id']}")
        full_value = run_metric(full_run, "secondary_compute_ms")
        temporal_value = run_metric(temporal_run, "secondary_compute_ms")
        require(full_value > 0.0 and temporal_value > 0.0, "H2 secondary_compute_ms must be positive")
        contrasts.append({
            "full_config_id": full_run["config_id"],
            "temporal_config_id": temporal_run["config_id"],
            "difference": full_value - temporal_value,
            "log_speedup": math.log(full_value / temporal_value),
            "speedup": full_value / temporal_value,
            "logical_work_equal": full_run["total_logical_updated_voxels"] == temporal_run["total_logical_updated_voxels"],
            "executed_steps_equal": full_run["total_executed_fixed_steps"] == temporal_run["total_executed_fixed_steps"],
            "approximation_fidelity_pass": is_approximation_fidelity_pass(temporal_run),
        })
    return contrasts, missing


def build_h3_contrasts(runs: dict[tuple[str, int], dict[str, Any]]) -> tuple[list[dict[str, Any]], list[str]]:
    lod_off: dict[tuple[Any, ...], dict[str, Any]] = {}
    lod_on: dict[tuple[Any, ...], dict[str, Any]] = {}
    for run in runs.values():
        key = run_match_key(run, H3_MATCH_FIELDS)
        if bool(run.get("spatial_lod_enabled")):
            require_unique_group(lod_on, key, run, "H3 LOD-on")
        else:
            require_unique_group(lod_off, key, run, "H3 LOD-off")
    contrasts: list[dict[str, Any]] = []
    missing: list[str] = []
    for key, off_run in lod_off.items():
        if key not in lod_on:
            missing.append(f"missing LOD-on mate for {off_run['config_id']}")
    for key, on_run in lod_on.items():
        if key not in lod_off:
            missing.append(f"missing LOD-off mate for {on_run['config_id']}")
    for key, on_run in sorted(lod_on.items(), key=lambda item: str(item[0])):
        off_run = lod_off.get(key)
        if off_run is None:
            continue
        off_submitted = submitted_total(off_run)
        on_submitted = submitted_total(on_run)
        contrasts.append({
            "mode": on_run["requested_mode"],
            "lod_off_config_id": off_run["config_id"],
            "lod_on_config_id": on_run["config_id"],
            "lod_off_submitted_total": off_submitted,
            "lod_on_submitted_total": on_submitted,
            "difference": off_submitted - on_submitted,
            "monotonic_non_increase": on_submitted <= off_submitted + EPS,
            "simulation_counts_unchanged": (
                off_run["actual_total_count"] == on_run["actual_total_count"] and
                off_run["actual_static_count"] == on_run["actual_static_count"] and
                off_run["actual_dynamic_count"] == on_run["actual_dynamic_count"]
            ),
            "logical_work_equal": off_run["total_logical_updated_voxels"] == on_run["total_logical_updated_voxels"],
            "executed_steps_equal": off_run["total_executed_fixed_steps"] == on_run["total_executed_fixed_steps"],
            "approximation_fidelity_pass": is_approximation_fidelity_pass(on_run),
        })
    return contrasts, missing


def summarize_paired_difference(contrasts: list[dict[str, Any]], endpoint: str) -> dict[str, Any]:
    diffs = [float(contrast["difference"]) for contrast in contrasts]
    logs = [float(contrast["log_speedup"]) for contrast in contrasts if contrast.get("log_speedup") is not None]
    diff_ci = ci95(diffs)
    log_ci = ci95(logs)
    speedup_ci = {
        "mean": math.exp(log_ci["mean"]) if log_ci["mean"] is not None else None,
        "lower": math.exp(log_ci["lower"]) if log_ci["lower"] is not None else None,
        "upper": math.exp(log_ci["upper"]) if log_ci["upper"] is not None else None,
        "status": log_ci["status"],
    }
    return {
        "endpoint": endpoint,
        "paired_n": len(contrasts),
        "mean_difference": diff_ci["mean"],
        "median_difference": statistics.median(diffs) if diffs else None,
        "difference_ci95": diff_ci,
        "mean_log_speedup": log_ci["mean"],
        "log_speedup_ci95": log_ci,
        "speedup_ci95": speedup_ci,
        "coefficient_of_variation": coefficient_of_variation(diffs),
    }


def ci_direction(ci: dict[str, Any], positive_label: str, negative_label: str) -> str:
    if ci["status"] != "MEASURED" or ci["lower"] is None or ci["upper"] is None:
        return "NOT_MEASURED"
    if ci["lower"] > 0.0:
        return positive_label
    if ci["upper"] < 0.0:
        return negative_label
    return "INCONCLUSIVE"


def support_reason(ok: bool, reason: str) -> str:
    return "" if ok else reason


def evaluate_h1(runs: dict[tuple[str, int], dict[str, Any]]) -> dict[str, Any]:
    contrasts = build_h1_contrasts(runs)
    return evaluate_h1_contrasts(contrasts)


def evaluate_h1_from_pairs(pairs: list[dict[str, Any]]) -> dict[str, Any]:
    contrasts = build_h1_contrasts_from_pairs(pairs)
    return evaluate_h1_contrasts(contrasts)


def evaluate_h1_contrasts(contrasts: list[dict[str, Any]]) -> dict[str, Any]:
    stats = summarize_paired_difference(contrasts, PRIMARY_ENDPOINT)
    direction = ci_direction(stats["difference_ci95"], "MULTI_LOWER", "MULTI_HIGHER")
    validation_complete = bool(contrasts) and all(contrast["validation_complete"] for contrast in contrasts)
    support = (
        stats["paired_n"] >= 2 and
        stats["difference_ci95"]["status"] == "MEASURED" and
        stats["difference_ci95"]["lower"] is not None and
        stats["difference_ci95"]["lower"] > 0.0 and
        validation_complete
    )
    reasons = [
        support_reason(stats["paired_n"] >= 2, "paired_n < 2"),
        support_reason(stats["difference_ci95"]["status"] == "MEASURED", "CI is NOT_MEASURED"),
        support_reason(direction == "MULTI_LOWER", "CI does not show Multi lower end-to-end latency"),
        support_reason(validation_complete, "validation/preflight/strict analysis completeness not satisfied"),
    ]
    return {
        "claim": "MultiGpuFull improves paired end-to-end present_to_present_ms versus SingleGpuFull",
        "contrast": "SingleGpuFull -> MultiGpuFull",
        "endpoint": PRIMARY_ENDPOINT,
        "paired_n": stats["paired_n"],
        "matched_factor_fields": list(H1_MATCH_FIELDS),
        "excluded_modes": ["SingleGpuTemporalDecimation", "MultiGpuTemporalDecimation"],
        "statistics": stats,
        "effect_direction": direction,
        "validation_requirements": ["strict analysis PASS", "matching validation/preflight evidence", "no fallback"],
        "reason": "; ".join(reason for reason in reasons if reason),
        "result": "SUPPORT" if support else ("NOT_MEASURED" if stats["paired_n"] < 2 else "NOT_SUPPORTED"),
    }


def evaluate_h2(runs: dict[tuple[str, int], dict[str, Any]]) -> dict[str, Any]:
    contrasts, missing = build_h2_contrasts(runs)
    stats = summarize_paired_difference(contrasts, "secondary_compute_ms")
    direction = ci_direction(stats["difference_ci95"], "TEMPORAL_LOWER", "TEMPORAL_HIGHER")
    logical_equal = bool(contrasts) and all(contrast["logical_work_equal"] for contrast in contrasts)
    steps_equal = bool(contrasts) and all(contrast["executed_steps_equal"] for contrast in contrasts)
    fidelity = bool(contrasts) and all(contrast["approximation_fidelity_pass"] for contrast in contrasts)
    ci_excludes_zero = direction in {"TEMPORAL_LOWER", "TEMPORAL_HIGHER"}
    support = (
        stats["paired_n"] >= 2 and
        stats["difference_ci95"]["status"] == "MEASURED" and
        ci_excludes_zero and
        logical_equal and
        steps_equal and
        fidelity and
        not missing
    )
    reasons = [
        support_reason(stats["paired_n"] >= 2, "paired_n < 2"),
        support_reason(stats["difference_ci95"]["status"] == "MEASURED", "CI is NOT_MEASURED"),
        support_reason(ci_excludes_zero, "secondary_compute_ms CI crosses zero"),
        support_reason(logical_equal, "logical work differs"),
        support_reason(steps_equal, "executed fixed steps differ"),
        support_reason(fidelity, "Temporal approximation-fidelity PASS missing"),
        support_reason(not missing, "; ".join(missing[:3])),
    ]
    return {
        "claim": "Temporal decimation changes Multi secondary compute work",
        "contrast": "MultiGpuFull -> MultiGpuTemporalDecimation",
        "endpoint": "secondary_compute_ms",
        "paired_n": stats["paired_n"],
        "matched_factor_fields": list(H2_MATCH_FIELDS),
        "excluded_modes": ["SingleGpuFull", "SingleGpuTemporalDecimation"],
        "statistics": stats,
        "effect_direction": direction,
        "logical_work_equal": logical_equal,
        "executed_steps_equal": steps_equal,
        "approximation_fidelity_pass": fidelity,
        "validation_requirements": ["strict analysis PASS", "Temporal approximation-fidelity PASS", "no fallback"],
        "contrast_coverage_complete": not missing,
        "missing_contrasts": missing,
        "reason": "; ".join(reason for reason in reasons if reason),
        "result": "SUPPORT" if support else ("NOT_MEASURED" if stats["paired_n"] < 2 else "NOT_SUPPORTED"),
    }


def evaluate_h3(runs: dict[tuple[str, int], dict[str, Any]]) -> dict[str, Any]:
    contrasts, missing = build_h3_contrasts(runs)
    reductions = [float(contrast["difference"]) for contrast in contrasts]
    reduction_ci = ci95(reductions)
    monotonic = bool(contrasts) and all(contrast["monotonic_non_increase"] for contrast in contrasts)
    counts_same = bool(contrasts) and all(contrast["simulation_counts_unchanged"] for contrast in contrasts)
    logical_equal = bool(contrasts) and all(contrast["logical_work_equal"] for contrast in contrasts)
    steps_equal = bool(contrasts) and all(contrast["executed_steps_equal"] for contrast in contrasts)
    fidelity = bool(contrasts) and all(contrast["approximation_fidelity_pass"] for contrast in contrasts)
    support = bool(contrasts) and monotonic and counts_same and logical_equal and steps_equal and fidelity and not missing
    reasons = [
        support_reason(bool(contrasts), "no matched LOD off/on contrasts"),
        support_reason(monotonic, "LOD-on submitted count exceeds LOD-off"),
        support_reason(counts_same, "actual simulation counts changed"),
        support_reason(logical_equal, "logical updated work changed"),
        support_reason(steps_equal, "executed fixed steps changed"),
        support_reason(fidelity, "LOD approximation-fidelity PASS missing"),
        support_reason(not missing, "; ".join(missing[:3])),
    ]
    return {
        "claim": "LOD-on submitted counts are monotonic/non-increasing versus LOD-off without simulation-work changes",
        "contrast": "LOD off -> LOD on within each requested mode",
        "endpoint": "primary_submitted_voxels + secondary_submitted_voxels",
        "paired_n": len(contrasts),
        "matched_factor_fields": list(H3_MATCH_FIELDS),
        "excluded_modes": [],
        "statistics": {
            "paired_n": len(contrasts),
            "mean_reduction": reduction_ci["mean"],
            "median_reduction": statistics.median(reductions) if reductions else None,
            "reduction_ci95": reduction_ci,
        },
        "effect_direction": "LOD_ON_NON_INCREASING" if monotonic else "LOD_ON_HIGHER",
        "monotonic_non_increase": monotonic,
        "simulation_counts_unchanged": counts_same,
        "logical_work_equal": logical_equal,
        "executed_steps_equal": steps_equal,
        "approximation_fidelity_pass": fidelity,
        "validation_requirements": ["strict analysis PASS", "LOD-on approximation-fidelity PASS", "no fallback"],
        "contrast_coverage_complete": not missing,
        "missing_contrasts": missing,
        "reason": "; ".join(reason for reason in reasons if reason),
        "result": "SUPPORT" if support else ("NOT_MEASURED" if not contrasts else "NOT_SUPPORTED"),
        "contrasts": contrasts,
    }


def hypothesis_results(runs: dict[tuple[str, int], dict[str, Any]],
                       pairs: list[dict[str, Any]]) -> dict[str, Any]:
    transfer_ratios = []
    composite_copy_shares = []
    for pair in pairs:
        metrics = pair["endpoint_metrics"]
        critical = metrics["critical_path_gpu_ms"]["multi"]
        transfer = metrics["transfer_ms"]["multi"]
        composite = metrics["composite_ms"]["multi"]
        if critical > 0.0:
            transfer_ratios.append(transfer / critical)
            composite_copy_shares.append((transfer + composite) / critical)
    rq3_transfer_ci = ci95(transfer_ratios)
    rq3_composite_copy_ci = ci95(composite_copy_shares)
    transfer_dominated = (
        rq3_transfer_ci["status"] == "MEASURED" and
        rq3_transfer_ci["lower"] is not None and
        rq3_transfer_ci["lower"] >= 0.5
    )
    return {
        "H1": evaluate_h1_from_pairs(pairs),
        "H2": evaluate_h2(runs),
        "H3": evaluate_h3(runs),
        "RQ3": {
            "claim": "Transfers dominate the Multi critical path under the predeclared >=0.5 transfer_ms/critical_path_ms criterion",
            "contrast": "valid Single/Multi pairs, Multi transfer/composite share",
            "endpoint": "transfer_ms / critical_path_gpu_ms",
            "paired_n": len(transfer_ratios),
            "transfer_ms_over_critical_path_ms_ci95": rq3_transfer_ci,
            "composite_plus_copy_over_critical_path_ms_ci95": rq3_composite_copy_ci,
            "transfer_dominated_criterion": "lower_ci_bound >= 0.5",
            "effect_direction": "TRANSFER_DOMINATED" if transfer_dominated else "NOT_TRANSFER_DOMINATED",
            "reason": "" if transfer_dominated else "transfer dominance CI lower bound is not >= 0.5",
            "result": "TRANSFER_DOMINATED" if transfer_dominated else "NOT_TRANSFER_DOMINATED",
        },
    }


def validate(root: Path) -> dict[str, Any]:
    artifacts = load_artifacts(root)
    run_id, suite, manifest_fields = validate_top_level(artifacts)
    configs = validate_matrix_and_executions(artifacts, run_id, suite)
    validation_cases = validate_validation_artifacts(artifacts, manifest_fields, list(configs.values()))
    validate_two_adapter(artifacts, manifest_fields)
    require(artifacts.manifest.get("status") == "COMPLETE", "manifest status must be COMPLETE")
    require(artifacts.manifest.get("created_utc"), "created_utc is required")
    require(artifacts.manifest.get("start_utc"), "start_utc is required")
    require(artifacts.manifest.get("end_utc"), "end_utc is required")
    recomputed_runs, invalid_reasons = recompute_runs(
        artifacts.raw_frames, configs, manifest_fields, validation_cases)
    require(not invalid_reasons, "invalid raw frames in COMPLETE suite: " + "; ".join(invalid_reasons[:3]))
    cross_check_runs(artifacts.runs, recomputed_runs)
    pairs = recompute_pairs(recomputed_runs)
    summary = summarize_pairs(pairs)
    result = {
        "schema": "mgpu_voxel_analysis_summary.v2",
        "status": "PASS",
        "run_id": run_id,
        "suite": suite,
        "primary_endpoint": PRIMARY_ENDPOINT,
        "artifact_checksums": artifacts.checksums,
        "recomputed_runs": list(recomputed_runs.values()),
        "recomputed_pairs": pairs,
        "statistics": summary,
        "hypothesis_results": hypothesis_results(recomputed_runs, pairs),
    }
    return result


def write_summary(root: Path, result: dict[str, Any]) -> Path:
    path = root / "analysis_summary.v2.json"
    text = json.dumps(result, indent=2, sort_keys=True, allow_nan=False)
    path.write_text(text + "\n", encoding="utf-8")
    checksum = sha256_file(path)
    (root / "analysis_summary.v2.sha256").write_text(checksum + "  analysis_summary.v2.json\n", encoding="utf-8")
    return path


class HostileFixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.fields = {
            "build.executable_sha256": "exe",
            "build.shader_bytecode_set_sha256": "shader",
            "adapter.luid_pair": "primary->secondary",
            "validation.protocol_sha256": "protocol",
            "validation.case_config_sha256": "case-config-set",
            "validation.camera_sha256": "camera-set",
            "render.resolution": "1920x1080",
            "render.color_format": "R8G8B8A8_UNORM",
            "render.depth_format": "R32_FLOAT",
            "render.sample_count": "1",
        }
        self.configs = [
            self.config("c_single", "SingleGpuFull", 0),
            self.config("c_multi", "MultiGpuFull", 1),
        ]

    def config(self, config_id: str, mode: str, order: int) -> dict[str, Any]:
        return {
            "config_id": config_id,
            "pair_id": "pair0",
            "session_id": "session0",
            "block_id": "block0",
            "mode": mode,
            "preset": "Low",
            "requested_static_budget_label": 100000,
            "requested_static_budget": 100000,
            "requested_dynamic_budget": 25000,
            "secondary_share": 0.5,
            "spatial_lod": "Off",
            "temporal_interval": 1,
            "repetition": 0,
            "order_index": order,
            "block_order_index": 0,
            "pair_member_order": order,
            "randomization_seed": 123,
            "warmup_frames": 1,
            "measured_frames": 2,
            "resolved_config_hash": "single-full-hash" if mode == "SingleGpuFull" else "multi-full-hash",
        }

    def write(self) -> None:
        manifest = {
            "schema": "mgpu_voxel_benchmark_manifest.v2",
            "suite": "Smoke",
            "run_id": "run0",
            "status": "COMPLETE",
            "created_utc": "2026-06-22T00:00:00Z",
            "start_utc": "2026-06-22T00:00:01Z",
            "end_utc": "2026-06-22T00:00:02Z",
            "randomization_seed": 123,
            "warmup_frames": 1,
            "measured_frames": 2,
            "provenance": {"schema": "mgpu_research_provenance.v1", "fields": self.fields},
            "configs": self.configs,
        }
        (self.root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        (self.root / "environment.json").write_text(json.dumps({
            "schema": "mgpu_voxel_environment.v2", "suite": "Smoke", "run_id": "run0",
        }), encoding="utf-8")
        cases = [{
            "case_id": "full_lod_off|equivalence|steady_240",
            "validation_kind": "implementation_equivalence",
            "status": "PASS",
            "passed": True,
            "blocked": False,
            "protocol_hash": "protocol",
            "reference_config_hash": "single-full-hash",
            "candidate_config_hash": "multi-full-hash",
            "config_hash": "case-config-set",
            "camera_hash": "case-camera-full",
            "requested_single_mode": "SingleGpuFull",
            "actual_single_mode": "SingleGpuFull",
            "requested_multi_mode": "MultiGpuFull",
            "actual_multi_mode": "MultiGpuFull",
        }]
        (self.root / "voxel_visual_validation.json").write_text(json.dumps({
            "schema": "mgpu_voxel_visual_validation.v2",
            "validation_run_id": "validation0",
            "aggregate_status": "PASS",
            "aggregate_passed": True,
            "provenance": {"schema": "mgpu_research_provenance.v1", "fields": self.fields},
            "cases": cases,
        }), encoding="utf-8")
        self.write_csv("voxel_visual_validation.csv", ["schema", "run_id", "suite", "case_id", "status"], [
            ["mgpu_voxel_visual_validation.v2", "run0", "Smoke", "full_lod_off|equivalence|steady_240", "PASS"],
        ])
        (self.root / "two_adapter_preflight.json").write_text(json.dumps({
            "schema": "mgpu_voxel_two_adapter_preflight.v2",
            "verification_run_id": "two0",
            "status": "PASS",
            "provenance": {"schema": "mgpu_research_provenance.v1", "fields": self.fields},
            "pair": {"distinct_luid": True},
            "runtime": {
                "passed": True,
                "fallback": False,
                "color_local_to_shared_bytes": 100,
                "depth_local_to_shared_bytes": 100,
                "color_shared_to_local_bytes": 100,
                "depth_shared_to_local_bytes": 100,
                "total_secondary_compute_dispatch_count": 2,
                "max_secondary_graphics_draw_count": 2,
                "max_secondary_rendered_voxel_count": 100,
                "particle_transfer_bytes": 0,
                "queue_calibrations": [{"queue": "gpu0.graphics", "valid": True}],
            },
        }), encoding="utf-8")
        for cfg in self.configs:
            (self.root / f"VoxelBenchmark_Smoke_{cfg['config_id']}_manifest.json").write_text(json.dumps({
                "schema": "mgpu_voxel_execution_manifest.v2",
                "suite": "Smoke",
                "run_id": "run0",
                "status": "COMPLETE",
                "config_id": cfg["config_id"],
                "pair_id": cfg["pair_id"],
                "session_id": cfg["session_id"],
                "block_id": cfg["block_id"],
                "repetition": cfg["repetition"],
                "randomized_order_index": cfg["order_index"],
                "block_order_index": cfg["block_order_index"],
                "pair_member_order": cfg["pair_member_order"],
                "randomization_seed": cfg["randomization_seed"],
                "warmup_frames": cfg["warmup_frames"],
                "measured_frames": cfg["measured_frames"],
                "resolved_config_hash": cfg["resolved_config_hash"],
                "start_utc": "2026-06-22T00:00:01Z",
                "end_utc": "2026-06-22T00:00:02Z",
            }), encoding="utf-8")
        raw_rows = []
        for cfg in self.configs:
            for frame in range(2):
                raw_rows.append(self.raw_frame(cfg, frame))
        self.write_csv("raw_frames.csv", RAW_HEADER, raw_rows)
        self.write_csv("runs.csv", RUNS_HEADER, [
            self.run_row(self.configs[0], 10.0),
            self.run_row(self.configs[1], 5.0),
        ])
        self.write_csv("paired_runs.csv", ["schema", "run_id", "suite", "session_id", "pair_id", "block_id", "repetition"], [
            ["mgpu_voxel_paired_runs.v2", "run0", "Smoke", "session0", "pair0", "block0", "0"],
        ])
        self.write_csv("invalid_records.csv", ["schema", "run_id", "suite", "reason"], [
            ["mgpu_voxel_invalid_records.v2", "run0", "Smoke", ""],
        ])
        self.write_csv("telemetry.csv", ["schema", "run_id", "suite", "config_id"], [
            ["mgpu_voxel_run_telemetry.v2", "run0", "Smoke", "c_single"],
        ])

    def write_csv(self, name: str, header: list[str], rows: list[list[Any]]) -> None:
        with (self.root / name).open("w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(header)
            writer.writerows(rows)

    def raw_frame(self, cfg: dict[str, Any], frame: int) -> list[Any]:
        multi = cfg["mode"] in MULTI_MODES
        return [
            "mgpu_voxel_raw_frame.v2", "run0", "Smoke", frame, cfg["session_id"], cfg["config_id"],
            cfg["pair_id"], cfg["block_id"], cfg["repetition"], cfg["order_index"],
            cfg["block_order_index"], cfg["pair_member_order"], cfg["randomization_seed"],
            cfg["mode"], cfg["mode"], "", "MixedStaticAndDynamic", "Valid matching benchmark configuration",
            "Benchmark", 1, 1, 0, 0, 0, 100000, 25000, 125000, 100000, 25000,
            100000, 25000 if multi else 0, 10, 1920, 1080,
            cfg["resolved_config_hash"], "full_lod_off|equivalence|steady_240",
            "protocol", cfg["resolved_config_hash"], "case-camera-full", "true", "true",
            "true", "true", "true", "true", "true", "true",
            0.5, 1.5, 2.0, 10.0 if not multi else 5.0,
            8.0 if not multi else 4.0, 9.0 if not multi else 5.0,
            1.0, 2.0, 1.0 if multi else 0.0, 1.0 if multi else 0.0,
            0.5 if multi else 0.0, 0.5 if multi else 0.0, 1.0 if multi else 0.0, 1.0,
            100 if multi else 0, 100 if multi else 0, 0, 200 if multi else 0,
            2 if multi else 0, 75000 if multi else 100000, 25000 if multi else 0, "true", "",
        ]

    def run_row(self, cfg: dict[str, Any], mean_ms: float) -> list[Any]:
        return [
            "mgpu_voxel_runs.v2", "run0", "Smoke", cfg["session_id"], cfg["config_id"], cfg["pair_id"],
            cfg["block_id"], cfg["mode"], cfg["mode"], cfg["repetition"], "true", "",
            125000, 100000, 25000, cfg["resolved_config_hash"], 2, 2, 0, mean_ms, 1.5, 2.0,
            8.0 if mean_ms == 10.0 else 4.0,
        ]


RAW_HEADER = [
    "schema", "run_id", "suite", "frame_index", "session_id", "config_id", "pair_id", "block_id",
    "repetition", "randomized_order_index", "block_order_index", "pair_member_order", "randomization_seed",
    "requested_mode", "actual_mode", "fallback_reason", "profile", "benchmark_config_class", "scheduler_mode",
    "requested_fixed_steps", "executed_fixed_steps", "dropped_steps", "dropped_simulation_steps",
    "dropped_simulation_time", "actual_static_voxels", "actual_dynamic_voxels", "total_voxels",
    "static_budget", "dynamic_budget", "primary_partition_voxels", "secondary_partition_voxels",
    "logical_updated_voxel_count", "render_width", "render_height", "resolved_config_hash",
    "visual_validation_case_id", "visual_validation_protocol_hash",
    "visual_validation_config_hash", "visual_validation_camera_hash",
    "visual_validation_has_result", "visual_validation_passed",
    "primary_compute_queue_calibration_valid", "primary_compute_queue_calibration_monotonic",
    "primary_graphics_queue_calibration_valid", "primary_graphics_queue_calibration_monotonic",
    "secondary_compute_queue_calibration_valid", "secondary_graphics_queue_calibration_valid",
    "frame_resource_backpressure_ms", "cpu_submission_ms", "cpu_total_frame_ms",
    "present_to_present_ms", "critical_path_gpu_ms", "gpu_work_sum_ms", "primary_compute_ms",
    "primary_base_graphics_ms", "secondary_compute_ms", "secondary_graphics_ms",
    "secondary_local_to_shared_copy_ms", "primary_shared_to_local_copy_ms", "transfer_ms", "composite_ms",
    "color_transfer_bytes", "depth_transfer_bytes", "particle_transfer_bytes", "render_output_transfer_bytes",
    "secondary_draw_calls", "primary_submitted_voxels", "secondary_submitted_voxels",
    "frame_valid", "invalid_reason",
]

RUNS_HEADER = [
    "schema", "run_id", "suite", "session_id", "config_id", "pair_id", "block_id",
    "requested_mode", "actual_mode", "repetition", "valid", "reason",
    "actual_total_count", "actual_static_count", "actual_dynamic_count", "resolved_config_hash",
    "measured_frame_count", "valid_frame_count", "invalid_frame_count",
    "mean_present_to_present_ms", "mean_cpu_submission_ms", "mean_cpu_total_frame_ms",
    "critical_path_gpu_ms",
]


class AnalysisTests(unittest.TestCase):
    def fixture(self) -> tuple[tempfile.TemporaryDirectory[str], Path]:
        temp = tempfile.TemporaryDirectory()
        root = Path(temp.name)
        HostileFixture(root).write()
        return temp, root

    def assert_fails(self, root: Path, pattern: str) -> None:
        with self.assertRaisesRegex(ValidationError, pattern):
            validate(root)

    def update_manifest_config(self, root: Path, config_id: str, **updates: Any) -> None:
        manifest = read_json(root / "manifest.json")
        for config in manifest["configs"]:
            if config["config_id"] == config_id:
                config.update(updates)
        (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    def update_execution_manifest(self, root: Path, config_id: str, **updates: Any) -> None:
        for path in root.glob(f"VoxelBenchmark_*_{config_id}_manifest.json"):
            data = read_json(path)
            data.update(updates)
            path.write_text(json.dumps(data), encoding="utf-8")

    def update_csv_rows(self, root: Path, name: str, predicate: Any, **updates: Any) -> None:
        rows = read_csv(root / name)
        for row in rows:
            if predicate(row):
                row.update({key: str(value) for key, value in updates.items()})
        write_dict_csv(root / name, rows)

    def update_validation_case(self, root: Path, case_id: str, **updates: Any) -> None:
        data = read_json(root / "voxel_visual_validation.json")
        for case in data["cases"]:
            if case["case_id"] == case_id:
                case.update(updates)
        (root / "voxel_visual_validation.json").write_text(json.dumps(data), encoding="utf-8")

    def test_valid_fixture_passes_and_writes_v2_summary(self) -> None:
        temp, root = self.fixture()
        with temp:
            result = validate(root)
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["schema"], "mgpu_voxel_analysis_summary.v2")
            self.assertEqual(result["statistics"]["speedup_ci95"]["status"], "NOT_MEASURED")
            self.assertEqual(result["primary_endpoint"], "present_to_present_ms")
            self.assertEqual(result["statistics"]["pair_count"], 1)
            runs = {run["config_id"]: run for run in result["recomputed_runs"]}
            self.assertEqual(runs["c_single"]["resolved_config_hash"], "single-full-hash")
            self.assertEqual(runs["c_multi"]["resolved_config_hash"], "multi-full-hash")
            path = write_summary(root, result)
            self.assertTrue(path.exists())
            self.assertTrue((root / "analysis_summary.v2.sha256").exists())

    def test_real_producer_raw_csv_columns_are_accepted(self) -> None:
        temp, root = self.fixture()
        with temp:
            rows = read_csv(root / "raw_frames.csv")
            self.assertIn("visual_validation_case_id", rows[0])
            self.assertIn("visual_validation_protocol_hash", rows[0])
            self.assertIn("visual_validation_config_hash", rows[0])
            self.assertIn("visual_validation_camera_hash", rows[0])
            self.assertIn("validation_case_id", rows[0])
            result = validate(root)
            self.assertEqual(result["status"], "PASS")

    def test_pairing_fails_for_different_pair_id(self) -> None:
        temp, root = self.fixture()
        with temp:
            self.update_manifest_config(root, "c_multi", pair_id="pair1")
            self.update_execution_manifest(root, "c_multi", pair_id="pair1")
            self.update_csv_rows(root, "raw_frames.csv", lambda row: row["config_id"] == "c_multi", pair_id="pair1")
            self.update_csv_rows(root, "runs.csv", lambda row: row["config_id"] == "c_multi", pair_id="pair1")
            self.assert_fails(root, "missing valid matching Single/Multi pair")

    def test_validate_single_multi_pair_allows_mode_specific_hashes(self) -> None:
        single = self.sample_pair_run("SingleGpuFull", "c_single")
        multi = self.sample_pair_run("MultiGpuFull", "c_multi")
        self.assertNotEqual(single["resolved_config_hash"], multi["resolved_config_hash"])
        self.assertNotEqual(single["validation_config_hash"], multi["validation_config_hash"])
        validate_single_multi_pair(single, multi)

    def test_pairing_fails_for_secondary_share_mismatch(self) -> None:
        temp, root = self.fixture()
        with temp:
            self.update_manifest_config(root, "c_multi", secondary_share=0.75)
            self.assert_fails(root, "missing valid matching Single/Multi pair")

    def test_threelevel_without_boolean_is_lod_on(self) -> None:
        manifest = {"randomization_seed": 123, "warmup_frames": 1, "measured_frames": 2}
        config = HostileFixture(Path(".")).config("c_lod", "SingleGpuFull", 0)
        config["spatial_lod"] = "ThreeLevel"
        config.pop("spatial_lod_enabled", None)
        normalized = normalize_config(config, manifest)
        self.assertTrue(normalized["spatial_lod_enabled"])

    def test_equivalence_case_tokens_do_not_count_as_fidelity(self) -> None:
        temporal = self.hypothesis_run(
            "MultiGpuTemporalDecimation", "temporal_equivalence_token", 0,
            validation_case_id="foo|equivalence|temporal|lod_on",
            matched_validation_case_id="foo|equivalence|temporal|lod_on",
            matched_validation_kind="implementation_equivalence",
            approximation_fidelity_pass=False,
        )
        self.assertFalse(is_approximation_fidelity_pass(temporal))

    def test_full_does_not_pair_with_temporal(self) -> None:
        single = self.sample_pair_run("SingleGpuFull", "c_single")
        multi = self.sample_pair_run("MultiGpuTemporalDecimation", "c_multi")
        with self.assertRaisesRegex(ValidationError, "cannot pair"):
            validate_single_multi_pair(single, multi)

    def test_duplicate_single_pair_key_fails(self) -> None:
        single = self.sample_pair_run("SingleGpuFull", "c_single")
        duplicate = dict(single)
        duplicate["config_id"] = "c_single_duplicate"
        multi = self.sample_pair_run("MultiGpuFull", "c_multi")
        with self.assertRaisesRegex(ValidationError, "duplicate Single run"):
            recompute_pairs({("c_single", 0): single, ("c_single_duplicate", 0): duplicate, ("c_multi", 0): multi})

    def test_pairing_fails_for_mode_independent_invariant_mismatch(self) -> None:
        for field, value in (
            ("actual_static_count", 100001),
            ("total_logical_updated_voxels", 11),
            ("validation_camera_hash", "other-camera"),
            ("total_executed_fixed_steps", 4),
        ):
            single = self.sample_pair_run("SingleGpuFull", "c_single")
            multi = self.sample_pair_run("MultiGpuFull", "c_multi")
            multi[field] = value
            with self.subTest(field=field):
                with self.assertRaisesRegex(ValidationError, "pair invariant mismatch"):
                    validate_single_multi_pair(single, multi)

    def test_empty_resolved_config_hash_fails(self) -> None:
        temp, root = self.fixture()
        with temp:
            self.update_csv_rows(root, "raw_frames.csv", lambda row: row["config_id"] == "c_multi",
                                 resolved_config_hash="")
            self.assert_fails(root, "resolved_config_hash missing|invalid raw frame")

    def test_wrong_candidate_config_hash_fails(self) -> None:
        temp, root = self.fixture()
        with temp:
            self.update_validation_case(root, "full_lod_off|equivalence|steady_240",
                                        candidate_config_hash="wrong-candidate")
            self.assert_fails(root, "implementation equivalence resolved_config_hash mismatch|invalid raw frame")

    def test_wrong_per_case_camera_hash_fails(self) -> None:
        temp, root = self.fixture()
        with temp:
            self.update_csv_rows(root, "raw_frames.csv", lambda row: row["config_id"] == "c_multi",
                                 visual_validation_camera_hash="wrong-camera")
            self.assert_fails(root, "validation camera hash mismatch|invalid raw frame")

    def sample_pair_run(self, mode: str, config_id: str) -> dict[str, Any]:
        return {
            "run_id": "run0",
            "session_id": "session0",
            "config_id": config_id,
            "pair_id": "pair0",
            "block_id": "block0",
            "repetition": 0,
            "randomization_seed": 123,
            "requested_mode": mode,
            "actual_mode": mode,
            "mode_family": mode_family(mode),
            "preset": "Low",
            "requested_static_budget_label": 100000,
            "requested_static_budget": 100000,
            "requested_dynamic_budget": 25000,
            "secondary_share": 0.5,
            "spatial_lod_enabled": False,
            "spatial_lod": "Off",
            "temporal_interval": 1,
            "temporal_policy": "",
            "partition_strategy": "",
            "partition_chunk_size": "",
            "render_width": 1920,
            "render_height": 1080,
            "actual_total_count": 125000,
            "actual_static_count": 100000,
            "actual_dynamic_count": 25000,
            "total_logical_updated_voxels": 20,
            "total_executed_fixed_steps": 2,
            "validation_protocol_hash": "protocol",
            "validation_camera_hash": "camera",
            "validation_config_hash": "single-case" if mode in SINGLE_MODES else "multi-case",
            "resolved_config_hash": "single-full-hash" if mode in SINGLE_MODES else "multi-full-hash",
            "implementation_equivalence_pass": True,
            "approximation_fidelity_pass": False,
            "matched_validation_case_id": "equivalence_case",
            "matched_validation_kind": "implementation_equivalence",
            "matched_validation_camera_hash": "camera",
            "mean_present_to_present_ms": 10.0 if mode in SINGLE_MODES else 5.0,
            "mean_cpu_submission_ms": 1.5,
            "mean_cpu_total_frame_ms": 2.0,
            "critical_path_gpu_ms": 8.0 if mode in SINGLE_MODES else 4.0,
            "gpu_work_sum_ms": 9.0 if mode in SINGLE_MODES else 5.0,
            "secondary_compute_ms": 1.0 if mode in MULTI_MODES else 0.0,
            "transfer_ms": 1.0,
            "composite_ms": 1.0,
            "primary_submitted_voxels": 100000 if mode in SINGLE_MODES else 75000,
            "secondary_submitted_voxels": 0 if mode in SINGLE_MODES else 25000,
        }

    def hypothesis_run(self, mode: str, config_id: str, repetition: int, **updates: Any) -> dict[str, Any]:
        run = self.sample_pair_run(mode, config_id)
        run.update({
            "repetition": repetition,
            "validation_case_id": "temporal_lod_off" if "Temporal" in mode else "full_lod_off",
            "matched_validation_case_id": "temporal_lod_off" if "Temporal" in mode else "full_lod_off",
            "temporal_interval": 4 if "Temporal" in mode else 1,
            "mean_present_to_present_ms": 10.0,
            "critical_path_gpu_ms": 8.0,
            "gpu_work_sum_ms": 8.0,
            "secondary_compute_ms": 2.0 if mode in MULTI_MODES else 0.0,
        })
        run.update(updates)
        if "approximation_fidelity_pass" not in updates:
            run["approximation_fidelity_pass"] = bool(
                "Temporal" in run["requested_mode"] or run.get("spatial_lod_enabled"))
        if run["approximation_fidelity_pass"]:
            run["matched_validation_kind"] = "approximation_fidelity"
        if "matched_validation_case_id" not in updates:
            run["matched_validation_case_id"] = run["validation_case_id"]
        run["matched_validation_camera_hash"] = run["validation_camera_hash"]
        run["mode_family"] = mode_family(run["requested_mode"])
        return run

    def run_map(self, runs: list[dict[str, Any]]) -> dict[tuple[str, int], dict[str, Any]]:
        return {(run["config_id"], int(run["repetition"])): run for run in runs}

    def integration_hypothesis_runs(self) -> dict[tuple[str, int], dict[str, Any]]:
        runs: list[dict[str, Any]] = []
        for rep in range(3):
            for mode in sorted(MODES):
                for lod_enabled in (False, True):
                    lod_name = "ThreeLevel" if lod_enabled else "Off"
                    family = "temporal" if "Temporal" in mode else "full"
                    lod_suffix = "lod_on" if lod_enabled else "lod_off"
                    submitted_base = 120.0 if not lod_enabled else 70.0
                    single = mode in SINGLE_MODES
                    temporal = "Temporal" in mode
                    run = self.hypothesis_run(
                        mode,
                        f"{mode}_{lod_suffix}_rep{rep}",
                        rep,
                        spatial_lod_enabled=lod_enabled,
                        spatial_lod=lod_name,
                        temporal_interval=4 if temporal else 1,
                        validation_case_id=f"{family}_{lod_suffix}",
                        resolved_config_hash=f"{mode}_{lod_suffix}_resolved_hash_rep{rep}",
                        mean_present_to_present_ms=(
                            10.0 + rep if mode == "SingleGpuFull" else
                            8.0 + rep if mode == "MultiGpuFull" else
                            100.0 + rep if mode == "SingleGpuTemporalDecimation" else
                            1.0 + rep
                        ),
                        secondary_compute_ms=(
                            4.0 + rep if mode == "MultiGpuFull" else
                            1.0 + rep if mode == "MultiGpuTemporalDecimation" else
                            0.0
                        ),
                        gpu_work_sum_ms=(
                            1.0 if mode == "MultiGpuFull" else
                            10.0 if mode == "MultiGpuTemporalDecimation" else
                            5.0
                        ),
                        primary_submitted_voxels=submitted_base if single else submitted_base * 0.75,
                        secondary_submitted_voxels=0.0 if single else submitted_base * 0.25,
                    )
                    runs.append(run)
        return self.run_map(runs)

    def test_integration_synthetic_fixture_separates_h1_h2_h3_factors(self) -> None:
        runs = self.integration_hypothesis_runs()
        self.assertEqual(len(runs), 24)
        hashes = [run["resolved_config_hash"] for run in runs.values()]
        self.assertEqual(len(set(hashes)), len(hashes))

        h1 = evaluate_h1(runs)
        h2 = evaluate_h2(runs)
        h3 = evaluate_h3(runs)

        self.assertEqual(h1["paired_n"], 6)
        self.assertEqual(h1["result"], "SUPPORT")
        self.assertEqual(h2["paired_n"], 6)
        self.assertEqual(h2["endpoint"], "secondary_compute_ms")
        self.assertEqual(h2["effect_direction"], "TEMPORAL_LOWER")
        self.assertEqual(h2["result"], "SUPPORT")
        self.assertEqual(h3["paired_n"], 12)
        self.assertEqual(h3["endpoint"], "primary_submitted_voxels + secondary_submitted_voxels")
        self.assertEqual(h3["result"], "SUPPORT")

    def test_h1_ignores_temporal_pairs_with_large_speedup(self) -> None:
        runs = []
        for rep, diff in enumerate((1.0, -1.0)):
            runs.append(self.hypothesis_run("SingleGpuFull", f"single_full_{rep}", rep,
                                            mean_present_to_present_ms=10.0))
            runs.append(self.hypothesis_run("MultiGpuFull", f"multi_full_{rep}", rep,
                                            mean_present_to_present_ms=10.0 - diff))
            runs.append(self.hypothesis_run("SingleGpuTemporalDecimation", f"single_temporal_{rep}", rep,
                                            mean_present_to_present_ms=100.0))
            runs.append(self.hypothesis_run("MultiGpuTemporalDecimation", f"multi_temporal_{rep}", rep,
                                            mean_present_to_present_ms=1.0))
        h1 = evaluate_h1(self.run_map(runs))
        self.assertEqual(h1["paired_n"], 2)
        self.assertNotEqual(h1["result"], "SUPPORT")

    def test_h1_full_ci_crosses_zero_not_supported(self) -> None:
        runs = []
        for rep, diff in enumerate((2.0, -2.0)):
            runs.append(self.hypothesis_run("SingleGpuFull", f"single_full_ci_{rep}", rep,
                                            mean_present_to_present_ms=10.0))
            runs.append(self.hypothesis_run("MultiGpuFull", f"multi_full_ci_{rep}", rep,
                                            mean_present_to_present_ms=10.0 - diff))
        h1 = evaluate_h1(self.run_map(runs))
        self.assertEqual(h1["effect_direction"], "INCONCLUSIVE")
        self.assertEqual(h1["result"], "NOT_SUPPORTED")

    def test_h2_uses_secondary_compute_not_gpu_work_sum(self) -> None:
        runs = []
        for rep in range(2):
            runs.append(self.hypothesis_run("MultiGpuFull", f"multi_full_gpuwork_{rep}", rep,
                                            secondary_compute_ms=2.0, gpu_work_sum_ms=10.0))
            runs.append(self.hypothesis_run("MultiGpuTemporalDecimation", f"multi_temporal_gpuwork_{rep}", rep,
                                            secondary_compute_ms=2.0, gpu_work_sum_ms=1.0))
        h2 = evaluate_h2(self.run_map(runs))
        self.assertEqual(h2["endpoint"], "secondary_compute_ms")
        self.assertNotEqual(h2["result"], "SUPPORT")

    def test_h2_support_when_secondary_compute_ci_excludes_zero_and_fidelity_passes(self) -> None:
        runs = []
        for rep in range(2):
            runs.append(self.hypothesis_run("MultiGpuFull", f"multi_full_h2_{rep}", rep,
                                            secondary_compute_ms=4.0))
            runs.append(self.hypothesis_run("MultiGpuTemporalDecimation", f"multi_temporal_h2_{rep}", rep,
                                            secondary_compute_ms=2.0))
        h2 = evaluate_h2(self.run_map(runs))
        self.assertEqual(h2["effect_direction"], "TEMPORAL_LOWER")
        self.assertEqual(h2["result"], "SUPPORT")

    def test_h2_ci_crosses_zero_not_supported(self) -> None:
        runs = []
        for rep, temporal_value in enumerate((1.0, 5.0)):
            runs.append(self.hypothesis_run("MultiGpuFull", f"multi_full_h2_ci_{rep}", rep,
                                            secondary_compute_ms=3.0))
            runs.append(self.hypothesis_run("MultiGpuTemporalDecimation", f"multi_temporal_h2_ci_{rep}", rep,
                                            secondary_compute_ms=temporal_value))
        h2 = evaluate_h2(self.run_map(runs))
        self.assertEqual(h2["effect_direction"], "INCONCLUSIVE")
        self.assertEqual(h2["result"], "NOT_SUPPORTED")

    def test_h2_requires_fidelity_and_logical_work_equality(self) -> None:
        runs = [
            self.hypothesis_run("MultiGpuFull", "multi_full_h2_bad_0", 0, secondary_compute_ms=4.0),
            self.hypothesis_run("MultiGpuTemporalDecimation", "multi_temporal_h2_bad_0", 0,
                                secondary_compute_ms=2.0, approximation_fidelity_pass=False),
            self.hypothesis_run("MultiGpuFull", "multi_full_h2_bad_1", 1, secondary_compute_ms=4.0),
            self.hypothesis_run("MultiGpuTemporalDecimation", "multi_temporal_h2_bad_1", 1,
                                secondary_compute_ms=2.0, total_logical_updated_voxels=21),
        ]
        h2 = evaluate_h2(self.run_map(runs))
        self.assertFalse(h2["approximation_fidelity_pass"])
        self.assertFalse(h2["logical_work_equal"])
        self.assertEqual(h2["result"], "NOT_SUPPORTED")

    def test_h2_missing_mate_fails_closed(self) -> None:
        runs = [
            self.hypothesis_run("MultiGpuFull", "multi_full_missing_h2_0", 0, secondary_compute_ms=4.0),
            self.hypothesis_run("MultiGpuTemporalDecimation", "multi_temporal_missing_h2_0", 0,
                                secondary_compute_ms=2.0),
            self.hypothesis_run("MultiGpuFull", "multi_full_missing_h2_1", 1, secondary_compute_ms=4.0),
        ]
        h2 = evaluate_h2(self.run_map(runs))
        self.assertFalse(h2["contrast_coverage_complete"])
        self.assertNotEqual(h2["result"], "SUPPORT")

    def test_h3_ignores_single_multi_speed_when_lod_increases_submitted_count(self) -> None:
        runs = [
            self.hypothesis_run("MultiGpuFull", "multi_lod_off_bad", 0,
                                spatial_lod_enabled=False, spatial_lod="Off",
                                primary_submitted_voxels=50, secondary_submitted_voxels=50,
                                mean_present_to_present_ms=10.0),
            self.hypothesis_run("MultiGpuFull", "multi_lod_on_bad", 0,
                                spatial_lod_enabled=True, spatial_lod="ThreeLevel",
                                validation_case_id="full_lod_on",
                                primary_submitted_voxels=80, secondary_submitted_voxels=80,
                                mean_present_to_present_ms=1.0),
        ]
        h3 = evaluate_h3(self.run_map(runs))
        self.assertFalse(h3["monotonic_non_increase"])
        self.assertEqual(h3["result"], "NOT_SUPPORTED")

    def test_h3_supports_matched_lod_non_increase_with_fidelity(self) -> None:
        runs = []
        for mode in sorted(MODES):
            interval = 4 if "Temporal" in mode else 1
            case_prefix = "temporal" if "Temporal" in mode else "full"
            runs.append(self.hypothesis_run(mode, f"{mode}_lod_off", 0,
                                            temporal_interval=interval, spatial_lod_enabled=False,
                                            spatial_lod="Off", validation_case_id=f"{case_prefix}_lod_off",
                                            primary_submitted_voxels=80, secondary_submitted_voxels=20))
            runs.append(self.hypothesis_run(mode, f"{mode}_lod_on", 0,
                                            temporal_interval=interval, spatial_lod_enabled=True,
                                            spatial_lod="ThreeLevel", validation_case_id=f"{case_prefix}_lod_on",
                                            primary_submitted_voxels=60, secondary_submitted_voxels=10))
        h3 = evaluate_h3(self.run_map(runs))
        self.assertEqual(h3["paired_n"], 4)
        self.assertEqual(h3["result"], "SUPPORT")

    def test_h3_missing_mate_fails_closed(self) -> None:
        runs = [
            self.hypothesis_run("MultiGpuFull", "multi_lod_off_missing_0", 0,
                                spatial_lod_enabled=False, spatial_lod="Off",
                                primary_submitted_voxels=80, secondary_submitted_voxels=20),
            self.hypothesis_run("MultiGpuFull", "multi_lod_on_missing_0", 0,
                                spatial_lod_enabled=True, spatial_lod="ThreeLevel",
                                validation_case_id="full_lod_on",
                                primary_submitted_voxels=60, secondary_submitted_voxels=10),
            self.hypothesis_run("SingleGpuFull", "single_lod_off_missing_0", 0,
                                spatial_lod_enabled=False, spatial_lod="Off",
                                primary_submitted_voxels=80, secondary_submitted_voxels=0),
        ]
        h3 = evaluate_h3(self.run_map(runs))
        self.assertFalse(h3["contrast_coverage_complete"])
        self.assertNotEqual(h3["result"], "SUPPORT")

    def test_ci_based_hypotheses_with_n1_are_not_measured(self) -> None:
        runs = [
            self.hypothesis_run("SingleGpuFull", "single_full_n1", 0, mean_present_to_present_ms=10.0),
            self.hypothesis_run("MultiGpuFull", "multi_full_n1", 0, mean_present_to_present_ms=5.0),
            self.hypothesis_run("MultiGpuTemporalDecimation", "multi_temporal_n1", 0, secondary_compute_ms=1.0),
        ]
        h1 = evaluate_h1(self.run_map(runs))
        h2 = evaluate_h2(self.run_map(runs))
        self.assertEqual(h1["result"], "NOT_MEASURED")
        self.assertEqual(h2["result"], "NOT_MEASURED")

    def test_student_t_quantile_df2(self) -> None:
        self.assertAlmostEqual(student_t_ppf(0.975, 2), 4.3026527299, places=6)

    def test_validation_json_empty(self) -> None:
        temp, root = self.fixture()
        with temp:
            (root / "voxel_visual_validation.json").write_text("{}", encoding="utf-8")
            self.assert_fails(root, "validation_json schema mismatch|visual validation aggregate")

    def test_validation_status_fail(self) -> None:
        temp, root = self.fixture()
        with temp:
            data = read_json(root / "voxel_visual_validation.json")
            data["aggregate_status"] = "FAIL"
            (root / "voxel_visual_validation.json").write_text(json.dumps(data), encoding="utf-8")
            self.assert_fails(root, "aggregate is not PASS")

    def test_two_adapter_fallback(self) -> None:
        temp, root = self.fixture()
        with temp:
            data = read_json(root / "two_adapter_preflight.json")
            data["runtime"]["fallback"] = True
            (root / "two_adapter_preflight.json").write_text(json.dumps(data), encoding="utf-8")
            self.assert_fails(root, "fallback")

    def test_wrong_run_id(self) -> None:
        temp, root = self.fixture()
        with temp:
            env = read_json(root / "environment.json")
            env["run_id"] = "wrong"
            (root / "environment.json").write_text(json.dumps(env), encoding="utf-8")
            self.assert_fails(root, "run_id mismatch")

    def test_raw_multi_to_single(self) -> None:
        temp, root = self.fixture()
        with temp:
            rows = read_csv(root / "raw_frames.csv")
            for row in rows:
                if row["config_id"] == "c_multi":
                    row["actual_mode"] = "SingleGpuFull"
            write_dict_csv(root / "raw_frames.csv", rows)
            self.assert_fails(root, "requested_mode != actual_mode|invalid raw frame")

    def test_negative_nan_inf_timing(self) -> None:
        for bad in ("-1", "NaN", "inf"):
            temp, root = self.fixture()
            with temp:
                rows = read_csv(root / "raw_frames.csv")
                rows[0]["present_to_present_ms"] = bad
                write_dict_csv(root / "raw_frames.csv", rows)
                self.assert_fails(root, "present_to_present_ms")

    def test_fake_runs_valid_true_with_invalid_raw(self) -> None:
        temp, root = self.fixture()
        with temp:
            rows = read_csv(root / "raw_frames.csv")
            rows[0]["dropped_steps"] = "1"
            write_dict_csv(root / "raw_frames.csv", rows)
            self.assert_fails(root, "invalid raw frame")

    def test_missing_and_duplicate_config(self) -> None:
        temp, root = self.fixture()
        with temp:
            manifest = read_json(root / "manifest.json")
            manifest["configs"] = manifest["configs"][:1]
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            self.assert_fails(root, "stale/extra execution|missing")
        temp, root = self.fixture()
        with temp:
            manifest = read_json(root / "manifest.json")
            manifest["configs"].append(dict(manifest["configs"][0]))
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            self.assert_fails(root, "duplicate config")

    def test_stale_camera_config_build_hash(self) -> None:
        for field in ("validation.camera_sha256", "validation.case_config_sha256", "build.executable_sha256"):
            temp, root = self.fixture()
            with temp:
                data = read_json(root / "voxel_visual_validation.json")
                data["provenance"]["fields"][field] = "stale"
                (root / "voxel_visual_validation.json").write_text(json.dumps(data), encoding="utf-8")
                self.assert_fails(root, "stale validation")

    def test_wrong_frame_count(self) -> None:
        temp, root = self.fixture()
        with temp:
            rows = read_csv(root / "raw_frames.csv")
            write_dict_csv(root / "raw_frames.csv", rows[:-1])
            self.assert_fails(root, "wrong frame count")

    def test_zero_one_copy_leg(self) -> None:
        temp, root = self.fixture()
        with temp:
            rows = read_csv(root / "raw_frames.csv")
            for row in rows:
                if row["config_id"] == "c_multi":
                    row["primary_shared_to_local_copy_ms"] = "0"
            write_dict_csv(root / "raw_frames.csv", rows)
            self.assert_fails(root, "shared-to-local copy")

    def test_mismatched_summary(self) -> None:
        temp, root = self.fixture()
        with temp:
            rows = read_csv(root / "runs.csv")
            rows[0]["mean_present_to_present_ms"] = "999"
            write_dict_csv(root / "runs.csv", rows)
            self.assert_fails(root, "runs.csv mean_present_to_present_ms mismatch")

    def test_one_valid_run_among_many_invalid_fails(self) -> None:
        temp, root = self.fixture()
        with temp:
            rows = read_csv(root / "raw_frames.csv")
            for row in rows:
                if row["config_id"] == "c_multi":
                    row["particle_transfer_bytes"] = "1"
            write_dict_csv(root / "raw_frames.csv", rows)
            self.assert_fails(root, "invalid raw frame")

    def test_artificial_100x_speedup_failed_evidence(self) -> None:
        temp, root = self.fixture()
        with temp:
            two = read_json(root / "two_adapter_preflight.json")
            two["runtime"]["passed"] = False
            (root / "two_adapter_preflight.json").write_text(json.dumps(two), encoding="utf-8")
            runs = read_csv(root / "runs.csv")
            for row in runs:
                if row["config_id"] == "c_multi":
                    row["mean_present_to_present_ms"] = "0.1"
            write_dict_csv(root / "runs.csv", runs)
            self.assert_fails(root, "runtime.passed")


def write_dict_csv(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


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
        output = write_summary(args.input, result)
    except Exception as exc:
        print(f"analysis_status=FAIL reason={exc}", file=sys.stderr)
        return 2
    print(f"analysis_status=PASS output={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

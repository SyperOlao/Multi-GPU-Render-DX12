#!/usr/bin/env python3
"""Generate publication artifacts only from hostile PASS analysis summaries.

The report deliberately treats producer-generated benchmark status/valid fields as
untrusted. It consumes only analysis_summary.v2.json files that pass checksum and
schema verification, and it never fills missing values with synthetic data.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import html
import json
import math
import sys
from pathlib import Path
from typing import Any


SUMMARY_SCHEMA = "mgpu_voxel_analysis_summary.v2"
REPORT_SCHEMA = "mgpu_voxel_publication_report.v1"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise RuntimeError(f"{path.name} must be a JSON object")
    return data


def load_verified_summary(root: Path) -> dict[str, Any]:
    summary_path = root / "analysis_summary.v2.json"
    checksum_path = root / "analysis_summary.v2.sha256"
    if not summary_path.exists():
        raise RuntimeError("missing analysis_summary.v2.json")
    if not checksum_path.exists():
        raise RuntimeError("missing analysis_summary.v2.sha256")
    expected = checksum_path.read_text(encoding="utf-8").split()[0].lower()
    actual = sha256_file(summary_path)
    if expected != actual:
        raise RuntimeError("analysis_summary.v2.json checksum mismatch")
    summary = load_json(summary_path)
    if summary.get("schema") != SUMMARY_SCHEMA:
        raise RuntimeError("analysis summary schema mismatch")
    if summary.get("status") != "PASS":
        raise RuntimeError("analysis summary is not PASS")
    return summary


def write_csv(path: Path, rows: list[dict[str, Any]], fallback_header: list[str] | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        header = fallback_header or ["status", "reason"]
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=header)
            writer.writeheader()
        return
    fields: list[str] = []
    for row in rows:
        for key in row.keys():
            if key not in fields:
                fields.append(key)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def fmt(value: Any) -> str:
    if value is None or value == "":
        return "NOT_MEASURED"
    if isinstance(value, float):
        if not math.isfinite(value):
            return "NOT_MEASURED"
        return f"{value:.6g}"
    return str(value)


def ci_status(ci: dict[str, Any] | None) -> str:
    if not isinstance(ci, dict):
        return "NOT_MEASURED"
    return str(ci.get("status") or "NOT_MEASURED")


def endpoint_row(label: str, suite: str, result: dict[str, Any], endpoint: str | None = None) -> dict[str, Any]:
    stats = result.get("statistics", {}) if isinstance(result, dict) else {}
    if endpoint is None:
        endpoint = str(result.get("endpoint") or stats.get("endpoint") or "")
    diff = stats.get("difference_ms_ci95") if isinstance(stats.get("difference_ms_ci95"), dict) else {}
    log_ci = stats.get("log_speedup_ci95") if isinstance(stats.get("log_speedup_ci95"), dict) else {}
    speed_ci = stats.get("speedup_ci95") if isinstance(stats.get("speedup_ci95"), dict) else {}
    return {
        "suite": suite,
        "hypothesis": label,
        "endpoint": endpoint or "NOT_MEASURED",
        "paired_n": stats.get("paired_n", "NOT_MEASURED"),
        "mean_difference_ms": stats.get("mean_difference_ms"),
        "median_difference_ms": stats.get("median_difference_ms"),
        "difference_ci_status": ci_status(diff),
        "difference_ci_lower_ms": diff.get("lower"),
        "difference_ci_upper_ms": diff.get("upper"),
        "mean_log_speedup": stats.get("mean_log_speedup"),
        "log_speedup_ci_status": ci_status(log_ci),
        "log_speedup_ci_lower": log_ci.get("lower"),
        "log_speedup_ci_upper": log_ci.get("upper"),
        "speedup_mean": speed_ci.get("mean"),
        "speedup_ci_status": ci_status(speed_ci),
        "speedup_ci_lower": speed_ci.get("lower"),
        "speedup_ci_upper": speed_ci.get("upper"),
        "effect_direction": stats.get("effect_direction", "NOT_MEASURED"),
        "coefficient_of_variation": stats.get("coefficient_of_variation"),
        "result": result.get("result", "NOT_MEASURED"),
    }


def load_environment(root: Path, suite: str, run_id: str) -> dict[str, Any]:
    publication_controls = {}
    controls_path = root / "environment_controls.publication.json"
    if controls_path.exists():
        try:
            publication_controls = load_json(controls_path)
        except Exception:
            publication_controls = {}
    env_path = root / "environment.json"
    if not env_path.exists():
        if publication_controls:
            controls = publication_controls
            return {
                "suite": suite,
                "run_id": run_id,
                "schema": controls.get("schema", "mgpu_voxel_environment_controls.v1"),
                "power_plan": controls.get("power_plan", {}).get("value", "UNKNOWN") if isinstance(controls.get("power_plan"), dict) else controls.get("power_plan", "UNKNOWN"),
                "process_priority": controls.get("process_priority", "UNKNOWN"),
                "process_affinity": controls.get("process_affinity", "UNKNOWN"),
                "vsync_tearing": controls.get("vsync_tearing", {}).get("status", "UNKNOWN") if isinstance(controls.get("vsync_tearing"), dict) else controls.get("vsync_tearing", "UNKNOWN"),
                "gpu_debug_validation": controls.get("debug_gpu_validation", {}).get("status", "UNKNOWN") if isinstance(controls.get("debug_gpu_validation"), dict) else controls.get("debug_gpu_validation", "UNKNOWN"),
                "hags": controls.get("hags", {}).get("value", "UNKNOWN") if isinstance(controls.get("hags"), dict) else controls.get("hags", "UNKNOWN"),
                "display_topology": "RECORDED" if controls.get("display_topology") else "UNKNOWN",
                "background_load_policy": controls.get("background_load_policy", {}).get("status", "UNKNOWN") if isinstance(controls.get("background_load_policy"), dict) else controls.get("background_load_policy", "UNKNOWN"),
                "thermal_clock_power": controls.get("thermal_clock_power", {}).get("status", "UNKNOWN") if isinstance(controls.get("thermal_clock_power"), dict) else controls.get("thermal_clock_power", "UNKNOWN"),
                "status": controls.get("session_control_status", "RECORDED"),
                "reason": "; ".join(controls.get("critical_uncontrolled_confounders", [])) if isinstance(controls.get("critical_uncontrolled_confounders"), list) else "",
            }
        return {
            "suite": suite,
            "run_id": run_id,
            "status": "UNKNOWN",
            "reason": "environment.json missing",
        }
    try:
        env = load_json(env_path)
    except Exception as exc:
        return {"suite": suite, "run_id": run_id, "status": "UNKNOWN", "reason": str(exc)}
    controls = env.get("environment_controls", {}) if isinstance(env.get("environment_controls"), dict) else {}
    if publication_controls:
        controls = {**controls, **publication_controls}
    return {
        "suite": suite,
        "run_id": run_id,
        "schema": env.get("schema", "UNKNOWN"),
        "power_plan": controls.get("power_plan", {}).get("value", "UNKNOWN") if isinstance(controls.get("power_plan"), dict) else controls.get("power_plan", env.get("power_plan", "UNKNOWN")),
        "process_priority": controls.get("process_priority", env.get("process_priority", "UNKNOWN")),
        "process_affinity": controls.get("process_affinity", env.get("process_affinity", "UNKNOWN")),
        "vsync_tearing": controls.get("vsync_tearing", {}).get("status", "UNKNOWN") if isinstance(controls.get("vsync_tearing"), dict) else controls.get("vsync_tearing", env.get("vsync_tearing", "UNKNOWN")),
        "gpu_debug_validation": controls.get("debug_gpu_validation", {}).get("status", "UNKNOWN") if isinstance(controls.get("debug_gpu_validation"), dict) else controls.get("gpu_debug_validation", env.get("gpu_debug_validation", "UNKNOWN")),
        "hags": controls.get("hags", {}).get("value", "UNKNOWN") if isinstance(controls.get("hags"), dict) else controls.get("hags", env.get("hags", "UNKNOWN")),
        "display_topology": "RECORDED" if controls.get("display_topology") else env.get("display_topology", "UNKNOWN"),
        "background_load_policy": controls.get("background_load_policy", {}).get("status", "UNKNOWN") if isinstance(controls.get("background_load_policy"), dict) else controls.get("background_load_policy", env.get("background_load_policy", "UNKNOWN")),
        "thermal_clock_power": controls.get("thermal_clock_power", {}).get("status", "UNKNOWN") if isinstance(controls.get("thermal_clock_power"), dict) else controls.get("thermal_clock_power", env.get("thermal_clock_power", "UNKNOWN")),
        "status": controls.get("session_control_status", controls.get("status", env.get("status", "RECORDED"))),
        "reason": controls.get("reason", env.get("reason", "")),
    }


def copy_invalid_records(root: Path, suite: str) -> list[dict[str, Any]]:
    path = root / "invalid_records.csv"
    if not path.exists():
        return [{"suite": suite, "status": "NOT_MEASURED", "reason": "invalid_records.csv missing"}]
    with path.open(newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        return [{"suite": suite, "status": "NONE_REPORTED", "reason": ""}]
    return [{"suite": suite, **row} for row in rows if any(str(v).strip() for v in row.values())]


def raw_dictionary(root: Path, suite: str) -> list[dict[str, str]]:
    path = root / "raw_frames.csv"
    if not path.exists():
        return [{"suite": suite, "field": "raw_frames.csv", "type": "missing", "description": "NOT_MEASURED"}]
    with path.open(newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        fields = reader.fieldnames or []
    descriptions = {
        "present_to_present_ms": "Primary end-to-end endpoint used for rendering speed claims.",
        "cpu_submission_ms": "Secondary CPU submission endpoint, not total CPU frame time.",
        "frame_resource_backpressure_ms": "CPU wait before submission caused by frame-resource availability.",
        "cpu_total_frame_ms": "Backpressure plus CPU submission.",
        "critical_path_gpu_ms": "GPU critical path endpoint recomputed by hostile analyzer.",
        "validation_case_id": "Exact visual validation case associated with this frame.",
        "validation_config_hash": "Validation config/protocol hash asserted by raw frame evidence.",
        "validation_camera_hash": "Validation camera hash asserted by raw frame evidence.",
    }
    return [
        {
            "suite": suite,
            "field": field,
            "type": "producer_field_revalidated_offline",
            "description": descriptions.get(field, "Raw frame field consumed or cross-checked by hostile analysis."),
        }
        for field in fields
    ]


def svg_forest(path: Path, title: str, rows: list[dict[str, Any]], value_key: str,
               lower_key: str, upper_key: str, null_value: float) -> None:
    measured = []
    for row in rows:
        try:
            value = float(row[value_key])
            lower = float(row[lower_key])
            upper = float(row[upper_key])
        except Exception:
            continue
        if math.isfinite(value) and math.isfinite(lower) and math.isfinite(upper):
            measured.append((row, value, lower, upper))
    if not measured:
        return
    lo = min([null_value] + [item[2] for item in measured])
    hi = max([null_value] + [item[3] for item in measured])
    span = hi - lo if hi > lo else 1.0
    width = 760
    row_h = 42
    left = 210
    right = 40
    plot_w = width - left - right
    height = 72 + row_h * len(measured)

    def x(v: float) -> float:
        return left + ((v - lo) / span) * plot_w

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        f'<text x="20" y="28" font-family="Segoe UI,Arial" font-size="18" font-weight="600">{html.escape(title)}</text>',
        f'<line x1="{x(null_value):.1f}" y1="48" x2="{x(null_value):.1f}" y2="{height - 20}" stroke="#555" stroke-dasharray="4 4"/>',
    ]
    for index, (row, value, lower, upper) in enumerate(measured):
        y = 70 + index * row_h
        label = f"{row.get('suite', '')} {row.get('hypothesis', '')}".strip()
        parts.append(f'<text x="20" y="{y + 5}" font-family="Segoe UI,Arial" font-size="13">{html.escape(label)}</text>')
        parts.append(f'<line x1="{x(lower):.1f}" y1="{y}" x2="{x(upper):.1f}" y2="{y}" stroke="#1f5f99" stroke-width="2"/>')
        parts.append(f'<circle cx="{x(value):.1f}" cy="{y}" r="4" fill="#1f5f99"/>')
        parts.append(f'<text x="{width - 36}" y="{y + 5}" text-anchor="end" font-family="Segoe UI,Arial" font-size="12">{html.escape(fmt(value))}</text>')
    parts.append("</svg>")
    path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def html_table(rows: list[dict[str, Any]]) -> str:
    if not rows:
        return "<p>NOT_MEASURED</p>"
    fields: list[str] = []
    for row in rows:
        for key in row.keys():
            if key not in fields:
                fields.append(key)
    out = ["<table><thead><tr>"]
    out.extend(f"<th>{html.escape(field)}</th>" for field in fields)
    out.append("</tr></thead><tbody>")
    for row in rows:
        out.append("<tr>")
        out.extend(f"<td>{html.escape(fmt(row.get(field)))}</td>" for field in fields)
        out.append("</tr>")
    out.append("</tbody></table>")
    return "".join(out)


def build_report(roots: dict[str, Path], output: Path, *, strict: bool) -> int:
    output.mkdir(parents=True, exist_ok=True)
    loaded: list[tuple[str, Path, dict[str, Any]]] = []
    blocked: list[dict[str, str]] = []
    for label, root in roots.items():
        try:
            loaded.append((label, root, load_verified_summary(root)))
        except Exception as exc:
            blocked.append({"suite": label, "status": "BLOCKED", "reason": str(exc)})

    if blocked and strict:
        write_blocked_report(output, blocked)
        return 2
    if not loaded:
        write_blocked_report(output, blocked or [{"suite": "all", "status": "BLOCKED", "reason": "no PASS analysis"}])
        return 2

    environment_rows = []
    h1_rows = []
    h2_rows = []
    h3_rows = []
    rq3_rows = []
    invalid_rows = []
    dictionary_rows = []
    commands_rows = []
    schema_rows = [{"artifact": "publication_report", "schema": REPORT_SCHEMA}]

    for label, root, summary in loaded:
        suite = str(summary.get("suite") or label)
        run_id = str(summary.get("run_id") or "")
        hypotheses = summary.get("hypothesis_results", {})
        if not isinstance(hypotheses, dict):
            hypotheses = {}
        environment_rows.append(load_environment(root, suite, run_id))
        schema_rows.append({"artifact": f"{suite}.analysis_summary", "schema": summary.get("schema", "")})
        commands_rows.append({
            "suite": suite,
            "command": f"python MGPU-VoxelWaterfall/Tools/analyze_benchmark.py --input {root}",
            "status": "PASS",
        })
        h1 = hypotheses.get("H1", {}) if isinstance(hypotheses.get("H1"), dict) else {}
        h2 = hypotheses.get("H2", {}) if isinstance(hypotheses.get("H2"), dict) else {}
        h3 = hypotheses.get("H3", {}) if isinstance(hypotheses.get("H3"), dict) else {}
        rq3 = hypotheses.get("RQ3", {}) if isinstance(hypotheses.get("RQ3"), dict) else {}
        h1_rows.append(endpoint_row("H1", suite, h1))
        h2_row = endpoint_row("H2", suite, h2)
        h2_row["logical_work_equal"] = h2.get("logical_work_equal", "NOT_MEASURED")
        h2_row["approximation_fidelity_pass"] = h2.get("approximation_fidelity_pass", "NOT_MEASURED")
        h2_rows.append(h2_row)
        h3_rows.append({
            "suite": suite,
            "hypothesis": "H3",
            "monotonic_non_increase": h3.get("monotonic_non_increase", "NOT_MEASURED"),
            "simulation_counts_unchanged": h3.get("simulation_counts_unchanged", "NOT_MEASURED"),
            "approximation_fidelity_pass": h3.get("approximation_fidelity_pass", "NOT_MEASURED"),
            "result": h3.get("result", "NOT_MEASURED"),
        })
        transfer_ci = rq3.get("transfer_ms_over_critical_path_ms_ci95", {}) if isinstance(rq3.get("transfer_ms_over_critical_path_ms_ci95"), dict) else {}
        composite_ci = rq3.get("composite_plus_copy_over_critical_path_ms_ci95", {}) if isinstance(rq3.get("composite_plus_copy_over_critical_path_ms_ci95"), dict) else {}
        rq3_rows.append({
            "suite": suite,
            "hypothesis": "RQ3",
            "transfer_ratio_mean": transfer_ci.get("mean"),
            "transfer_ratio_ci_status": ci_status(transfer_ci),
            "transfer_ratio_ci_lower": transfer_ci.get("lower"),
            "transfer_ratio_ci_upper": transfer_ci.get("upper"),
            "composite_plus_copy_share_mean": composite_ci.get("mean"),
            "composite_plus_copy_share_ci_status": ci_status(composite_ci),
            "composite_plus_copy_share_ci_lower": composite_ci.get("lower"),
            "composite_plus_copy_share_ci_upper": composite_ci.get("upper"),
            "criterion": rq3.get("transfer_dominated_criterion", "NOT_MEASURED"),
            "result": rq3.get("result", "NOT_MEASURED"),
        })
        invalid_rows.extend(copy_invalid_records(root, suite))
        dictionary_rows.extend(raw_dictionary(root, suite))

    write_csv(output / "hardware_environment_table.csv", environment_rows)
    write_csv(output / "h1_paired_endpoint_table.csv", h1_rows)
    write_csv(output / "h2_temporal_ablation_table.csv", h2_rows)
    write_csv(output / "h3_lod_count_fidelity_table.csv", h3_rows)
    write_csv(output / "transfer_composite_breakdown_table.csv", rq3_rows)
    write_csv(output / "invalid_exclusion_table.csv", invalid_rows)
    write_csv(output / "raw_data_dictionary.csv", dictionary_rows)
    write_csv(output / "schema_versions.csv", schema_rows)
    write_csv(output / "commands.csv", commands_rows)

    svg_forest(
        output / "h1_paired_endpoint_forest.svg",
        "H1 paired end-to-end difference CI",
        h1_rows,
        "mean_difference_ms",
        "difference_ci_lower_ms",
        "difference_ci_upper_ms",
        0.0,
    )
    svg_forest(
        output / "h1_speedup_ci.svg",
        "H1 exponentiated speedup CI",
        h1_rows,
        "speedup_mean",
        "speedup_ci_lower",
        "speedup_ci_upper",
        1.0,
    )
    svg_forest(
        output / "transfer_dominance_ci.svg",
        "RQ3 transfer ratio CI",
        rq3_rows,
        "transfer_ratio_mean",
        "transfer_ratio_ci_lower",
        "transfer_ratio_ci_upper",
        0.5,
    )

    write_results(output, environment_rows, h1_rows, h2_rows, h3_rows, rq3_rows, invalid_rows, blocked)
    write_status(output, "PASS", loaded, blocked)
    return 0


def write_blocked_report(output: Path, blocked: list[dict[str, str]]) -> None:
    output.mkdir(parents=True, exist_ok=True)
    write_csv(output / "blocked_inputs.csv", blocked)
    write_results(output, [], [], [], [], [], [], blocked)
    write_status(output, "BLOCKED", [], blocked)


def write_results(output: Path, environment_rows: list[dict[str, Any]], h1_rows: list[dict[str, Any]],
                  h2_rows: list[dict[str, Any]], h3_rows: list[dict[str, Any]],
                  rq3_rows: list[dict[str, Any]], invalid_rows: list[dict[str, Any]],
                  blocked: list[dict[str, str]]) -> None:
    sections = [
        "# MGPU-VoxelWaterfall Results",
        "",
        f"Schema: `{REPORT_SCHEMA}`",
        "",
    ]
    if blocked:
        sections.extend(["## Status", "", "BLOCKED inputs:", ""])
        sections.extend(f"- `{row['suite']}`: {row['reason']}" for row in blocked)
        sections.extend(["", "No supporting table or plot was generated from blocked evidence.", ""])
    sections.extend([
        "## Hardware And Environment",
        "",
        markdown_table(environment_rows),
        "",
        "## H1 Paired Endpoint",
        "",
        markdown_table(h1_rows) if h1_rows else "NOT_MEASURED",
        "",
        "## H2 Temporal Ablation",
        "",
        markdown_table(h2_rows) if h2_rows else "NOT_MEASURED",
        "",
        "## H3 LOD Count And Fidelity",
        "",
        markdown_table(h3_rows) if h3_rows else "NOT_MEASURED",
        "",
        "## Transfer And Composite Breakdown",
        "",
        markdown_table(rq3_rows) if rq3_rows else "NOT_MEASURED",
        "",
        "## Invalid And Exclusion Records",
        "",
        markdown_table(invalid_rows) if invalid_rows else "NOT_MEASURED",
        "",
        "## Raw Data Dictionary",
        "",
        "See `raw_data_dictionary.csv`. Missing fields remain `NOT_MEASURED`; no synthetic values are inserted.",
        "",
    ])
    (output / "RESULTS.md").write_text("\n".join(sections), encoding="utf-8")
    html_doc = [
        "<!doctype html><meta charset=\"utf-8\"><title>MGPU-VoxelWaterfall Results</title>",
        "<style>body{font-family:Segoe UI,Arial,sans-serif;margin:32px;line-height:1.45}table{border-collapse:collapse;margin:12px 0}th,td{border:1px solid #ccc;padding:4px 7px;font-size:12px}th{background:#f3f5f7;text-align:left}</style>",
        "<h1>MGPU-VoxelWaterfall Results</h1>",
        f"<p>Schema: <code>{REPORT_SCHEMA}</code></p>",
    ]
    if blocked:
        html_doc.append("<h2>Status</h2><p>BLOCKED. No supporting table or plot was generated from blocked evidence.</p>")
        html_doc.append(html_table(blocked))
    html_doc.extend([
        "<h2>Hardware And Environment</h2>", html_table(environment_rows),
        "<h2>H1 Paired Endpoint</h2>", html_table(h1_rows),
        "<h2>H2 Temporal Ablation</h2>", html_table(h2_rows),
        "<h2>H3 LOD Count And Fidelity</h2>", html_table(h3_rows),
        "<h2>Transfer And Composite Breakdown</h2>", html_table(rq3_rows),
        "<h2>Invalid And Exclusion Records</h2>", html_table(invalid_rows),
        "<h2>Raw Data Dictionary</h2><p>See <code>raw_data_dictionary.csv</code>.</p>",
    ])
    (output / "RESULTS.html").write_text("\n".join(html_doc), encoding="utf-8")


def markdown_table(rows: list[dict[str, Any]]) -> str:
    if not rows:
        return "NOT_MEASURED"
    fields: list[str] = []
    for row in rows:
        for key in row.keys():
            if key not in fields:
                fields.append(key)
    lines = [
        "| " + " | ".join(fields) + " |",
        "| " + " | ".join("---" for _ in fields) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(fmt(row.get(field)).replace("|", "\\|") for field in fields) + " |")
    return "\n".join(lines)


def write_status(output: Path, status: str, loaded: list[tuple[str, Path, dict[str, Any]]],
                 blocked: list[dict[str, str]]) -> None:
    payload = {
        "schema": REPORT_SCHEMA,
        "status": status,
        "inputs": [
            {
                "label": label,
                "path": str(root),
                "suite": summary.get("suite"),
                "run_id": summary.get("run_id"),
                "analysis_sha256": sha256_file(root / "analysis_summary.v2.json"),
            }
            for label, root, summary in loaded
        ],
        "blocked_inputs": blocked,
        "outputs": sorted(path.name for path in output.iterdir() if path.is_file()),
    }
    (output / "publication_report_status.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", type=Path, action="append", required=True)
    parser.add_argument("--full", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()
    try:
        roots: dict[str, Path] = {}
        for index, path in enumerate(args.smoke):
            roots[f"Smoke/session_{index:03d}"] = path
        for index, path in enumerate(args.full):
            roots[f"Full/session_{index:03d}"] = path
        return build_report(roots, args.output, strict=args.strict)
    except Exception as exc:
        print(f"publication_report_status=FAIL reason={exc}", file=sys.stderr)
        try:
            write_blocked_report(args.output, [{"suite": "report", "status": "BLOCKED", "reason": str(exc)}])
        except Exception:
            pass
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

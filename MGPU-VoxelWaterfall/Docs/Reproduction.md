# Reproduction

## Preconditions

Comparative benchmark suites require:

- exact-matrix visual validation `PASS`;
- explicit two-hardware-adapter runtime verification `PASS`;
- compatible `mgpu_research_provenance.v1` fields for build, shader bytecode, adapter LUIDs/drivers, validation protocol/config/camera, render formats/resolution/sample count, workload seeds/counts, temporal/LOD/partition/chunk settings, and runtime/toolchain identity;
- a fresh empty output directory for each benchmark run.

If the machine lacks two compatible hardware D3D12 adapters with distinct LUID values, comparative benchmark status is `BLOCKED`.

## Publication Pipeline

Preferred end-to-end command:

```powershell
.\MGPU-VoxelWaterfall\Research\run_publication_suite.ps1 -Sessions 1 -SmokeRepetitions 1 -FullRepetitions 3
```

The pipeline runs:

```text
build/provenance check -> two-adapter verification -> exact-matrix visual validation -> Smoke -> hostile analysis -> Full -> hostile analysis -> report
```

Any nonzero, `BLOCKED`, or `INVALID` step stops later measurements and writes `publication_status.json`, command logs, exit codes, and checksums.

## Manual Commands

Run visual validation into a run-specific directory:

```text
MGPU-VoxelWaterfall.exe --run-validation-once --validation-output-dir=<validation-dir>
```

Run explicit two-adapter verification into a run-specific directory:

```text
MGPU-VoxelWaterfall.exe --verify-two-adapter --two-adapter-output-dir=<preflight-dir>
```

Run Smoke:

```text
MGPU-VoxelWaterfall.exe --benchmark-smoke --benchmark-output-dir=<fresh-empty-artifact-dir> --benchmark-seed=<uint> --benchmark-repetitions=<predeclared-n>
```

Run Full:

```text
MGPU-VoxelWaterfall.exe --benchmark-full --benchmark-output-dir=<fresh-empty-artifact-dir> --benchmark-seed=<uint> --benchmark-repetitions=<predeclared-n>
```

Omit `--benchmark-repetitions` only for the preliminary profile. For publication runs, choose `n` before measurement:

```text
python MGPU-VoxelWaterfall\Tools\plan_benchmark_power.py --pilot-sd <sd> --mde <difference-ms>
python MGPU-VoxelWaterfall\Tools\plan_benchmark_power.py --pilot-sd <sd> --ci-half-width <ms>
```

Run hostile analysis:

```text
python MGPU-VoxelWaterfall\Tools\analyze_benchmark.py --input <artifact-dir>
```

Generate article tables only from verified hostile analysis:

```text
python MGPU-VoxelWaterfall\Tools\plot_benchmark.py --input <artifact-dir> --output <tables-dir>
```

Generate publication report only from PASS analysis summaries:

```text
python MGPU-VoxelWaterfall\Tools\generate_publication_report.py --smoke <smoke-dir> --full <full-dir> --output <report-dir> --strict
```

## Exit Codes

- `0`: suite reached producer `COMPLETE` and hostile/verification command passed.
- `2`: `INVALID`, `FAIL`, runtime error, startup exception for research commands, or hostile-analysis rejection.
- `3`: `BLOCKED` precondition, including missing/stale validation evidence or missing/incompatible two-adapter hardware.
- `4`: cancellation/interruption when a benchmark run is interrupted.

## Expected And Optional Artifacts

Expected for a COMPLETE benchmark suite:

- `manifest.json`;
- `environment.json`;
- `two_adapter_preflight.json`;
- `voxel_visual_validation.json`;
- `voxel_visual_validation.csv`;
- `raw_frames.csv`;
- `runs.csv`;
- `paired_runs.csv`;
- `paired_summary.csv`;
- `invalid_records.csv`;
- `telemetry.csv`;
- `VoxelBenchmark_<Suite>_<config_id>_manifest.json` for each execution;
- `VoxelBenchmark_<Suite>_Status.json`;
- one raw per-execution CSV matching `VoxelBenchmark_*.csv`;
- `README.txt`.

Expected after hostile analysis:

- `analysis_summary.v2.json`;
- `analysis_summary.v2.sha256`.

Publication pipeline artifacts:

- `build_manifest.json`;
- `environment_controls.<phase>.json`;
- `commands.jsonl`;
- `checksums.sha256`;
- `report/RESULTS.md`;
- `report/RESULTS.html`;
- report CSV tables and SVG plots only when the underlying analysis is PASS and measured;
- `publication_status.json`;
- `publication_artifacts.zip` only after manifest completeness verification.

Optional artifacts:

- `memory_timeline.csv` is not produced by automatic benchmark suites. It is produced by memory soak/rebuild commands only.
- temperature/clock/power telemetry is optional; if unavailable it must be `UNKNOWN` with a reason.

For a `BLOCKED` suite, `manifest.json` and `invalid_records.csv` are required. Other evidence files may be absent if the gate blocked before they were generated. A blocked artifact must not be treated as a numeric result source.

## Artifact Semantics

`two_adapter_preflight.json` is both a named artifact and required evidence for comparative runs. It must be listed consistently by the manifest/artifact reader and must not be replaced by a copied stale file.

`compiled_shader_bytecode_sha256` and `compiled_shader_bytecode_set_sha256` refer to actual `.cso` bytecode blobs used by runtime/build outputs. Source HLSL hashes are not shader bytecode evidence.

`COMPLETE` is a producer state. `PASS` is validation/preflight/hostile-analysis evidence. A `COMPLETE` producer suite can still be rejected by hostile analysis if any required artifact or semantic check fails.

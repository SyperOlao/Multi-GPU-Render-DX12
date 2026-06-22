# MGPU-VoxelWaterfall Schema Data Dictionary

This directory documents the artifact contract used by the benchmark and hostile analyzer. Example JSON files in `examples/` are fixture-only examples and do not contain performance results.

## Schema Names

- `mgpu_voxel_benchmark_manifest.v2`: benchmark suite manifest.
- `mgpu_voxel_environment.v2`: environment/build/runtime metadata.
- `mgpu_voxel_visual_validation.v2`: visual validation JSON/CSV rows.
- `mgpu_voxel_two_adapter_preflight.v2`: two-adapter runtime verification.
- `mgpu_voxel_execution_manifest.v2`: one execution manifest per config.
- `mgpu_voxel_raw_frame.v2`: raw frame CSV rows.
- `mgpu_voxel_runs.v2`: run/repetition CSV rows recomputed by hostile analysis.
- `mgpu_voxel_paired_runs.v2`: paired run CSV rows.
- `mgpu_voxel_invalid_records.v2`: invalid/exclusion CSV rows.
- `mgpu_voxel_run_telemetry.v2`: per-run telemetry CSV rows.
- `mgpu_voxel_analysis_summary.v2`: hostile analysis output.
- `mgpu_research_provenance.v1`: canonical provenance record embedded in artifacts.
- `mgpu_voxel_publication_report.v1`: generated publication report status.
- `mgpu_voxel_build_manifest.v1`: clean-build manifest.
- `mgpu_voxel_environment_controls.v1`: publication session environment controls.
- `mgpu_voxel_publication_session.v1`: publication session identity.
- `mgpu_voxel_publication_suite_status.v1`: publication pipeline status.
- `mgpu_voxel_hardware_gpu_ci_status.v1`: CI marker for skipped hardware GPU tests.
- `mgpu_voxel_shader_bytecode_set.v1`: canonical shader bytecode set hash input.
- `mgpu_voxel_visual_validation_protocol.v2`: visual validation protocol identity.
- `mgpu_voxel_visual_validation_generator.v2`: visual validation generator identity.
- `mgpu_two_adapter_preflight_identity.v2`: two-adapter preflight run identity input.
- `mgpu_voxel_paired_summary.v1`: legacy producer paired summary CSV schema.
- `mgpu_voxel_reproduction_readme.v1`: generated artifact README marker.

Legacy accepted input schemas in the hostile analyzer:

- `mgpu_voxel_benchmark_manifest.v1`
- `mgpu_voxel_environment.v1`
- `mgpu_voxel_invalid_records.v1`
- `mgpu_voxel_paired_runs.v1`
- `mgpu_voxel_run_telemetry.v1`
- `mgpu_voxel_runs.v1`

## Status Enums

Producer suite/execution statuses:

- `PENDING`
- `RUNNING`
- `COMPLETE`
- `BLOCKED`
- `INVALID`
- `CANCELLED`
- `INTERRUPTED`

Evidence/analysis statuses:

- `PASS`
- `FAIL`
- `BLOCKED`
- `NOT_MEASURED`
- `INCONCLUSIVE`

`COMPLETE` is not equivalent to `PASS`. A producer `COMPLETE` suite can still fail hostile analysis.

## CLI Flags

Application flags:

- `--run-validation-once`
- `--validation-output-dir=`
- `--verify-two-adapter`
- `--two-adapter-output-dir=`
- `--benchmark-smoke`
- `--benchmark-full`
- `--benchmark-seed=`
- `--benchmark-repetitions=`
- `--benchmark-output-dir=`
- `--profile-sweep`
- `--profile-sweep-seed=`
- `--profile-sweep-warmup-frames=`
- `--profile-sweep-measured-frames=`
- `--profile-sweep-repetitions=`
- `--profile-sweep-output-dir=`
- `--memory-soak`
- `--memory-duration-seconds=`
- `--memory-output-dir=`
- `--memory-rebuild-stress`
- `--memory-rebuild-cycles=`
- `--memory-stable-seconds=`

Tool flags:

- `analyze_benchmark.py --input`
- `analyze_benchmark.py --self-test`
- `plot_benchmark.py --input --output`
- `generate_publication_report.py --smoke --full --output --strict`
- `plan_benchmark_power.py --pilot-sd --mde --ci-half-width --power --max-n`

## Canonical Artifact Filenames

Required for hostile PASS analysis of a COMPLETE benchmark:

- `manifest.json`
- `environment.json`
- `voxel_visual_validation.json`
- `voxel_visual_validation.csv`
- `two_adapter_preflight.json`
- `raw_frames.csv`
- `runs.csv`
- `paired_runs.csv`
- `invalid_records.csv`
- `telemetry.csv`
- `VoxelBenchmark_<Suite>_<config_id>_manifest.json`

Additional producer artifacts:

- `paired_summary.csv`
- `VoxelBenchmark_<Suite>_Status.json`
- `VoxelBenchmark_*.csv`
- `README.txt`

Analysis/report artifacts:

- `analysis_summary.v2.json`
- `analysis_summary.v2.sha256`
- `RESULTS.md`
- `RESULTS.html`
- `checksums.sha256`
- `commands.jsonl`
- `publication_status.json`
- `publication_artifacts.zip`

Publication pipeline files are stored under `MGPU-VoxelWaterfall/Research/`; the default publication output directory is `MGPU-VoxelWaterfall/Research/artifacts/publication/`.

Optional artifacts:

- `memory_timeline.csv`: produced by memory soak/rebuild commands only, not automatic benchmark suites.

## Core Fields

### `manifest.json`

- `schema`: must be `mgpu_voxel_benchmark_manifest.v2`.
- `suite`: `Smoke` or `Full`.
- `run_id`: unique run identity.
- `status`: producer status enum.
- `created_utc`: first artifact creation timestamp, written once.
- `start_utc`: first transition to `RUNNING`, stable across status rewrites.
- `end_utc`: terminal timestamp.
- `primary_endpoint`: `present_to_present_ms`.
- `provenance.fields`: canonical provenance key/value map.
- `configs`: expected execution matrix. Each config is required exactly once.

### Config Fields

- `config_id`: unique config id inside suite.
- `session_id`, `pair_id`, `block_id`, `repetition`: pairing/blocking identifiers.
- `mode`: requested execution mode.
- `preset`: workload preset.
- `requested_static_budget_label`: label such as `100000`; this is not total count.
- `requested_static_budget`: requested static voxel budget.
- `requested_dynamic_budget`: requested dynamic voxel budget.
- `secondary_share`: requested secondary partition share.
- `spatial_lod`: `Off` or `ThreeLevel`.
- `temporal_interval`: temporal decimation interval.
- `order_index`, `block_order_index`, `pair_member_order`: randomized order fields.

### Provenance Fields

The benchmark gate and hostile analyzer compare at least:

- `build.executable_sha256`
- `build.shader_bytecode_set_sha256`
- `adapter.luid_pair`
- `adapter.primary_driver_version`
- `adapter.secondary_driver_version`
- `validation.protocol_sha256`
- `validation.case_config_sha256`
- `validation.camera_sha256`
- `render.resolution`
- `render.color_format`
- `render.depth_format`
- `render.sample_count`
- `workload.static_seed`
- `workload.dynamic_seed`
- requested and actual workload counts
- temporal/LOD/partition/chunk settings
- `runtime.toolchain`

### Raw Frame Fields

Raw frames contain producer observations. Hostile analysis recomputes validity and run summaries from these rows rather than trusting producer `valid` fields.

Important fields:

- `requested_mode`, `actual_mode`, `fallback_reason`
- `requested_fixed_steps`, `executed_fixed_steps`, dropped-step fields
- `actual_static_voxels`, `actual_dynamic_voxels`, `total_voxels`
- `primary_partition_voxels`, `secondary_partition_voxels`
- `logical_updated_voxel_count`
- `render_width`, `render_height`
- `resolved_config_hash`: mode-specific resolved execution configuration hash; required in every raw frame and must match the execution manifest.
- `visual_validation_case_id`, `visual_validation_protocol_hash`, `visual_validation_config_hash`, `visual_validation_camera_hash`
- validation pass fields
- queue calibration fields
- `frame_resource_backpressure_ms`
- `cpu_submission_ms`
- `cpu_total_frame_ms`
- `present_to_present_ms`
- GPU timing, transfer byte, submitted count, and invalid reason fields

### Endpoint Boundaries

- `present_to_present_ms`: interval between successful presents; primary endpoint.
- `frame_resource_backpressure_ms`: wait before CPU submission caused by frame-resource availability.
- `cpu_submission_ms`: after frame-resource acquire through `EndFrameCpu`.
- `cpu_total_frame_ms`: backpressure plus submission.

### Validation Fields

Visual validation v2 records:

- protocol/generator version;
- canonical protocol hash;
- ordered case/checkpoint list;
- tolerances and decision thresholds;
- implementation-equivalence cases;
- approximation-fidelity cases;
- per-case/per-checkpoint metrics.

Benchmark raw rows use the producer column names `visual_validation_case_id`, `visual_validation_protocol_hash`, `visual_validation_config_hash`, and `visual_validation_camera_hash`. The hostile analyzer normalizes these to `validation_case_id`, `validation_protocol_hash`, `validation_config_hash`, and `validation_camera_hash` internally.

The raw validation references must match an exact visual JSON case:

- raw case id equals `cases[].case_id`;
- raw protocol hash equals `cases[].protocol_hash` and the aggregate validation protocol hash;
- raw config hash equals `cases[].reference_config_hash` or `cases[].candidate_config_hash`;
- raw camera hash equals `cases[].camera_hash`.

Aggregate `validation.case_config_sha256` and `validation.camera_sha256` identify the validation case/camera sets. They are not required to equal an individual resolved config hash or per-case camera hash.

### Two-Adapter Evidence Fields

PASS requires distinct hardware LUIDs, requested and actual Multi mode, nonempty secondary partition, GPU1 compute and graphics work, exact expected copy bytes for independent color/depth local-to-shared and shared-to-local legs, composite submission, calibrated fences, zero particle transfer bytes, and matching build/shader/config/driver/render provenance.

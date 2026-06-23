# MGPU Voxel Waterfall Methodology

Schema family: `mgpu_voxel_* v2` for benchmark manifests, execution manifests, raw frame rows, run rows, paired rows, invalid records, telemetry, visual validation, two-adapter preflight, and hostile analysis summary. Older producer v1 artifacts are legacy inputs only when explicitly accepted by `Tools/analyze_benchmark.py`.

## Research Questions And Hypotheses

- H1: Does validated explicit two-hardware-adapter rendering reduce paired run-level end-to-end `present_to_present_ms` for `SingleGpuFull` versus `MultiGpuFull`?
- H2: Does temporal decimation change secondary compute work while exact logical work equality and approximation-fidelity validation still pass?
- H3: Does Spatial Density LOD monotonically reduce submitted counts without changing simulation counts and while approximation-fidelity validation still passes?
- RQ3: Are cross-adapter transfer and composite costs transfer-dominated under the predeclared `transfer_ms / critical_path_gpu_ms >= 0.5` criterion?

No document may state H1/H2/H3 support until a strict `PASS` analysis archive exists with the corresponding confidence interval and complete validation evidence.

## Workload Terminology

Preset labels such as `static_budget_100k` identify requested static voxel budget. Dynamic voxels are configured separately and are added on top. The artifact fields `actual_static_count`, `actual_dynamic_count`, and `actual_total_count` are the only total-count evidence.

The workload is a deterministic synthetic voxel waterfall scene with static canyon/basin geometry and dynamic voxel particles. It is a graphics and scheduling workload, not a physically correct fluid simulation.

## Experimental Unit And Pairing

The inference unit is one independent run/repetition. Frames are descriptive samples used to recompute a run mean and validity; frames are not independent observations for confidence intervals.

Pairing uses explicit fields, not encoded `pair_id` semantics. A valid pair must match:

- `session_id`, `pair_id`, `block_id`, and `repetition`;
- camera hash, validation protocol hash, render resolution/formats/sample count;
- randomization seed, static workload seed, dynamic workload seed where exported;
- requested and actual workload counts, temporal policy/interval, LOD state, secondary share, partition/chunk settings, and logical work.

`pair_id` is an identifier only. It does not itself contain seed, resolution, or resolved-count evidence.

`resolved_config_hash` is the identity of one concrete resolved execution configuration. It is expected to be mode-specific because `SingleGpuFull`/`MultiGpuFull` and `SingleGpuTemporalDecimation`/`MultiGpuTemporalDecimation` have different requested/actual modes and may have different validation candidate/reference config hashes. It is therefore required to be present and valid for each run, but it is not part of the Single/Multi pair key and equality across the pair is not required.

Pair identity is `run_id` + `session_id` + `pair_id` + `block_id` + `repetition` plus explicit equality of all mode-independent invariants listed above. `validation_config_hash` may be mode-specific; the analyzer checks each run's validation evidence without using cross-mode equality of that hash as a pairing condition.

## Endpoints

Primary endpoint: paired run-level mean `present_to_present_ms`, measured as the interval between successful `Present()` calls. This is the endpoint used for rendering speed claims.

Secondary endpoints:

- `cpu_submission_ms`: time from after frame-resource acquire to `EndFrameCpu`;
- `frame_resource_backpressure_ms`: wait before CPU submission caused by frame-resource availability;
- `cpu_total_frame_ms`: backpressure plus submission;
- `critical_path_gpu_ms`, `gpu_work_sum_ms`, compute/graphics/copy/composite timings, transfer bytes, submitted counts, and validation metrics.

`cpu_submission_ms` must not be described as full CPU frame time.

## Validation Matrix

Visual validation protocol v2 is resolved after the actual benchmark configuration is applied and GPU resources are rebuilt. Hashes are canonical JSON/SHA-256, not raw C++ struct bytes.

Smoke validation matrix:

- static budget label `100k`, requested dynamic budget from the Smoke config;
- secondary share `0.50`;
- LOD off/on;
- mode families Full and Temporal;
- Temporal interval `2`;
- each case includes deterministic checkpoints required by the protocol.

Full validation matrix:

- static budget labels `100k`, `250k`, `500k`, `1m`;
- secondary shares `0.25`, `0.50`, `0.75`;
- LOD off/on;
- mode families Full and Temporal;
- Temporal interval `4`;
- no repetition/order dimension in validation coverage.

Two validation kinds are distinct:

- implementation equivalence: Single versus Multi for the same policy;
- approximation fidelity: Temporal/LOD variant versus Full + LOD-off reference.

H2/H3 cannot be supported by implementation equivalence alone.

PASS criteria are the exported threshold fields for color/depth/combined mismatch percentages and coverage mismatch. MAE, RMSE, PSNR, and max error are descriptive unless named as threshold fields in the validation artifact.

## Benchmark Gate

The benchmark gate compares compatible provenance records across suite manifest, visual validation, two-adapter preflight, and execution manifests. Required fields include:

- `build.executable_sha256`;
- `build.shader_bytecode_set_sha256`;
- `adapter.luid_pair`;
- `adapter.primary_driver_version`;
- `adapter.secondary_driver_version`;
- `validation.protocol_sha256`;
- `validation.case_config_sha256`;
- `validation.camera_sha256`;
- `render.resolution`, `render.color_format`, `render.depth_format`, `render.sample_count`;
- `workload.static_seed`, `workload.dynamic_seed`;
- requested and actual static/dynamic counts;
- `workload.temporal_interval`, `workload.spatial_lod`, `workload.chunk_size`;
- `runtime.toolchain`.

Stale camera/config/protocol/build/shader/adapter evidence blocks the suite. Missing visual validation or missing two-adapter preflight blocks the suite. Placeholder evidence is invalid.

## State And Status Semantics

Producer suite/execution statuses:

```text
PENDING -> RUNNING -> COMPLETE
PENDING/RUNNING -> BLOCKED | INVALID | CANCELLED | INTERRUPTED
```

`COMPLETE` is allowed only when every manifest config has exactly one execution manifest, every execution is `COMPLETE`, warmup/measured frame counts match exactly, required pairs/repetitions are valid, and no unfinished manifests remain.

`PASS` is not a suite status. `PASS` is used for visual validation, two-adapter verification, and hostile analysis summaries.

## Statistical Decision Rules

For valid H1 pairs:

- paired difference: `single_run_mean_ms - multi_run_mean_ms`;
- log speedup: `log(single_run_mean_ms / multi_run_mean_ms)`;
- aggregate speedup: `exp(mean(log_speedup))`.

Confidence intervals use two-sided Student-t over paired run-level values. For `n < 2`, CI fields are `null`/`NOT_MEASURED`, never zero-width.

Decision rules:

- H1 `SUPPORT` uses only `SingleGpuFull -> MultiGpuFull` on `present_to_present_ms`; it requires strict analysis `PASS`, complete validation/preflight evidence, paired `n >= 2`, and a 95% CI for `single - multi` whose lower bound is above zero.
- H2 `SUPPORT` uses only `MultiGpuFull -> MultiGpuTemporalDecimation` on `secondary_compute_ms`; it requires strict analysis `PASS`, paired `n >= 2`, a measured 95% CI that does not cross zero, exact logical-work and fixed-step equality, and Temporal approximation-fidelity PASS.
- H3 `SUPPORT` uses matched `LOD off -> LOD on` contrasts within each requested mode on `primary_submitted_voxels + secondary_submitted_voxels`; it requires strict analysis `PASS`, monotonic/non-increasing LOD-on submitted counts, unchanged simulation counts/logical work/fixed steps, and LOD approximation-fidelity PASS.
- RQ3 reports transfer dominance only when the lower CI bound for `transfer_ms / critical_path_gpu_ms` is at least `0.5`.

The Full suite default `n=3` is preliminary. Publication runs must predeclare repetition count using `Tools/plan_benchmark_power.py` from pilot SD and target MDE or desired CI width. Per-config contrasts beyond the primary contrast are exploratory and require multiplicity control.

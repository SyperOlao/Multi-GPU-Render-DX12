# MGPU-VoxelWaterfall

`MGPU-VoxelWaterfall` is a DirectX 12 sample for explicit multi-adapter voxel simulation and rendering. The scene contains one logical synthetic voxel waterfall. It is a deterministic graphics workload, not a physically correct fluid simulation.

## Architecture

- `VoxelSceneWorkload` owns the logical dataset: transform, seed, total voxel count, simulation parameters, deterministic chunk ownership, temporal policy, and spatial LOD policy.
- `PrimaryPartition` and `SecondaryPartition` contain disjoint `GlobalVoxelId` sets. Empty partitions have count 0 and do not create placeholder voxels.
- `VoxelGpuPartition` is adapter-local. Once created, its buffers, descriptors, root signatures, PSOs, indirect draw arguments, and constant buffers belong to one `GDevice`.
- Mode changes rebuild adapter-local partitions after a controlled flush instead of migrating resources between devices.
- GPU virtual addresses and descriptors are never shared between command lists from different adapters.

## Multi-GPU Frame

GPU 0:

- simulates primary-owned voxels;
- renders the ordinary scene and primary-owned voxels;
- owns the swap chain and final present;
- receives secondary color plus linear depth;
- runs depth-aware composition.

GPU 1:

- simulates secondary-owned voxels;
- records and executes a real graphics command list;
- issues the secondary voxel draw through the indirect render path;
- renders into GPU 1-local color and linear-depth textures;
- transfers only color plus linear depth to GPU 0.

```text
GPU0 compute -> GPU0 base graphics -----------------------------\
GPU1 compute -> GPU1 voxel graphics -> local-to-shared copy -> GPU0 shared-to-local copy
                                                                 \-> depth composition -> resolve/UI/present
```

The primary base graphics pass does not wait for the secondary image before it starts. Final composition waits for the copied secondary render output.

## Rendering Policies

Temporal Decimation is a simulation cadence policy. Full modes simulate both partitions every fixed step. Temporal modes update the secondary partition every configured interval while rendering the latest state every display frame with the current camera.

Spatial Density LOD is a render-list policy. The simulation dataset is unchanged. Each adapter builds its own compacted render list:

- LOD0 uses a `1x1x1` cell block.
- LOD1 uses a `2x2x2` cell block.
- LOD2 uses a `4x4x4` cell block.

For every occupied voxel selected for rendering, the build shader computes a signed cell coordinate and then derives a true 3D group key:

```text
groupCoord = floorDiv(signedCellCoord, blockSize)
```

Static streams use `signedCellCoord = GridOrigin + localGridCoordinate`, and camera distance is evaluated from the actual world-space cell center plus the object transform. Static LOD never reconstructs position from waterfall width, depth, spawn height, or floor height. Dynamic streams use the particle continuous position divided by the dynamic voxel size. Each adapter owns a local hash table for group claims and emits exactly one render item per occupied `(lod, groupCoord)` key; the representative particle is only metadata.

The emitted `VoxelLodRenderItem` stores explicit previous/current aggregate centers, explicit half extents, LOD level, material, stream kind, and optional representative index. The draw shader renders that aggregate AABB directly instead of scaling a cube around an arbitrary representative particle.

Temporal Decimation and Spatial Density LOD are independent and can be benchmarked in matching combinations.

## Synthetic Waterfall

The dynamic waterfall is a deterministic visual workload, not a fluid solver. Small preset uses the authored waterfall width/depth to build a logical spawn grid and distributes `GlobalVoxelId` values across the full X/Z grid with a stable per-cycle permutation. Recycle uses the same X/Z mapping, so a fixed seed produces the same curtain and histogram every run.

The simulation remains fixed-step with interpolation. Near the floor, particles are clamped into a shallow deterministic basin layer, spread laterally inside bounded pool extents, and recycle after a deterministic basin residence time. This keeps a readable curtain and pool without increasing particle count or using oversized LOD cubes to hide sparse LOD0 density.

## Composition

GPU 1 transfers linear depth, not hardware depth. GPU 0 samples primary hardware depth through a typeless-compatible depth resource and converts it to the same linear convention. The composite pass chooses secondary color only when secondary linear depth is valid and closer than primary depth within the configured convention.

## Benchmarking

Automatic benchmarks compare only matching configurations:

- `SingleGpuFull` vs `MultiGpuFull`;
- `SingleGpuTemporalDecimation` vs `MultiGpuTemporalDecimation`;
- each pair with Spatial Density LOD off and on.

Runs fix the randomization seed, static workload seed, dynamic workload seed, deterministic voxel ids, secondary share, render resolution, temporal interval, LOD policy, and reset state. Raw CSV files contain frame samples. The hostile analyzer recomputes runs and paired speedups from raw frames using the predeclared primary endpoint `present_to_present_ms`.

The raw CSV separates `frame_resource_backpressure_ms`, `cpu_submission_ms`, `cpu_total_frame_ms`, and `present_to_present_ms`. Submission time is not reported as full CPU frame time. The raw CSV also includes calibrated critical-path GPU time, GPU work sum, compute, LOD compaction, graphics, copy, composite, transfer bytes, submitted voxel counts, LOD counts, requested/actual mode, and visual validation metrics.

Automatic benchmark suites are fail-closed. They start measurement only after deterministic visual validation and explicit two-hardware-adapter verification both report `PASS` for compatible `mgpu_research_provenance.v1` records: executable SHA-256, shader bytecode-set SHA-256, adapter LUID pair, adapter driver identity, validation protocol/case/camera hashes, render formats/resolution/sample count, workload seeds/counts, temporal/LOD/partition/chunk settings, and runtime/toolchain identity.

Smoke suite:

- command: `MGPU-VoxelWaterfall.exe --benchmark-smoke --benchmark-output-dir=<fresh-empty-dir>`;
- optional deterministic randomization seed override: `--benchmark-seed=<uint>`;
- optional predeclared repetition override: `--benchmark-repetitions=<n>`;
- 8 executions: `SingleGpuFull`, `MultiGpuFull`, `SingleGpuTemporalDecimation`, `MultiGpuTemporalDecimation` crossed with Spatial LOD off/on;
- Low canonical static-budget 100k preset, secondary share 0.5, recorded temporal interval 2;
- 1 repetition, 30 warm-up frames, 120 measured frames;
- intended after every code change.

Full suite:

- command: `MGPU-VoxelWaterfall.exe --benchmark-full --benchmark-output-dir=<fresh-empty-dir>`;
- optional deterministic randomization seed override: `--benchmark-seed=<uint>`;
- optional predeclared repetition override: `--benchmark-repetitions=<n>`;
- static budget labels 100k, 250k, 500k, 1m; dynamic voxels are added on top and actual total count is recorded separately;
- Full and Temporal Single/Multi modes, shares 0.25/0.5/0.75, Spatial LOD off/on;
- preliminary profile: 3 repetitions, 100 warm-up frames, 500 measured frames;
- publication profile: predeclare `--benchmark-repetitions=<n>` from `Tools/plan_benchmark_power.py` using pilot SD and target MDE or CI width before running;
- deterministic randomized order with the recorded seed;
- intended as an overnight run after PASS Smoke, visual validation, and hardware verification.

Expected COMPLETE-suite output files:

- `VoxelBenchmark_<Suite>_Status.json` for suite PASS/RUNNING/COMPLETE/BLOCKED state;
- `manifest.json` with suite, run id, gates, exact matrix, hashes, statistical method, and exclusion rules;
- `environment.json` with git, executable/DLL/shader hashes, OS, CPU/RAM, GPU identifiers, drivers, display, debug layer, process, and timestamp metadata. Unknown values include a reason;
- `two_adapter_preflight.json` from explicit hardware verification;
- `voxel_visual_validation.json` and `voxel_visual_validation.csv`;
- `raw_frames.csv`, the concatenated frame-level records;
- one raw CSV per execution for traceability;
- `runs.csv`, the independent repetition/run-level records;
- `paired_summary.csv`, the paired aggregate summary;
- `invalid_records.csv`, the predeclared exclusion journal;
- `telemetry.csv`, the per-run telemetry summary;
- one per-execution manifest JSON with run id, pair id, resolved counts, validation ids, hashes, and status;
- `README.txt` with exact reproduction commands for the artifact directory.

Optional output files:

- `memory_timeline.csv` is not produced by automatic benchmark suites. It is produced only by memory soak/rebuild commands.

Exit codes:

- `0`: suite reached producer `COMPLETE` with every execution complete and valid, or explicit verification passed;
- `2`: failed or invalid runtime result;
- `3`: blocked precondition, including missing validation PASS or incompatible two-adapter hardware.
- `4`: cancelled or interrupted benchmark run.

`--verify-two-adapter` is an inventory/preflight/runtime-verification command. If the machine lacks a compatible second hardware adapter, it exits `3` and writes BLOCKED evidence instead of falling back to Single mode.

## Validation

Visual validation is numeric, not based on submission flags. A valid result requires color/depth comparison metrics such as MAE, RMSE, PSNR, maximum error, mismatch percentages, and depth RMSE. Benchmark rows without available validation metrics are marked invalid and are excluded from speedup.

The validation runner fails closed when deterministic Single/Multi validation textures and `VoxelValidationCompare.hlsl` tile statistics are not supplied. When the GPU capture path provides completed color/depth compare stats, the runner performs CPU final reduction, exports color/depth metric rows, and links the result to build hash, shader hash, adapter pair, config hash, and camera hash.

## Research Protocol

Research questions:

- Does explicit two-hardware-adapter rendering reduce end-to-end `present_to_present_ms` and calibrated critical-path GPU time for the deterministic synthetic voxel waterfall workload?
- How do temporal decimation and spatial density LOD affect the Single/Multi paired difference?
- Do cross-adapter color/depth transfers dominate or amortize under the tested workload sizes and secondary shares?

Hypotheses:

- H1: A validated `MultiGpuFull` run can reduce run-mean end-to-end `present_to_present_ms` versus its matched `SingleGpuFull` block when the secondary partition is non-empty and render-output transfer is active.
- H2: Temporal decimation reduces GPU1 compute work but must not invalidate visual validation or matching semantics.
- H3: Spatial Density LOD reduces submitted voxel counts monotonically without changing simulation counts.

Primary endpoint:

- Run-level mean `present_to_present_ms` for explicit matched Single/Multi pairs.

Secondary metrics:

- `cpu_submission_ms`, `cpu_total_frame_ms`, calibrated critical-path GPU time, GPU work sum, compute/graphics/copy/composite timing, transfer bytes, submitted/rendered voxel counts, LOD counts, validation metrics, and invalid frame counts.

Workload definition:

- The workload is a deterministic synthetic voxel waterfall scene with static canyon/basin geometry and dynamic voxel particles. It is a graphics benchmark and is not a physically correct fluid simulation.

Validation criteria:

- Benchmark measurement starts only after deterministic visual validation and explicit two-hardware-adapter verification both pass for compatible provenance: executable SHA-256, actual shader bytecode-set SHA-256, adapter LUID pair, adapter-specific driver identity, validation protocol/config/camera hashes, resolution/formats/sample count, workload seeds/counts, temporal/LOD/partition/chunk settings, and runtime/toolchain identity.
- Requested Multi mode falling back to Single is `BLOCKED` or `INVALID`, not a valid comparative result.

Experimental procedure:

- Run Smoke after every code change.
- Run Full only after PASS Smoke, PASS visual validation, and PASS two-GPU verification on compatible hardware.
- Full uses three independent repetitions per configuration. The randomized order is deterministic and logged by seed.
- Pair/block IDs identify matching blocks. Seed, resolved counts, LOD policy, temporal policy, resolution, requested secondary share, and repetition are separate fields that the analyzer must compare directly. Pair blocks are randomized as a complete block design, and Single-first versus Multi-first order is logged.
- Multiple sessions can be run by selecting different output directories and seed overrides; each session writes its own manifest.

Exclusion rules:

- Exclusions are predeclared in `manifest.json` and recorded in `invalid_records.csv`.
- No outliers are removed silently.
- Invalid timestamp calibration invalidates dependent cross-queue metrics.
- Missing visual validation, missing matching pair, mode fallback, nonzero particle transfer, zero Multi render-output transfer, or contradictory draw/partition records invalidate the row.

Statistical method:

- The experimental unit for inference is an independent benchmark repetition/run, not an individual frame.
- Frames inside a run are descriptive distribution samples and contribute run mean/median/P95/P99 only.
- Aggregate confidence intervals use a two-sided Student-t interval over run means. With three repetitions this uses `df=2`, not a normal `1.96` approximation.
- Paired speedup is computed only after matching run-level aggregation.
- `speedup / 2` is reported as `TwoDeviceNominalEfficiency`; it is not a capacity-normalized heterogeneous multi-GPU efficiency.

Threats to internal validity:

- Thermal drift, driver scheduling, background processes, power plan changes, display topology, debug layer/GPU validation state, and invalid timestamp calibration can affect timings.
- Cross-adapter support and driver versions are adapter-pair specific.
- Visual validation must pass for the exact build/shader/adapter pair; stale validation results are rejected.

Threats to external validity:

- Results apply to this synthetic voxel workload and tested hardware pair. They should not be generalized to physically correct fluid simulation, unrelated renderers, or heterogeneous adapters without new validation.

Reproducibility instructions:

- Use the commands recorded in each artifact directory `README.txt`.
- Re-run offline analysis with:

```text
python MGPU-VoxelWaterfall\Tools\analyze_benchmark.py --input <artifact-dir>
```

- The analysis script reads only raw artifacts, validates schema/hash consistency, recomputes summaries, and exits nonzero for `FAIL` or `BLOCKED`.

## Requirements And Limits

- Multi-GPU requires a distinct compatible hardware adapter with graphics, compute, copy queues, shared fences, and cross-adapter row-major texture support.
- Cross-adapter transfer moves only render output: color plus linear depth.
- Floating-point differences between adapters are expected; validation uses tolerances, not bitwise equality.
- The waterfall is a synthetic voxel workload. It should not be described as physically correct liquid.

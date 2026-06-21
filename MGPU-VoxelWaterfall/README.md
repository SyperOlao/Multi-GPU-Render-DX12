# MGPU-VoxelWaterfall

`MGPU-VoxelWaterfall` is a DirectX 12 sample for explicit multi-adapter voxel simulation and rendering.
The scene contains one logical synthetic voxel waterfall, split in object space into deterministic non-overlapping partitions.

The sample is a graphics workload, not a physically correct water simulation. Each voxel is represented as one structured-buffer particle and rendered as an opaque cube.

## Architecture

- One `VoxelWaterfallWorkload` owns the world transform, simulation parameters, seed, total voxel count, and partition rule.
- The workload is split into `PrimaryPartition` and `SecondaryPartition`.
- Partition ownership is chunk-based and deterministic from global voxel ids and `SecondaryShare`.
- `SingleGpuFull` creates both partitions on GPU 0.
- `MultiGpuFull` creates `PrimaryPartition` on GPU 0 and `SecondaryPartition` on GPU 1.
- Temporal decimation modes use the same scene and partition rule, but update partitions according to the temporal policy.

## Multi-GPU Rendering

GPU 0:

- simulates and renders `PrimaryPartition`;
- renders the ordinary scene into the primary forward/SSAA target;
- owns the swap chain;
- receives secondary color and linear depth;
- runs depth-aware composition, UI, resolve, and present.

GPU 1:

- simulates `SecondaryPartition`;
- records and executes its own graphics command list;
- draws secondary voxels into GPU 1-local offscreen color and linear-depth render targets;
- copies only those render outputs through cross-adapter bridge textures.

No particle pool, alive list, dead list, injection buffer, or full simulation state is copied from GPU 1 to GPU 0 for rendering.

## Frame Graph

```text
GPU0 compute
GPU1 compute
GPU0 base graphics
GPU1 voxel graphics
GPU1 local-to-shared copy
GPU0 shared-to-local copy
GPU0 depth composition
GPU0 resolve/UI/present
```

`GPU0 base graphics` and the `GPU1 compute -> graphics -> copy` chain are submitted independently. GPU 0 waits for the secondary image only before final composition.

## Cross-Adapter Transfer

For each frame resource, GPU 1 owns:

- `SecondaryLocalColor`;
- `SecondaryLocalLinearDepth`;
- `SecondaryLocalDepthStencil`.

The cross-adapter bridge contains:

- shared color texture;
- shared linear-depth texture.

GPU 0 owns:

- `PrimaryReceivedSecondaryColor`;
- `PrimaryReceivedSecondaryLinearDepth`;
- `PrimaryCompositeColor`.

The secondary DSV is local to GPU 1 and is never transferred.

## Composition

Composition is depth-aware. The shader samples:

- primary base color;
- primary hardware depth converted to linear view depth;
- received secondary color;
- received secondary linear view depth.

If the secondary pixel alpha is zero, primary color is kept. Otherwise, secondary color is selected only when its linear depth is valid and closer than the primary linear depth by the configured epsilon.

## Execution Modes

- `SingleGpuFull`: both partitions simulate and render on GPU 0.
- `MultiGpuFull`: primary partition runs on GPU 0; secondary partition runs on GPU 1.
- `SingleGpuTemporalDecimation`: same scene and quality policy on GPU 0 with temporal update decimation.
- `MultiGpuTemporalDecimation`: same temporal policy with partitions distributed across adapters.

When a second compatible hardware adapter is unavailable, multi-GPU modes are disabled with an explicit reason. UI, telemetry, and benchmark CSVs report requested and actual modes separately.

## Benchmarking

Automatic benchmarks use matching baselines only:

- `MultiGpuFull` is compared with `SingleGpuFull`.
- `MultiGpuTemporalDecimation` is compared with `SingleGpuTemporalDecimation`.

Configurations keep the same seed, global voxel ids, total voxel count, partition rule, camera, viewport, render resolution, simulation steps, visual quality policy, and reset state. The benchmark includes secondary shares of 25%, 50%, and 75%, with deterministic configuration ordering.

Summary CSV columns include:

- `requested_mode`;
- `actual_mode`;
- `primary_adapter`;
- `secondary_adapter`;
- `total_voxels`;
- `secondary_share`;
- `temporal_policy`;
- `render_width`;
- `render_height`;
- `average_cpu_frame_ms`;
- `critical_path_gpu_ms`;
- `gpu_work_sum_ms`;
- `primary_graphics_ms`;
- `secondary_graphics_ms`;
- `transfer_ms`;
- `composite_ms`;
- `transfer_bytes`;
- `particle_transfer_bytes`;
- `speedup_vs_matching_single_gpu`;
- `efficiency`;
- `visual_validation_passed`.

`critical_path_gpu_ms` is computed from calibrated queue timestamps in the common CPU/QPC time domain. `gpu_work_sum_ms` is recorded separately and is not treated as frame latency.

## Limitations

- The waterfall is a deterministic synthetic voxel workload, not a Navier-Stokes fluid solver.
- Voxels do not exert pressure or collision forces on each other.
- Multi-GPU requires a distinct compatible hardware adapter with graphics/compute/copy queues and cross-adapter row-major texture support.
- Cross-adapter render-output transfer has measurable bandwidth and synchronization cost.
- Floating-point results can differ slightly across adapters; validation must use tolerances rather than bitwise equality.

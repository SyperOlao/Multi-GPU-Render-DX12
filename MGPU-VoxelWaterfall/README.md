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

- LOD0 renders all voxels near the camera.
- LOD1 renders one representative from each stable group of 8 and scales it by 2.
- LOD2 renders one representative from each stable group of 64 and scales it by 4.

Temporal Decimation and Spatial Density LOD are independent and can be benchmarked in matching combinations.

## Composition

GPU 1 transfers linear depth, not hardware depth. GPU 0 samples primary hardware depth through a typeless-compatible depth resource and converts it to the same linear convention. The composite pass chooses secondary color only when secondary linear depth is valid and closer than primary depth within the configured convention.

## Benchmarking

Automatic benchmarks compare only matching configurations:

- `SingleGpuFull` vs `MultiGpuFull`;
- `SingleGpuTemporalDecimation` vs `MultiGpuTemporalDecimation`;
- each pair with Spatial Density LOD off and on.

Runs fix the seed, deterministic voxel ids, secondary share, render resolution, temporal interval, LOD policy, and reset state. Raw CSV files contain frame samples. The summary CSV aggregates repetitions by configuration first, then computes speedup from aggregated matching baselines using `mean_cpu_frame_ms`.

The raw CSV includes CPU frame time, calibrated critical-path GPU time, GPU work sum, compute, LOD compaction, graphics, copy, composite, transfer bytes, submitted voxel counts, LOD counts, requested/actual mode, and visual validation metrics.

## Validation

Visual validation is numeric, not based on submission flags. A valid result requires color/depth comparison metrics such as MAE, RMSE, PSNR, maximum error, mismatch percentages, and depth RMSE. Benchmark rows without available validation metrics are marked invalid and are excluded from speedup.

Current limitation: the validation runner fails closed when deterministic Single/Multi validation textures are not captured, so those runs report invalid rather than a false pass.

## Requirements And Limits

- Multi-GPU requires a distinct compatible hardware adapter with graphics, compute, copy queues, shared fences, and cross-adapter row-major texture support.
- Cross-adapter transfer moves only render output: color plus linear depth.
- Floating-point differences between adapters are expected; validation uses tolerances, not bitwise equality.
- The waterfall is a synthetic voxel workload. It should not be described as physically correct liquid.

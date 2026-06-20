# MGPU-VoxelWaterfall

`MGPU-VoxelWaterfall` is a DirectX 12 sample for comparing single-GPU and split multi-GPU simulation of a simplified voxel waterfall.

The simulation intentionally keeps the original particle/structured-buffer architecture from `MGPU-Particles`: every voxel is represented as one structured-buffer element, updated by a compute shader and rendered as a small opaque cube/billboard through the particle rendering path.

## Distribution architecture

- Primary GPU:
  - simulates `NearVoxelWaterfall`;
  - renders Near, Medium, Far, and the scene;
  - owns the swap chain and graphics pass.
- Secondary GPU:
  - optionally simulates `MediumVoxelWaterfall`;
  - optionally simulates `FarVoxelWaterfall`.
- Cross-adapter resources:
  - Medium/Far simulation buffers are copied through the existing cross-adapter shared-resource path before primary GPU rendering.

## Execution modes

- `PrimaryOnly`
  - Near, Medium, and Far simulation all run on the primary GPU every frame.
- `SplitMultiGpu`
  - Near runs on primary GPU every frame.
  - Medium and Far run on secondary GPU every frame.
- `SplitMultiGpuLod`
  - Near runs on primary GPU every frame.
  - Medium runs on secondary GPU every 2 frames by default.
  - Far runs on secondary GPU every 4 frames by default.

If a second hardware adapter is unavailable, split modes are disabled and the sample remains in `PrimaryOnly`.

## Controls

The ImGui panel provides:

- execution mode selection;
- enable/disable per LOD;
- voxel count per LOD;
- voxel size, gravity, and waterfall height;
- Medium/Far update intervals;
- manual benchmark start/stop;
- automatic benchmark start/stop.

## CSV benchmark format

Per-test CSV files use:

`VoxelBenchmark_<Mode>_<Preset>_<TotalCount>.csv`

Columns:

- `frame_index`
- `execution_mode`
- `near_voxel_count`
- `medium_voxel_count`
- `far_voxel_count`
- `total_voxel_count`
- `updated_voxel_count`
- `medium_update_interval`
- `far_update_interval`
- `primary_adapter_name`
- `secondary_adapter_name`
- `primary_compute_ms`
- `secondary_compute_ms`
- `cross_adapter_transfer_ms`
- `graphics_ms`
- `primary_wait_ms`
- `secondary_wait_ms`
- `synchronization_ms`
- `cpu_frame_ms`
- `gpu_frame_ms`

The automatic benchmark also writes:

`VoxelBenchmark_Summary.csv`

Summary columns:

- `mode`
- `preset`
- `total_voxel_count`
- `average_frame_ms`
- `median_frame_ms`
- `p95_frame_ms`
- `average_primary_compute_ms`
- `average_secondary_compute_ms`
- `average_transfer_ms`
- `average_sync_ms`
- `average_graphics_ms`
- `target_60_fps_reached`

`target_60_fps_reached` is `true` when `average_frame_ms <= 16.67`.

## Running the automatic benchmark

1. Build and run `MGPU-VoxelWaterfall`.
2. Open the `Voxel Waterfall` ImGui window.
3. Press `Start Auto Benchmark`.
4. Wait until all configurations complete or press `Stop Auto Benchmark`.
5. Open CSV files in `VoxelBenchmarkResults`.

The automatic benchmark runs these presets:

- Low: 100,000 total voxels.
- Medium: 250,000 total voxels.
- High: 500,000 total voxels.
- VeryHigh: 1,000,000 total voxels.

Each preset is tested in:

- `PrimaryOnly`
- `SplitMultiGpu`
- `SplitMultiGpuLod`

Each configuration runs 100 warm-up frames and records 500 frames.

## Simulation limitations

- Voxels do not interact with each other.
- The water model is intentionally simplified and not physically correct.
- LOD distribution between GPUs is static.
- There is no automatic migration between GPUs.
- Performance depends heavily on secondary GPU capability.
- Cross-adapter transfer has a measurable cost.
- There is no collision grid or fluid pressure solve.

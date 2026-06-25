# Multi-GPU Voxel Rendering Contract

This document is the architecture contract for the first stage of the MGPU-VoxelWaterfall rendering refactor. It does not describe an implemented renderer. It defines the behavior that later changes must preserve.

## Scope

- The rendering strategy is object-space split, not split-screen SFR.
- The scene contains one logical voxel waterfall.
- The waterfall's voxel set is split into non-overlapping partitions.
- Partitions are simulation and rendering ownership boundaries. A voxel belongs to exactly one partition for a frame.
- GPU 0 owns the swap chain and all final presentation work.
- GPU 1 contributes rendered pixels for its voxel partition, not particle pool data for GPU 0 to draw later.

## Required Frame Ownership

GPU 0:

- Simulates the primary voxel partition.
- Renders the ordinary scene and the primary voxel partition.
- Owns the swap chain.
- Performs final depth-aware composition.
- Runs resolve, UI, and present after composition.

GPU 1:

- Simulates the secondary voxel partition.
- Creates and executes a real graphics command list on its graphics queue.
- Issues Draw calls for the secondary voxel partition.
- Renders the secondary partition into GPU 1-owned offscreen color and linear-depth textures.
- Transfers only the rendered result to GPU 0: color plus linear depth.

## Required Data Flow

- GPU 1 must not send particle pools, alive lists, dead lists, or other per-particle simulation buffers to GPU 0 for rendering in multi-GPU render mode.
- GPU 1 output is two render results: color and linear depth.
- GPU 1 first copies its local offscreen color/depth results into cross-adapter/shared textures.
- GPU 0 then copies those shared textures into GPU 0-local per-frame textures before composition.
- Final composition compares GPU 1 linear depth against GPU 0 scene/primary depth and writes the correct visible color.
- A simple alpha overlay is not a valid composition path.

## Graphics Pipeline Split

The primary graphics pass must be split into:

1. GPU 0 base rendering: ordinary scene plus primary voxel partition into GPU 0-local render targets.
2. GPU 0 final composition: consumes GPU 0 base color/depth plus GPU 1 copied color/linear-depth.

GPU 0 base rendering and the GPU 1 chain must start in parallel as far as dependencies allow. Waiting for GPU 1 is allowed only before the final depth-aware composition that consumes GPU 1 results.

## Queue Timeline

One multi-GPU frame must follow this queue timeline:

```text
GPU0 compute
  - simulate primary voxel partition
  - signal primary partition ready

GPU1 compute
  - simulate secondary voxel partition
  - signal secondary partition ready for GPU1 graphics

GPU0 base graphics
  - wait for GPU0 compute
  - render ordinary scene and primary voxel partition
  - produce GPU0 base color and depth

GPU1 voxel graphics
  - wait for GPU1 compute
  - render secondary voxel partition with real Draw calls
  - produce GPU1 offscreen color and linear depth

GPU1 local-to-shared copy
  - wait for GPU1 voxel graphics
  - copy GPU1 local color and linear depth to cross-adapter shared textures
  - signal shared color/depth ready

GPU0 shared-to-local copy
  - wait for GPU1 shared color/depth ready
  - copy shared color and linear depth to GPU0-local per-frame textures
  - signal GPU0-local secondary render result ready

GPU0 depth composition
  - wait for GPU0 base graphics
  - wait for GPU0 shared-to-local copy
  - depth-aware compose GPU0 base color/depth with secondary color/linear-depth

GPU0 resolve/UI/present
  - run resolve/post UI work
  - present through the GPU0-owned swap chain
```

Concurrency requirement:

```text
GPU0 compute  -> GPU0 base graphics  --------------------\
GPU1 compute  -> GPU1 voxel graphics -> local-to-shared -> GPU0 shared-to-local copy
                                                          \-> GPU0 depth composition -> resolve/UI/present
```

`GPU0 base graphics` and the GPU1 `compute -> voxel graphics -> local-to-shared copy` chain must overlap whenever their dependencies permit.

## Synchronization Contract

- All normal per-frame inter-queue and inter-adapter waits must be GPU fence waits through command queues.
- CPU waits such as `Flush`, `WaitForFenceValue`, event waits, or blocking readback waits are forbidden in the normal frame path.
- CPU waits are allowed only for initialization, resize/recreation, shutdown, explicit benchmark teardown, or explicit user-triggered mode/resource reconfiguration.
- Every resource that can be referenced by multiple frames in flight must have a distinct version per `FrameResource`.
- Fence values used by multiple queues must be tracked per frame or otherwise proven not to alias between frames in flight.

## Adapter Availability

- Multi-GPU render mode is available only when a second compatible hardware adapter exists.
- Compatibility requires at least:
  - a distinct secondary hardware adapter, not WARP and not the same logical device;
  - successful cross-adapter shared fence creation/opening;
  - support for the cross-adapter row-major texture path used by `GCrossAdapterResource`;
  - successful allocation/opening of the required shared color and linear-depth resources.
- If these checks fail, multi-GPU render mode must be explicitly unavailable.
- The program must not silently fall back to single GPU while still labeling the frame, UI, benchmark row, or log as multi-GPU.

## Cross-Adapter Resource Constraints Found In The Current Engine

- `GDevice::Initialize` queries `D3D12_FEATURE_D3D12_OPTIONS::CrossAdapterRowMajorTextureSupported`.
- `GCrossAdapterResource` forces `D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER`.
- `GCrossAdapterResource` forces `D3D12_TEXTURE_LAYOUT_ROW_MAJOR`.
- `GCrossAdapterResource` creates a shared heap with `D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER`.
- The prime device creates the heap and shared handle; the secondary device opens the same heap handle.
- Both devices create placed resources at heap offset 0 over that shared heap.
- The allocation size is based on `GetCopyableFootprints` row pitch times height, then aligned.
- Current command queues are created per device for graphics/direct, compute, and copy.
- Shared fences are created with `D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER`, then opened on the other device.
- Texture movement between adapters is modeled as local resource -> shared resource on one adapter, then shared resource -> local resource on the other adapter.
- The existing SFR example uses cross-adapter backbuffer copies and split-screen/scissor regions; that is not the voxel renderer contract.
- Copy-back samples can move simulated particle data back to GPU 0 and draw on GPU 0; that is not the multi-GPU render contract.

## Studied Engine APIs And Existing Paths

- `VoxelWaterfallApp::Draw`, `InitDevices`, `ApplyExecutionMode`, `InitFrameResource`, `OnResize`, `Flush`.
- `RenderPipeline::RenderFrame`.
- `VoxelRenderPasses::RecordFrame`, `RecordForwardPath`, `RecordFullQuad`, `RecordDraw`.
- `VoxelSimulationScheduler::DispatchFrame`.
- The voxel emitter simulation and draw path.
- The cross-adapter render-output bridge path.
- `VoxelBenchmarkProfiler::Initialize`, `BeginRange`, `EndRange`, `ResolveRange`, `SetQueueFence`, `IsFrameReady`.
- `GCrossAdapterResource` construction, `GetPrimeResource`, `GetSharedResource`, `Resize`, `Reset`.
- `GDevice::Initialize`, `IsCrossAdapterTextureSupported`, `SharedFence`, `TrySharedFence`, `ShareResource`, `GetCommandQueue`.
- `GCommandQueue::ExecuteCommandList`, `Signal`, `Wait`, `WaitForFenceValue`, `Flush`.
- `GCommandList::CopyResource`, `CopyTextureRegion`, `Draw`, `Dispatch`, `TransitionBarrier`, `SetRenderTargets`.
- `MGPU-SFR` split-frame rendering, shared backbuffer, shared fence, cross-adapter copy, viewport/scissor split.
- `MGPU-Particles` cross-adapter compute, shared fences, simulated data copy-back, GPU0 drawing.

## Prohibited Substitutions

The following are explicitly not acceptable implementations of this contract:

- GPU 1 only performs compute.
- GPU 1 only copies data.
- GPU 1 computes particles, but all Draw calls remain on GPU 0.
- The entire scene is duplicated on both GPUs for half-screen SFR.
- The secondary image is composited by alpha overlay without a depth test.
- `Flush` or `WaitForFenceValue` is called every frame on the CPU path.
- A voxel simulation-data bridge remains as a parallel alternative path for multi-GPU rendering.
- Unfinished placeholders or temporary fallback paths are added to the code.

## Implementation Status

The MGPU-VoxelWaterfall implementation is expected to follow this contract directly: adapter-local voxel partitions, GPU 1 graphics for secondary-owned voxels, color plus linear-depth transfer, and GPU 0 depth-aware composition.

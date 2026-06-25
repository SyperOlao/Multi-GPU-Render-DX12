# Third Party Notices

This file records third-party dependencies required to build and package `MGPU-VoxelWaterfall`.

## ImGui

- Path: `Submodule/imgui`
- Upstream: `https://github.com/ocornut/imgui`
- Pinned commit: `9a5c070308ae97bf0f884b071d0262ae1cad26f7`
- License: MIT
- Local license file: `Submodule/imgui/LICENSE.txt`

## Assimp

- Package: `assimp-v143`
- Version: `5.4.3`
- Restored path: `packages/assimp-v143.5.4.3`
- License: BSD 3-Clause style license as shipped in `packages/assimp-v143.5.4.3/LICENSE`

## DirectX Tool Kit for DirectX 12

- Package: `directxtk12_uwp`
- Version: `2025.3.21.3`
- Restored path: `packages/directxtk12_uwp.2025.3.21.3`
- License: MIT per Microsoft DirectXTK project/package metadata. Verify against restored NuGet metadata before public release packaging.

## DirectXTex

- Package: `directxtex_uwp`
- Version: `2025.3.25.2`
- Restored path: `packages/directxtex_uwp.2025.3.25.2`
- License: MIT per Microsoft DirectXTex project/package metadata. Verify against restored NuGet metadata before public release packaging.

## DirectXMesh

- Package: `directxmesh_uwp`
- Version: `2025.3.25.2`
- Restored path: `packages/directxmesh_uwp.2025.3.25.2`
- License: MIT per Microsoft DirectXMesh project/package metadata. Verify against restored NuGet metadata before public release packaging.

## WinPixEventRuntime

- Package: `WinPixEventRuntime`
- Version: `1.0.240308001`
- Restored path: `packages/WinPixEventRuntime.1.0.240308001`
- License/redistribution: Microsoft package terms. Verify package metadata and redistribution permissions before public release packaging.

## Repository-local engine projects

The following projects are repository-local code and are built as static libraries:

- `Allocator`
- `Common`
- `Graphics`
- `Utils`

Their licensing follows the main repository license status. See `LICENSE_STATUS.md`.

# Building MGPU-VoxelWaterfall

This repository is intended to build from a clean Windows checkout without neighboring sample projects beyond the files in this repo.

## Required Toolchain

- Windows 10/11 x64.
- Visual Studio 2022 Community, Professional, or Enterprise.
- Workload: `Desktop development with C++` (`Microsoft.VisualStudio.Workload.NativeDesktop`).
- Platform toolset: `v143`.
- Exact MSVC toolset directory required by scripts: `14.44.35207`.
- Exact Windows SDK: `10.0.26100.0`.
- Build architecture/configuration: `Release|x64`.
- Python 3.10+ on `PATH` for non-GPU tests.
- Git on `PATH` for submodule/bootstrap support.

The project file uses `v143`; `bootstrap.ps1` and `build.ps1` fail fast unless the exact MSVC toolset revision and Windows SDK above are installed.

## Dependencies

In-repository projects:

- `Common/Common.vcxproj`
- `Utils/Utils.vcxproj`
- `Graphics/Graphics.vcxproj`
- `Allocator/Allocator.vcxproj`
- `MGPU-VoxelWaterfall/MGPU-VoxelWaterfall.vcxproj`

Pinned external dependencies:

- ImGui submodule: `Submodule/imgui` at `9a5c070308ae97bf0f884b071d0262ae1cad26f7`.
- NuGet packages from `MGPU-VoxelWaterfall/packages.config`:
  - `assimp-v143` `5.4.3`
  - `directxmesh_uwp` `2025.3.25.2`
  - `directxtex_uwp` `2025.3.25.2`
  - `directxtk12_uwp` `2025.3.21.3`
  - `WinPixEventRuntime` `1.0.240308001`

The authoritative lock file is `dependency-lock.json`.

## Fresh Clone Build

```powershell
git clone --recurse-submodules <repo-url> Multi-GPU-Render-DX12
cd Multi-GPU-Render-DX12
.\MGPU-VoxelWaterfall\InternalBuild\bootstrap.ps1
.\MGPU-VoxelWaterfall\InternalBuild\build.ps1 -Configuration Release -Platform x64
```

`build.ps1` restores dependencies, builds `MGPU-VoxelWaterfall.sln`, runs non-GPU tests, and writes:

```text
artifacts/build/build_manifest.json
```

## ZIP Source Build

If the source arrives as a ZIP without `.git` metadata or submodule contents:

```powershell
Expand-Archive .\Multi-GPU-Render-DX12.zip
cd .\Multi-GPU-Render-DX12
.\MGPU-VoxelWaterfall\InternalBuild\bootstrap.ps1
.\MGPU-VoxelWaterfall\InternalBuild\build.ps1 -Configuration Release -Platform x64
```

`bootstrap.ps1` clones `Submodule/imgui` and checks out the pinned commit from `dependency-lock.json`.

## Manual CLI Build

After bootstrap:

```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -version "[17.0,18.0)" -latest -products * -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath
& "$vs\MSBuild\Current\Bin\MSBuild.exe" .\MGPU-VoxelWaterfall.sln /m /restore /p:Configuration=Release /p:Platform=x64
.\MGPU-VoxelWaterfall\InternalBuild\test.ps1
```

Expected binary output:

```text
x64/Release/MGPU-VoxelWaterfall.exe
x64/Release/*.dll
x64/Release/*.cso
```

## Build Manifest

`artifacts/build/build_manifest.json` records:

- compiler, linker, MSBuild, platform toolset, MSVC toolset revision, and Windows SDK;
- build flags relevant to research reproducibility;
- pinned submodule commit and NuGet package versions;
- executable SHA-256;
- loaded/package DLL SHA-256 for DLLs copied next to the executable;
- actual compiled shader bytecode SHA-256 for `.cso` blobs and the aggregate shader bytecode set SHA-256.

Paths in the manifest are normalized relative paths, not machine-local absolute paths.

## CI

The workflow `.github/workflows/mgpu-voxelwaterfall-ci.yml` runs on `windows-2022`:

1. checkout with submodules;
2. bootstrap dependencies;
3. build `Release|x64`;
4. run non-GPU reference/analyzer tests;
5. write `hardware_gpu_tests.json` with `SKIPPED/NOT_AVAILABLE`;
6. package binaries, shader bytecode, manifest, and notices.

Hardware visual validation and two-adapter runtime verification are not claimed as PASS in CI. Research reproduction and publication-artifact instructions live under `MGPU-VoxelWaterfall/Docs/` and `MGPU-VoxelWaterfall/Research/`.

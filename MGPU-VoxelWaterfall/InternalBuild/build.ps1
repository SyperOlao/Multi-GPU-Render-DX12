param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [ValidateSet("x64")]
    [string]$Platform = "x64",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$BuildRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $BuildRoot
$RepoRoot = Split-Path -Parent $ProjectRoot
$Artifacts = Join-Path $BuildRoot "artifacts\build"
$Solution = Join-Path $BuildRoot "MGPU-VoxelWaterfall.sln"
$Lock = Get-Content (Join-Path $BuildRoot "dependency-lock.json") -Raw | ConvertFrom-Json

function Fail($Message) { throw "[build] $Message" }

function Get-Vs2022Path {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $path = & $vswhere -version "[17.0,18.0)" -latest -products * -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath
    if (-not $path) { Fail "Visual Studio 2022 Native Desktop workload not found." }
    return $path.Trim()
}

function Get-Sha256($Path) {
    if (-not (Test-Path $Path)) { return $null }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Rel($Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $base = [System.IO.Path]::GetFullPath($ProjectRoot + [System.IO.Path]::DirectorySeparatorChar)
    if ($full.StartsWith($base, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($base.Length).Replace('\', '/')
    }
    return "external/" + (Split-Path -Leaf $full)
}

& (Join-Path $BuildRoot "bootstrap.ps1")

$vs = Get-Vs2022Path
$msbuild = Join-Path $vs "MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) { Fail "MSBuild not found at $msbuild" }

$solutionDir = [System.IO.Path]::GetFullPath($RepoRoot + [System.IO.Path]::DirectorySeparatorChar)
& $msbuild $Solution /m /restore /p:Configuration=$Configuration /p:Platform=$Platform "/p:SolutionDir=$solutionDir"
if ($LASTEXITCODE -ne 0) { Fail "MSBuild failed." }

if (-not $SkipTests) {
    & (Join-Path $BuildRoot "test.ps1")
    if ($LASTEXITCODE -ne 0) { Fail "Tests failed." }
}

New-Item -ItemType Directory -Force -Path $Artifacts | Out-Null
$binDir = Join-Path $RepoRoot "x64\$Configuration"
$exe = Join-Path $binDir "MGPU-VoxelWaterfall.exe"
if (-not (Test-Path $exe)) { Fail "Expected executable not produced: $exe" }

$toolset = [string]$Lock.visual_studio.msvc_toolset
$cl = Join-Path $vs "VC\Tools\MSVC\$toolset\bin\Hostx64\x64\cl.exe"
$link = Join-Path $vs "VC\Tools\MSVC\$toolset\bin\Hostx64\x64\link.exe"
$sdk = [string]$Lock.visual_studio.windows_sdk

$dlls = @{}
Get-ChildItem -Path $binDir -Filter *.dll -File | Sort-Object Name | ForEach-Object {
    $dlls[(Rel $_.FullName)] = Get-Sha256 $_.FullName
}
$shaders = @{}
Get-ChildItem -Path $binDir -Filter *.cso -File | Sort-Object Name | ForEach-Object {
    $shaders[(Rel $_.FullName)] = Get-Sha256 $_.FullName
}
$shaderSetText = "mgpu_voxel_shader_bytecode_set.v1`n"
$shaders.GetEnumerator() | Sort-Object Name | ForEach-Object {
    $shaderSetText += "$($_.Name)=$($_.Value)`n"
}
$sha = [System.Security.Cryptography.SHA256]::Create()
$shaderSetHash = [System.BitConverter]::ToString(
    $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($shaderSetText))
).Replace("-", "").ToLowerInvariant()
$sha.Dispose()

$packages = @{}
$Lock.nuget_packages.PSObject.Properties | ForEach-Object {
    $dirName = "$($_.Name).$($_.Value)"
    $dirPath = Join-Path (Join-Path $RepoRoot "packages") $dirName
    $packages[$dirName] = @{ path = Rel $dirPath; exists = (Test-Path $dirPath) }
}
$imguiCommit = if (Test-Path (Join-Path $RepoRoot "Submodule\imgui\.git")) { (git -C (Join-Path $RepoRoot "Submodule\imgui") rev-parse HEAD).Trim() } else { "unknown" }

$manifest = [ordered]@{
    schema = "mgpu_voxel_build_manifest.v1"
    created_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    solution = "MGPU-VoxelWaterfall.sln"
    configuration = $Configuration
    platform = $Platform
    toolchain = [ordered]@{
        visual_studio_installation = (Split-Path -Leaf $vs)
        platform_toolset = [string]$Lock.visual_studio.platform_toolset
        msvc_toolset = $toolset
        windows_sdk = $sdk
        cl_path = "VC/Tools/MSVC/$toolset/bin/Hostx64/x64/cl.exe"
        cl_file_version = (Get-Item $cl).VersionInfo.FileVersion
        linker_path = "VC/Tools/MSVC/$toolset/bin/Hostx64/x64/link.exe"
        linker_file_version = (Get-Item $link).VersionInfo.FileVersion
        msbuild_path = "MSBuild/Current/Bin/MSBuild.exe"
        msbuild_file_version = (Get-Item $msbuild).VersionInfo.FileVersion
    }
    flags = [ordered]@{
        configuration = $Configuration
        platform = $Platform
        language_standard = "stdcpp17"
        warning_level = "Level3"
        conformance_mode = "false"
        whole_program_optimization = ($Configuration -eq "Release")
    }
    dependencies = [ordered]@{
        submodules = @{ "Submodule/imgui" = @{ commit = $imguiCommit; expected_commit = [string]$Lock.submodules.'Submodule/imgui'.commit } }
        nuget_packages = $Lock.nuget_packages
        restored_package_directories = $packages
    }
    outputs = [ordered]@{
        executable = Rel $exe
        executable_sha256 = Get-Sha256 $exe
        dll_sha256 = $dlls
        compiled_shader_bytecode_sha256 = $shaders
        compiled_shader_bytecode_set_sha256 = $shaderSetHash
    }
}

$manifestPath = Join-Path $Artifacts "build_manifest.json"
$manifest | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 $manifestPath
Write-Host "build_manifest=$((Rel $manifestPath))"
Write-Host "build_status=PASS"

param(
    [switch]$SkipNuGetRestore
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$BuildRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $BuildRoot
$RepoRoot = Split-Path -Parent $ProjectRoot
$LockPath = Join-Path $BuildRoot "dependency-lock.json"
$Lock = Get-Content $LockPath -Raw | ConvertFrom-Json

function Fail($Message) {
    throw "[bootstrap] $Message"
}

function Get-VsWhere {
    $path = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $path)) { Fail "vswhere.exe not found. Install Visual Studio 2022 with Desktop development with C++." }
    return $path
}

function Get-Vs2022Path {
    $vswhere = Get-VsWhere
    $path = & $vswhere -version "[17.0,18.0)" -latest -products * -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath
    if (-not $path) { Fail "Visual Studio 2022 with workload Microsoft.VisualStudio.Workload.NativeDesktop is required." }
    return $path.Trim()
}

function Ensure-Toolchain {
    $vs = Get-Vs2022Path
    $toolset = [string]$Lock.visual_studio.msvc_toolset
    $sdk = [string]$Lock.visual_studio.windows_sdk
    $toolsetDir = Join-Path $vs "VC\Tools\MSVC\$toolset"
    if (-not (Test-Path $toolsetDir)) { Fail "MSVC v143 toolset $toolset not found under $toolsetDir." }
    $sdkDir = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include\$sdk"
    if (-not (Test-Path $sdkDir)) { Fail "Windows SDK $sdk not found under $sdkDir." }
    return $vs
}

function Ensure-NuGet {
    $nuget = Get-Command nuget.exe -ErrorAction SilentlyContinue
    if ($nuget) { return $nuget.Source }
    $tools = Join-Path $BuildRoot ".tools"
    New-Item -ItemType Directory -Force -Path $tools | Out-Null
    $nugetPath = Join-Path $tools "nuget.exe"
    if (-not (Test-Path $nugetPath)) {
        Invoke-WebRequest -Uri "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe" -OutFile $nugetPath
    }
    return $nugetPath
}

function Ensure-Imgui {
    $entry = $Lock.submodules.'Submodule/imgui'
    $path = Join-Path $RepoRoot "Submodule\imgui"
    $commit = [string]$entry.commit
    $url = [string]$entry.url
    if (Test-Path (Join-Path $RepoRoot ".git")) {
        git -C $RepoRoot submodule update --init --recursive -- Submodule/imgui
    } elseif (-not (Test-Path (Join-Path $path ".git"))) {
        if (Test-Path $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        git clone $url $path
    }
    git -C $path fetch --tags origin $commit
    git -C $path checkout --detach $commit
    $actual = (git -C $path rev-parse HEAD).Trim()
    if ($actual -ne $commit) { Fail "ImGui commit mismatch: expected $commit, got $actual." }
}

function Restore-NuGet {
    if ($SkipNuGetRestore) { return }
    $nuget = Ensure-NuGet
    $vs = Get-Vs2022Path
    $msbuildPath = Join-Path $vs "MSBuild\Current\Bin"
    & $nuget restore (Join-Path $BuildRoot "MGPU-VoxelWaterfall.sln") -PackagesDirectory (Join-Path $RepoRoot "packages") -MSBuildPath $msbuildPath -NonInteractive
    if ($LASTEXITCODE -ne 0) { Fail "NuGet restore failed." }
}

Ensure-Toolchain | Out-Null
Ensure-Imgui
Restore-NuGet

Write-Host "bootstrap_status=PASS"

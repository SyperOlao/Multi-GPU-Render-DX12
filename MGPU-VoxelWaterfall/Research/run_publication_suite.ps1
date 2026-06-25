param(
    [int]$Sessions = 1,
    [int]$SmokeRepetitions = 1,
    [int]$FullRepetitions = 3,
    [uint32]$RandomizationSeedBase = 1000,
    [uint32]$StaticWorkloadSeedBase = 2000,
    [uint32]$DynamicWorkloadSeedBase = 3000,
    [string]$BaseOutputDirectory = "Research\artifacts\publication",
    [ValidateSet("Release")]
    [string]$Configuration = "Release",
    [ValidateSet("x64")]
    [string]$Platform = "x64",
    [string]$Python = "python"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptRoot
$RepoRoot = Split-Path -Parent $ProjectRoot
$Timestamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
$RunToken = "publication_${Timestamp}_pid$PID"
$OutputRoot = Join-Path (Join-Path $ProjectRoot $BaseOutputDirectory) $RunToken
$Logs = Join-Path $OutputRoot "logs"
$CommandsLog = Join-Path $OutputRoot "commands.jsonl"
$StatusPath = Join-Path $OutputRoot "publication_status.json"
$BuildManifestSource = Join-Path $ProjectRoot "InternalBuild\artifacts\build\build_manifest.json"
$Exe = Join-Path $RepoRoot "x64\$Configuration\MGPU-VoxelWaterfall.exe"

function Fail($Message) { throw "[publication] $Message" }

function Rel($Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $base = [System.IO.Path]::GetFullPath($ProjectRoot + [System.IO.Path]::DirectorySeparatorChar)
    if ($full.StartsWith($base, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($base.Length).Replace('\', '/')
    }
    return $full
}

function Write-JsonFile($Path, $Object) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $Object | ConvertTo-Json -Depth 20 | Set-Content -Encoding UTF8 $Path
}

function Append-JsonLine($Path, $Object) {
    $line = ($Object | ConvertTo-Json -Depth 20 -Compress)
    Add-Content -Encoding UTF8 -Path $Path -Value $line
}

function Write-Status($Status, $Reason, $FailedStep, $ExitCode) {
    $payload = [ordered]@{
        schema = "mgpu_voxel_publication_suite_status.v1"
        run_id = $RunToken
        status = $Status
        reason = $Reason
        failed_step = $FailedStep
        exit_code = $ExitCode
        output_directory = (Rel $OutputRoot)
        created_utc = $Timestamp
        updated_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    }
    Write-JsonFile $StatusPath $payload
}

function Invoke-LoggedCommand($Step, [string]$FileName, [string[]]$Arguments, $WorkingDirectory, $PriorityClass = $null) {
    New-Item -ItemType Directory -Force -Path $Logs | Out-Null
    $safeStep = $Step -replace '[^A-Za-z0-9_.-]', '_'
    $stdout = Join-Path $Logs "$safeStep.stdout.log"
    $stderr = Join-Path $Logs "$safeStep.stderr.log"
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $FileName
    foreach ($arg in $Arguments) { [void]$psi.ArgumentList.Add($arg) }
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $psi
    [void]$process.Start()
    if ($PriorityClass) {
        try { $process.PriorityClass = [Enum]::Parse([System.Diagnostics.ProcessPriorityClass], $PriorityClass) } catch {}
    }
    $outText = $process.StandardOutput.ReadToEnd()
    $errText = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    Set-Content -Encoding UTF8 -Path $stdout -Value $outText
    Set-Content -Encoding UTF8 -Path $stderr -Value $errText
    $record = [ordered]@{
        step = $Step
        command = @($FileName) + $Arguments
        working_directory = (Rel $WorkingDirectory)
        stdout = (Rel $stdout)
        stderr = (Rel $stderr)
        exit_code = $process.ExitCode
        started_utc = $process.StartTime.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        ended_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    }
    Append-JsonLine $CommandsLog $record
    return $process.ExitCode
}

function Stop-OnFailure($Step, $ExitCode, $BlockedCode = 3) {
    if ($ExitCode -eq 0) { return }
    $status = if ($ExitCode -eq $BlockedCode) { "BLOCKED" } else { "INVALID" }
    Write-Status $status "$Step failed with exit code $ExitCode" $Step $ExitCode
    Write-Checksums
    exit $ExitCode
}

function Get-OptionalCommand($Name) {
    try { return (Get-Command $Name -ErrorAction Stop).Source } catch { return $null }
}

function Get-EnvironmentControls($SessionId, $Phase, $RandomizationSeed, $StaticSeed, $DynamicSeed) {
    $unknown = @()
    $critical = @()
    $powerPlan = "UNKNOWN"
    $powerReason = ""
    try {
        $powerPlan = ((powercfg /getactivescheme) -join " ").Trim()
        if ($powerPlan -notmatch "High performance|Ultimate Performance") {
            $critical += "power plan is not High performance or Ultimate Performance"
        }
    } catch {
        $powerReason = "powercfg unavailable: $($_.Exception.Message)"
        $unknown += "power_plan"
    }

    $hags = "UNKNOWN"
    $hagsReason = ""
    try {
        $graphics = Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" -Name HwSchMode -ErrorAction Stop
        $hags = [string]$graphics.HwSchMode
    } catch {
        $hagsReason = "HwSchMode registry value unavailable"
        $unknown += "hags"
    }

    $display = @()
    try {
        $display = @(Get-CimInstance Win32_VideoController | ForEach-Object {
            [ordered]@{
                name = $_.Name
                pnp_device_id = $_.PNPDeviceID
                current_horizontal_resolution = $_.CurrentHorizontalResolution
                current_vertical_resolution = $_.CurrentVerticalResolution
                current_refresh_rate = $_.CurrentRefreshRate
                driver_version = $_.DriverVersion
            }
        })
    } catch {
        $unknown += "display_topology"
    }

    $thermal = [ordered]@{
        status = "UNKNOWN"
        reason = "temperature, clock, and power telemetry API not available"
    }
    $nvidiaSmi = Get-OptionalCommand "nvidia-smi"
    if ($nvidiaSmi) {
        try {
            $query = & $nvidiaSmi --query-gpu=name,temperature.gpu,clocks.gr,power.draw --format=csv,noheader,nounits
            $thermal = [ordered]@{ status = "RECORDED"; source = "nvidia-smi"; samples = @($query) }
        } catch {
            $thermal = [ordered]@{ status = "UNKNOWN"; reason = "nvidia-smi query failed: $($_.Exception.Message)" }
        }
    }

    $process = Get-Process -Id $PID
    $controls = [ordered]@{
        schema = "mgpu_voxel_environment_controls.v1"
        session_id = $SessionId
        phase = $Phase
        recorded_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        randomization_seed = $RandomizationSeed
        static_workload_seed = $StaticSeed
        dynamic_workload_seed = $DynamicSeed
        seed_note = "The app CLI currently accepts randomization seed; static/dynamic workload seed separation is recorded for provenance and must match benchmark artifacts before publication claims."
        power_plan = [ordered]@{ value = $powerPlan; unknown_reason = $powerReason }
        process_priority = [string]$process.PriorityClass
        process_affinity = [string]$process.ProcessorAffinity
        vsync_tearing = [ordered]@{ status = "UNKNOWN"; reason = "not exposed by current benchmark CLI; benchmark artifacts must carry render provenance" }
        debug_gpu_validation = [ordered]@{ status = "UNKNOWN"; reason = "not exposed by current benchmark CLI; build/provenance check must reject debug validation if captured" }
        hags = [ordered]@{ value = $hags; unknown_reason = $hagsReason }
        display_topology = $display
        background_load_policy = [ordered]@{ status = "MARKED"; reason = "operator-controlled policy; no OS-wide deterministic enforcement is available in this script" }
        thermal_clock_power = $thermal
        unknown_controls = $unknown
        critical_uncontrolled_confounders = $critical
        session_control_status = if ($critical.Count -gt 0) { "MARKED_UNCONTROLLED" } else { "RECORDED" }
    }
    return $controls
}

function Write-EnvironmentControls($Path, $SessionId, $Phase, $RandomizationSeed, $StaticSeed, $DynamicSeed) {
    $controls = Get-EnvironmentControls $SessionId $Phase $RandomizationSeed $StaticSeed $DynamicSeed
    Write-JsonFile $Path $controls
    return $controls
}

function Assert-BuildManifest($Path) {
    if (-not (Test-Path $Path)) { Fail "build_manifest.json missing after build" }
    $manifest = Get-Content $Path -Raw | ConvertFrom-Json
    if (-not $manifest.outputs.executable_sha256) { Fail "build manifest missing executable_sha256" }
    if (-not $manifest.outputs.compiled_shader_bytecode_set_sha256) { Fail "build manifest missing compiled shader bytecode set SHA-256" }
}

function Write-Checksums {
    if (-not (Test-Path $OutputRoot)) { return }
    $checksumPath = Join-Path $OutputRoot "checksums.sha256"
    $zipName = "publication_artifacts.zip"
    $rows = Get-ChildItem -Path $OutputRoot -Recurse -File |
        Where-Object { $_.Name -ne "checksums.sha256" -and $_.Name -ne $zipName } |
        Sort-Object FullName |
        ForEach-Object {
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
            "$hash  $(Rel $_.FullName)"
        }
    Set-Content -Encoding ASCII -Path $checksumPath -Value $rows
}

function Assert-ManifestCompleteness($SessionRoots, $ReportDir) {
    $required = @(
        (Join-Path $OutputRoot "build_manifest.json"),
        (Join-Path $ReportDir "RESULTS.md"),
        (Join-Path $ReportDir "RESULTS.html"),
        (Join-Path $OutputRoot "checksums.sha256"),
        $CommandsLog
    )
    foreach ($session in $SessionRoots) {
        $required += (Join-Path $session "environment_controls.smoke.json")
        $required += (Join-Path $session "environment_controls.full.json")
        $required += (Join-Path $session "smoke\manifest.json")
        $required += (Join-Path $session "smoke\raw_frames.csv")
        $required += (Join-Path $session "smoke\analysis_summary.v2.json")
        $required += (Join-Path $session "smoke\analysis_summary.v2.sha256")
        $required += (Join-Path $session "full\manifest.json")
        $required += (Join-Path $session "full\raw_frames.csv")
        $required += (Join-Path $session "full\analysis_summary.v2.json")
        $required += (Join-Path $session "full\analysis_summary.v2.sha256")
    }
    $missing = @($required | Where-Object { -not (Test-Path $_) })
    if ($missing.Count -gt 0) {
        Fail "publication manifest incomplete: $($missing -join '; ')"
    }
}

trap {
    try {
        Write-Status "INVALID" "$($_.Exception.Message)" "script_exception" 2
        Write-Checksums
    } catch {}
    exit 2
}

if ($Sessions -lt 1) { Fail "Sessions must be >= 1" }
$parent = Split-Path -Parent $OutputRoot
New-Item -ItemType Directory -Force -Path $parent | Out-Null
if (Test-Path $OutputRoot) {
    $existing = @(Get-ChildItem -Force -Path $OutputRoot)
    if ($existing.Count -gt 0) { Fail "output directory is not empty: $OutputRoot" }
}
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
New-Item -ItemType Directory -Force -Path $Logs | Out-Null

Write-Status "RUNNING" "publication suite started" "" 0

$buildExit = Invoke-LoggedCommand "build_provenance_check" "powershell" @(
    "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $ProjectRoot "InternalBuild\build.ps1"),
    "-Configuration", $Configuration, "-Platform", $Platform
) $RepoRoot
Stop-OnFailure "build_provenance_check" $buildExit
Assert-BuildManifest $BuildManifestSource
Copy-Item -LiteralPath $BuildManifestSource -Destination (Join-Path $OutputRoot "build_manifest.json") -Force
if (-not (Test-Path $Exe)) { Fail "benchmark executable missing: $Exe" }

$sessionRoots = @()
for ($i = 0; $i -lt $Sessions; ++$i) {
    $sessionId = "session_{0:D3}" -f $i
    $sessionRoot = Join-Path $OutputRoot $sessionId
    $sessionRoots += $sessionRoot
    New-Item -ItemType Directory -Force -Path $sessionRoot | Out-Null

    $randomizationSeed = [uint32]($RandomizationSeedBase + $i)
    $staticSeed = [uint32]($StaticWorkloadSeedBase + $i)
    $dynamicSeed = [uint32]($DynamicWorkloadSeedBase + $i)
    Write-JsonFile (Join-Path $sessionRoot "session_identity.json") ([ordered]@{
        schema = "mgpu_voxel_publication_session.v1"
        session_id = $sessionId
        randomization_seed = $randomizationSeed
        static_workload_seed = $staticSeed
        dynamic_workload_seed = $dynamicSeed
        smoke_repetitions = $SmokeRepetitions
        full_repetitions = $FullRepetitions
    })

    $twoDir = Join-Path $sessionRoot "two_adapter_verification"
    $twoExit = Invoke-LoggedCommand "$sessionId.two_adapter_verification" $Exe @(
        "--verify-two-adapter", "--two-adapter-output-dir=$twoDir"
    ) $RepoRoot "High"
    Stop-OnFailure "$sessionId.two_adapter_verification" $twoExit

    $validationDir = Join-Path $sessionRoot "visual_validation"
    $validationExit = Invoke-LoggedCommand "$sessionId.visual_validation_exact_matrix" $Exe @(
        "--run-validation-once", "--validation-output-dir=$validationDir"
    ) $RepoRoot "High"
    Stop-OnFailure "$sessionId.visual_validation_exact_matrix" $validationExit

    Write-EnvironmentControls (Join-Path $sessionRoot "environment_controls.smoke.json") $sessionId "Smoke" $randomizationSeed $staticSeed $dynamicSeed | Out-Null
    $smokeDir = Join-Path $sessionRoot "smoke"
    $smokeExit = Invoke-LoggedCommand "$sessionId.smoke_measurement" $Exe @(
        "--benchmark-smoke", "--benchmark-output-dir=$smokeDir",
        "--benchmark-seed=$randomizationSeed", "--benchmark-repetitions=$SmokeRepetitions"
    ) $RepoRoot "High"
    Stop-OnFailure "$sessionId.smoke_measurement" $smokeExit

    Copy-Item -LiteralPath (Join-Path $sessionRoot "environment_controls.smoke.json") -Destination (Join-Path $smokeDir "environment_controls.publication.json") -Force

    $smokeAnalysisExit = Invoke-LoggedCommand "$sessionId.smoke_hostile_analysis" $Python @(
        (Join-Path $ProjectRoot "Tools\analyze_benchmark.py"), "--input", $smokeDir
    ) $RepoRoot
    Stop-OnFailure "$sessionId.smoke_hostile_analysis" $smokeAnalysisExit

    Write-EnvironmentControls (Join-Path $sessionRoot "environment_controls.full.json") $sessionId "Full" $randomizationSeed $staticSeed $dynamicSeed | Out-Null
    $fullDir = Join-Path $sessionRoot "full"
    $fullExit = Invoke-LoggedCommand "$sessionId.full_measurement" $Exe @(
        "--benchmark-full", "--benchmark-output-dir=$fullDir",
        "--benchmark-seed=$randomizationSeed", "--benchmark-repetitions=$FullRepetitions"
    ) $RepoRoot "High"
    Stop-OnFailure "$sessionId.full_measurement" $fullExit

    Copy-Item -LiteralPath (Join-Path $sessionRoot "environment_controls.full.json") -Destination (Join-Path $fullDir "environment_controls.publication.json") -Force

    $fullAnalysisExit = Invoke-LoggedCommand "$sessionId.full_hostile_analysis" $Python @(
        (Join-Path $ProjectRoot "Tools\analyze_benchmark.py"), "--input", $fullDir
    ) $RepoRoot
    Stop-OnFailure "$sessionId.full_hostile_analysis" $fullAnalysisExit
}

$reportDir = Join-Path $OutputRoot "report"
$reportArgs = @((Join-Path $ProjectRoot "Tools\generate_publication_report.py"))
foreach ($sessionRoot in $sessionRoots) {
    $reportArgs += @("--smoke", (Join-Path $sessionRoot "smoke"))
}
foreach ($sessionRoot in $sessionRoots) {
    $reportArgs += @("--full", (Join-Path $sessionRoot "full"))
}
$reportArgs += @("--output", $reportDir, "--strict")
$reportExit = Invoke-LoggedCommand "publication_report" $Python $reportArgs $RepoRoot
Stop-OnFailure "publication_report" $reportExit

Write-Checksums
Assert-ManifestCompleteness $sessionRoots $reportDir
Write-Status "COMPLETE" "publication suite complete" "" 0
Write-Checksums

$zipPath = Join-Path $OutputRoot "publication_artifacts.zip"
$zipTemp = Join-Path (Split-Path -Parent $OutputRoot) "$RunToken.zip"
if (Test-Path $zipTemp) { Remove-Item -LiteralPath $zipTemp -Force }
if (Test-Path $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
$zipSources = @(Get-ChildItem -LiteralPath $OutputRoot -Force | Where-Object { $_.Name -ne "publication_artifacts.zip" } | ForEach-Object { $_.FullName })
Compress-Archive -Path $zipSources -DestinationPath $zipTemp -Force
Move-Item -LiteralPath $zipTemp -Destination $zipPath -Force

Write-Host "publication_status=COMPLETE output=$(Rel $OutputRoot)"
Write-Host "publication_zip=$(Rel $zipPath)"
exit 0

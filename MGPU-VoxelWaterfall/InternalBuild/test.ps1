param(
    [string]$Python = "python",
    [string]$OutputDirectory = "artifacts\test"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$BuildRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $BuildRoot
$Out = Join-Path $BuildRoot $OutputDirectory
New-Item -ItemType Directory -Force -Path $Out | Out-Null

function Run-Test($Name, [scriptblock]$Command) {
    Write-Host "test=$Name status=RUNNING"
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "test failed: $Name" }
    Write-Host "test=$Name status=PASS"
}

Run-Test "analyzer_adversarial" { & $Python (Join-Path $ProjectRoot "Tools\analyze_benchmark.py") --self-test }
Run-Test "benchmark_csv_aggregate" { & $Python (Join-Path $ProjectRoot "Tools\benchmark_csv_aggregate_tests.py") }
Run-Test "visual_validation_protocol" { & $Python (Join-Path $ProjectRoot "Tools\visual_validation_protocol_tests.py") }
Run-Test "two_adapter_mock_telemetry" { & $Python (Join-Path $ProjectRoot "Tools\two_adapter_verification_tests.py") }
Run-Test "publication_report_fail_closed" { & $Python (Join-Path $ProjectRoot "Tools\publication_report_tests.py") }
Run-Test "docs_consistency" { & $Python (Join-Path $ProjectRoot "Tools\docs_consistency_tests.py") }

$hardware = [ordered]@{
    schema = "mgpu_voxel_hardware_gpu_ci_status.v1"
    status = "SKIPPED"
    reason = "Hardware GPU validation and two-adapter runtime verification require a configured local two-adapter D3D12 machine; CI does not claim PASS."
}
$hardware | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $Out "hardware_gpu_tests.json")
Write-Host "hardware_gpu_tests=SKIPPED reason=NOT_AVAILABLE"
Write-Host "test_status=PASS"

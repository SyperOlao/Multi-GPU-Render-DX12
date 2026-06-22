# Reproduction

## Preconditions

Comparative benchmark suites require:

- deterministic visual validation `PASS`;
- explicit two-hardware-adapter runtime verification `PASS`;
- matching build hash, shader hash, adapter pair, validation configuration, and camera hash.

If the machine lacks two compatible hardware D3D12 adapters with distinct LUID values, comparative benchmark status is `BLOCKED`.

## Commands

Run visual validation:

```text
MGPU-VoxelWaterfall.exe --run-validation-once
```

Run explicit two-adapter verification:

```text
MGPU-VoxelWaterfall.exe --verify-two-adapter
```

Run Smoke:

```text
MGPU-VoxelWaterfall.exe --benchmark-smoke --benchmark-output-dir=<artifact-dir> --benchmark-seed=<uint>
```

Run Full:

```text
MGPU-VoxelWaterfall.exe --benchmark-full --benchmark-output-dir=<artifact-dir> --benchmark-seed=<uint>
```

Full is an overnight run and should only be started after PASS Smoke, PASS visual validation, and PASS two-GPU verification on the target hardware.

Run offline analysis:

```text
python MGPU-VoxelWaterfall\Tools\analyze_benchmark.py --input <artifact-dir>
```

Run analysis self-tests:

```text
python MGPU-VoxelWaterfall\Tools\analyze_benchmark.py --self-test
```

## Expected Artifacts

- `manifest.json`
- `environment.json`
- `two_adapter_preflight.json`
- `voxel_visual_validation.json`
- `voxel_visual_validation.csv`
- `raw_frames.csv`
- `runs.csv`
- `paired_runs.csv`
- `paired_summary.csv`
- `invalid_records.csv`
- `telemetry.csv`
- `memory_timeline.csv`
- `README.txt`

Numeric benchmark results must come from these artifacts. Missing hardware or stale validation must be reported as `BLOCKED` or `NOT_MEASURED`, not filled with synthetic numbers.


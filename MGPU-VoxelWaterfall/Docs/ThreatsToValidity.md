# Threats To Validity

## Internal Validity

- Thermal drift, GPU boost behavior, power plan changes, HAGS state, display topology, and background processes can change timings between paired executions.
- Driver scheduling and WDDM behavior are adapter-pair specific.
- Invalid queue clock calibration invalidates dependent cross-queue critical-path metrics.
- Visual validation must be tied to the same build hash, shader hash, adapter pair, config hash, and camera hash as the benchmark. Stale validation is rejected.
- The benchmark compares explicit multi-adapter rendering paths. Requested Multi falling back to Single is invalid for comparative analysis.

## External Validity

- Results apply to this synthetic voxel waterfall workload and tested hardware pair.
- The workload is not a physically correct fluid simulation.
- Results should not be generalized to unrelated renderers, mesh-shader backends, or heterogeneous GPU pairs without separate ablation and throughput calibration.

## Construct Validity

- CPU frame time and GPU critical-path time measure different effects and are stored separately.
- P95/P99 frame latency is descriptive within a run. It is not a tail estimate over independent experimental units when only a few repetitions are used.
- `TwoDeviceNominalEfficiency` is not a heterogeneous capacity-normalized efficiency.


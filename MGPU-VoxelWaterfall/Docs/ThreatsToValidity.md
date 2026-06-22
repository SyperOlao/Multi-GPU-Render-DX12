# Threats To Validity

## Internal Validity

- Thermal drift, GPU boost behavior, power plan changes, HAGS state, display topology, and background load can change timings between paired executions. Publication sessions record these controls; unknown values require reasons.
- Driver scheduling and WDDM behavior are adapter-pair specific. Driver identity is tied to adapter LUID/PnP identity in provenance.
- Invalid queue timestamp calibration invalidates cross-queue GPU timing and critical-path metrics.
- Visual validation must match the same executable SHA-256, shader bytecode-set SHA-256, adapter pair, driver identity, validation protocol/config/camera hashes, resolution/formats/sample count, workload seeds/counts, temporal/LOD/partition/chunk settings, and runtime/toolchain identity as the benchmark.
- Requested Multi falling back to Single is `INVALID` for comparative analysis. It is not a slow or fast Multi result.

## Construct Validity

- The primary endpoint is end-to-end `present_to_present_ms`. `cpu_submission_ms` is a narrower submission endpoint and is not full CPU frame time.
- `COMPLETE` and `PASS` have different meanings. `COMPLETE` is a producer state; `PASS` is evidence/analysis acceptance.
- Implementation equivalence and approximation fidelity answer different questions. H2/H3 require approximation fidelity against the Full + LOD-off reference, not only Single/Multi equality under the same approximation.
- P95/P99 frame latency is descriptive within a run. It is not an independent-unit tail estimate when repetition count is small.
- Static budget labels are not total voxel targets. Dynamic voxels are added separately and actual totals must be read from artifact counts.

## External Validity

- Results apply to this deterministic synthetic voxel waterfall workload, tested adapters, drivers, resolution, formats, and policy matrix.
- The workload is not a physically correct fluid simulation.
- Results should not be generalized to unrelated renderers, mesh-shader backends, integrated/discrete pairings, or heterogeneous GPU pairs without separate validation and throughput calibration.

## Statistical Validity

- The preliminary Full profile uses small `n`; it is not sufficient for publication claims unless the preregistered precision rule is met.
- Per-configuration exploration across the large matrix requires multiplicity control and must be labeled exploratory.
- No support decision is allowed when the confidence interval crosses the null, when `n < 2`, or when validation/preflight/hostile analysis is incomplete.

# MGPU Voxel Waterfall Research Methodology

## Research Questions

- Does explicit two-hardware-adapter rendering reduce run-level CPU frame time and calibrated GPU critical-path time for the deterministic synthetic voxel waterfall workload?
- How do temporal decimation and spatial density LOD change the paired Single/Multi result?
- When does cross-adapter color/depth transfer dominate the secondary adapter contribution?

## Hypotheses

- H1: Validated `MultiGpuFull` can reduce run-mean CPU frame time versus the matched `SingleGpuFull` block when the secondary partition is non-empty and render-output transfer is active.
- H2: Temporal decimation changes secondary simulation cadence without changing the declared benchmark logical work for a matched pair.
- H3: Spatial density LOD reduces submitted voxel counts while preserving deterministic validation metrics within tolerance.

## Workload

The workload is a deterministic synthetic voxel waterfall scene containing static canyon/basin geometry and dynamic voxel particles. It is a graphics and scheduling workload. It is not a physically correct fluid simulation.

## Experimental Unit

The experimental unit for inference is one independent benchmark run/repetition. Individual frames inside a run are descriptive samples used to compute per-run mean, median, P95, and P99 latency. They are not treated as independent replicates for confidence intervals.

## Pairing

Each comparative block has one Single execution and one matching Multi execution with the same:

- seed and reset state;
- resolved static/dynamic/total voxel counts;
- camera, resolution, and presentation mode;
- temporal policy and interval;
- spatial LOD policy;
- secondary share key.

The runner records `session_id`, `block_id`, `pair_id`, `repetition`, block order, and pair member order. Paired differences and log-speedups are computed only within matching block/repetition values.

## Randomization

The benchmark uses a randomized complete block design. For each repetition, pair blocks are shuffled with the logged deterministic seed. Within each block, Single-first versus Multi-first order is selected by the same deterministic random stream. Matching executions therefore remain close in time while still controlling order bias.

## Primary Endpoint

The primary endpoint is paired run-level mean CPU frame time.

## Secondary Metrics

Secondary metrics include calibrated critical-path GPU time, GPU work sum, compute/graphics/copy/composite times, transfer bytes, voxel counts, LOD counts, scheduler dropped steps, hash probe statistics, memory counters, validation metrics, and invalid-frame counts.

## Statistical Method

For each valid paired block, the analysis computes:

- paired difference: `single_run_mean_ms - multi_run_mean_ms`;
- log speedup: `log(single_run_mean_ms / multi_run_mean_ms)`.

Aggregate speedup is `exp(mean(log_speedup))`. Confidence intervals use a two-sided Student-t interval over run-level paired values. With three repetitions this uses `df=2`, not a normal 1.96 approximation.

`TwoDeviceNominalEfficiency = observed_speedup / 2` is reported only as a nominal metric. For heterogeneous adapters, normalized efficiency must use standalone adapter throughput calibration:

```text
heterogeneous_ideal_speedup = 1 + secondary_throughput / primary_throughput
normalized_heterogeneous_efficiency = observed_speedup / heterogeneous_ideal_speedup
```

If calibration is unavailable, heterogeneous normalized efficiency is `NOT_MEASURED`.

## Exclusion Rules

Rows are invalid when visual validation is missing or failed, two-GPU verification is missing for requested Multi, requested mode differs from actual mode, timestamp calibration is invalid, particle transfer bytes are nonzero, Multi render-output transfer bytes are zero, scheduler dropped steps occur in benchmark mode, or no matching valid paired block exists.

Outliers are not removed silently. Every exclusion is written to `invalid_records.csv`.


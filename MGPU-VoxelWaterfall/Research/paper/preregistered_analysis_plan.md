# Preregistered Analysis Plan

This plan must be finalized before the final Full experiment. Any later change must be labeled post hoc.

## Primary Contrast

Primary contrast: paired `SingleGpuFull` versus `MultiGpuFull` using run-level mean `present_to_present_ms`.

Primary endpoint boundary: `present_to_present_ms` is the interval between successful presents. It is the end-to-end rendering endpoint. `cpu_submission_ms` is secondary and covers only after frame-resource acquire through `EndFrameCpu`.

## Inclusion Requirements

A run contributes to confirmatory analysis only when:

- producer suite status is `COMPLETE`;
- hostile analysis status is `PASS`;
- exact visual validation matrix is `PASS` for matching protocol/config/camera hashes;
- two-adapter preflight is `PASS`;
- requested mode equals actual mode;
- no fallback occurred;
- timestamp calibration is valid;
- scheduler dropped steps are zero;
- requested/executed fixed steps match;
- raw frame counts match exact warmup/measured counts;
- Single/Multi pairs match on session, block, repetition, resolved config hash, seeds, camera, resolution/formats/sample count, counts, and logical work.

## Exclusions

Exclusions are predeclared:

- missing/stale validation or preflight evidence;
- nonzero particle transfer bytes;
- zero required render-output transfer leg bytes for Multi;
- same adapter LUID for two-adapter evidence;
- GPU1 compute or graphics evidence missing/zero in Multi verification;
- invalid queue/fence calibration;
- duplicate/missing config or execution manifest;
- any invalid raw frame in a producer `COMPLETE` suite.

No outlier removal is allowed outside these rules. Every exclusion must appear in `invalid_records.csv` or hostile analysis failure output.

## Minimum Paired n And Precision Rule

The preliminary profile may use `n=3` only for pilot estimates. The publication Full run must choose paired `n` before measurement using:

```text
python MGPU-VoxelWaterfall\Tools\plan_benchmark_power.py --pilot-sd <sd> --mde <difference-ms>
```

or:

```text
python MGPU-VoxelWaterfall\Tools\plan_benchmark_power.py --pilot-sd <sd> --ci-half-width <ms>
```

Minimum acceptable confirmatory precision: TODO before final Full run. Record pilot SD, MDE or CI half-width, selected `n`, and command output here before measurement.

## Multiplicity Policy

The primary H1 contrast is confirmatory. Per-config contrasts across workload labels, shares, LOD states, temporal states, GPU endpoints, and CPU endpoints are exploratory unless explicitly listed here before the run.

Exploratory families must use Holm or FDR correction and must be labeled exploratory in tables and text.

## Hypothesis Decision Rules

H1 `SUPPORT`:

- strict hostile analysis `PASS`;
- complete validation/preflight evidence;
- paired Student-t CI for `single - multi` on `present_to_present_ms` has lower bound greater than zero.

H2 `SUPPORT`:

- strict hostile analysis `PASS`;
- exact logical work equality and fixed-step equality;
- approximation-fidelity validation PASS;
- measured secondary compute work statistics exist;
- any directional claim must state the CI and cannot be SUPPORT if the CI crosses the null.

H3 `SUPPORT`:

- strict hostile analysis `PASS`;
- LOD-on submitted counts are monotonic/non-increasing relative to LOD-off;
- simulation counts are unchanged;
- approximation-fidelity validation PASS.

RQ3 transfer-dominated:

- strict hostile analysis `PASS`;
- CI for `transfer_ms / critical_path_gpu_ms` is measured;
- lower CI bound is at least `0.5`.

For `n < 2`, all CI-based decisions are `NOT_MEASURED`, not SUPPORT.

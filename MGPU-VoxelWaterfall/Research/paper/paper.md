# MGPU-VoxelWaterfall: Explicit Multi-Adapter Voxel Rendering

## Abstract

TODO: Fill after a strict PASS publication archive exists. Do not claim H1/H2/H3 support without PASS analysis summaries and confidence intervals.

## Introduction

TODO: Describe the explicit two-hardware-adapter rendering problem and the deterministic voxel waterfall workload.

## Research Questions/Hypotheses

- H1: Validated `MultiGpuFull` versus matched `SingleGpuFull` on paired run-level `present_to_present_ms`.
- H2: Temporal decimation changes secondary compute work while exact logical-work equality and approximation-fidelity validation pass.
- H3: Spatial Density LOD reduces submitted counts monotonically without changing simulation counts and with approximation-fidelity validation PASS.
- RQ3: Transfer/composite contribution relative to critical-path GPU time under the predeclared transfer-dominated criterion.

Support decisions are TODO until strict PASS data and CIs exist.

## Related Work

TODO: Add only verified sources to `references.bib`.

## Novelty/Contribution

TODO: State concrete contributions after implementation and validation artifacts are archived.

## Methods

TODO: Summarize deterministic workload, explicit adapter partitioning, validation, and hostile analysis.

## Hardware/Software Environment

TODO: Populate from publication `environment.json`, `environment_controls.*.json`, and `build_manifest.json`. Unknown fields must remain UNKNOWN with reason.

## Validation

TODO: Report implementation-equivalence and approximation-fidelity validation separately. Include exact protocol/config/camera hashes.

## Experimental Design

TODO: Describe sessions, randomization seed, static workload seed, dynamic workload seed, blocks, repetitions, and exact Smoke/Full matrices.

## Statistical Analysis Plan

See `preregistered_analysis_plan.md`. Do not revise the plan after inspecting final Full results except in a clearly marked post hoc section.

## Results

TODO: Generate from `run_publication_suite.ps1` output only. Use `NOT_MEASURED` or `INCONCLUSIVE` when evidence or CI is incomplete.

## Discussion

TODO: Interpret only strict PASS evidence. Separate confirmatory and exploratory findings.

## Threats/Limitations

TODO: Sync with `MGPU-VoxelWaterfall/Docs/ThreatsToValidity.md`.

## Conclusion

TODO: Fill after final analysis.

## Data/Code Availability

TODO: Provide archive path, commit hash, build manifest, checksums, and exact commands after publication suite completion.

## References

TODO: Generated from `references.bib` after sources are verified.

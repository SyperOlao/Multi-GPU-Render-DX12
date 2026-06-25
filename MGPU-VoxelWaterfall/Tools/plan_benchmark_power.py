#!/usr/bin/env python3
"""Pre-run paired-n planning for MGPU-VoxelWaterfall benchmarks."""

from __future__ import annotations

import argparse
import math
import sys


T975 = {
    1: 12.706204736,
    2: 4.302652730,
    3: 3.182446305,
    4: 2.776445105,
    5: 2.570581836,
    6: 2.446911851,
    7: 2.364624252,
    8: 2.306004135,
    9: 2.262157163,
    10: 2.228138852,
    20: 2.085963447,
    30: 2.042272456,
    60: 2.000297822,
}


def t975(df: int) -> float:
    if df in T975:
        return T975[df]
    if df < 20:
        return T975[10]
    if df < 30:
        return T975[20]
    if df < 60:
        return T975[30]
    return 1.959963985


def required_n_for_ci(sd: float, half_width: float, max_n: int) -> int:
    for n in range(2, max_n + 1):
        half = t975(n - 1) * sd / math.sqrt(n)
        if half <= half_width:
            return n
    raise RuntimeError(f"required n exceeds --max-n={max_n}")


def required_n_for_mde(sd: float, mde: float, power: float, max_n: int) -> int:
    # Normal approximation for pre-run planning; final inference still uses paired Student-t.
    z_alpha = 1.959963985
    z_power = {0.80: 0.841621234, 0.85: 1.036433389, 0.90: 1.281551566, 0.95: 1.644853627}
    zp = z_power.get(round(power, 2))
    if zp is None:
        raise RuntimeError("supported --power values: 0.80, 0.85, 0.90, 0.95")
    n = math.ceil(((z_alpha + zp) * sd / mde) ** 2)
    return max(2, min(max_n, n))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pilot-sd", type=float, required=True, help="paired difference SD from a pilot run")
    parser.add_argument("--mde", type=float, help="minimum detectable paired difference")
    parser.add_argument("--ci-half-width", type=float, help="desired 95%% CI half width")
    parser.add_argument("--power", type=float, default=0.80)
    parser.add_argument("--max-n", type=int, default=200)
    args = parser.parse_args()
    if args.pilot_sd <= 0.0:
        raise SystemExit("--pilot-sd must be positive")
    if not args.mde and not args.ci_half_width:
        raise SystemExit("provide --mde or --ci-half-width")
    if args.mde is not None and args.mde <= 0.0:
        raise SystemExit("--mde must be positive")
    if args.ci_half_width is not None and args.ci_half_width <= 0.0:
        raise SystemExit("--ci-half-width must be positive")

    result: dict[str, object] = {
        "pilot_sd": args.pilot_sd,
        "note": "Predeclare n before running the publication profile; final CIs use paired Student-t.",
    }
    if args.ci_half_width is not None:
        result["required_paired_n_for_ci_half_width"] = required_n_for_ci(
            args.pilot_sd, args.ci_half_width, args.max_n)
    if args.mde is not None:
        result["required_paired_n_for_mde"] = required_n_for_mde(
            args.pilot_sd, args.mde, args.power, args.max_n)
        result["power"] = args.power
    for key, value in result.items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"planning_status=FAIL reason={exc}", file=sys.stderr)
        raise SystemExit(2)

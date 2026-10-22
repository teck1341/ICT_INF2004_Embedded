#!/usr/bin/env python3
"""Calculate the current conservative spatial barcode sampling budget.

No configuration is changed and no hardware sufficiency is certified.
Use effective decoded counts per wheel revolution, not disc slots.
"""
import argparse
import json
import math


def positive(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError('must be finite and greater than zero')
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wheel-radius-mm', type=positive, required=True)
    parser.add_argument('--counts-per-wheel-rev', type=int, required=True)
    parser.add_argument('--speed-mps', type=positive, required=True)
    parser.add_argument('--worst-sample-gap-ms', type=positive, required=True)
    parser.add_argument('--minimum-run-mm', type=positive, required=True)
    args = parser.parse_args()
    if args.counts_per_wheel_rev < 1:
        parser.error('counts per wheel revolution must be a measured positive integer')
    quantum = 2.0 * math.pi * args.wheel_radius_mm / args.counts_per_wheel_rev
    travel = args.speed_mps * args.worst_sample_gap_ms
    step = quantum + travel
    allowed = args.minimum_run_mm / 5.0
    denominator = allowed - travel
    required_counts = (math.ceil(2.0 * math.pi * args.wheel_radius_mm / denominator)
                       if denominator > 0 else None)
    print(json.dumps({
        'inputs': vars(args),
        'wheel_quantum_mm': quantum,
        'travel_during_sample_gap_mm': travel,
        'conservative_step_bound_mm': step,
        'required_minimum_run_mm': 5.0 * step,
        'sampling_gate_passes': step <= allowed,
        'minimum_integer_counts_for_this_formula': required_counts,
        'interpretation': (
            'Arithmetic gate only. Confirm measured encoder resolution, optical spot, '
            'print scale, jitter, slip and codebook. A passed gate is not a decode '
            'or robot-readiness result. Null required counts means the sample-gap '
            'travel alone exceeds this five-step budget; revise sensing/speed design.'
        )
    }, indent=2))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Build public-step application tests with explicit host-only boundary doubles.

The shipped robot_config.h is never modified. The default copy retains every
inhibit. Temporary fixture profiles enable reviewed-input flags only to exercise
application behavior with synthetic sensors, servo endpoints and fake codebook.
The wiring_only profile enables ROBOT_WIRING_CONFIRMED alone (commissioning
stage 2). Timing scenarios model an offset accelerometer clock and legal
preemption points; they order host steps, they do not measure target timing.
--motion-phase/--imu-phase override every selected scenario's Motion (0-4)
and IMU (0-9) release phases for a phase sweep (T-53); the report records
them.
No SDK, GPIO registers, kernel scheduler, radio stack, or real hardware is tested.

Reports go to $ROBOT_VALIDATION_DIR, else the git-ignored
firmware/validation/local; never into the dated evidence folders.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone

from robot_paths import (FIRMWARE, PORTABLE_HEADERS, PORTABLE_SOURCES, REPO,
                         TARGET_HEADERS, app_file, include_flags)

FLAGS = [
    "-std=c99", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
    "-Wconversion", "-Wsign-conversion", "-Wshadow", "-Wstrict-prototypes",
    "-Wmissing-prototypes", "-Wformat=2", "-Wundef", "-Wcast-qual",
    "-Wwrite-strings", "-Wswitch-enum", "-Wnull-dereference",
]
FIXTURE_GATES = (
    "ROBOT_CALIBRATION_CONFIRMED", "ROBOT_SERVO_CONFIRMED",
    "ROBOT_BARCODE_CONFIRMED", "ROBOT_WIRING_CONFIRMED",
    "ROBOT_COURSE_CONFIRMED", "ROBOT_CORRIDOR_CONFIRMED",
    "ROBOT_IMU_MOUNT_CONFIRMED",
)
# Profiles that enable only some gates; every other fixture profile uses all.
PROFILE_GATES = {
    "wiring_only": ("ROBOT_WIRING_CONFIRMED",),
}
SCENARIOS = {
    "default": ("default", "uart_pins"),
    "fixture": (
        "local_control", "stop_epoch", "junction_wait", "left_stall",
        "right_stall", "line_timeout", "unknown_range", "scan_handoff",
        "scan_unknown_return", "full_bypass", "command_policy", "uart_pins",
        "pitch_fault", "roll_fault", "invalid_tilt_start", "transient_tilt",
        "fault_diagnostic", "fault_mask", "fault_stress", "encoder_wrap",
        "encoder_jumps", "boot_counts", "stop_race", "stop_snapshot",
        "telemetry_budget", "telemetry_wrap", "barcode_noise",
        "barcode_junction", "hump_history", "watchdog_owners",
        "transient_range", "range_obstacle", "range_stop", "sonar_silent",
        "stale_range_pose", "range_start", "barcode_gap",
        "accel_pending", "accel_stale", "accel_invalid",
        "accel_clock_fast", "accel_clock_slow", "preempt_hump",
        "preempt_hump_always", "preempt_steady", "preempt_steady_double",
        "preempt_vehicle", "collision_guard", "preempt_collision",
        "barcode_range_pause", "barcode_quiet_pause", "barcode_crossbar",
        "scan_side_silent", "motor_output_fault",
    ),
    "wiring_only": ("wiring_only",),
    "servo_asymmetric_forward": ("servo_endpoints", "full_bypass"),
    "servo_asymmetric_reverse": ("servo_endpoints", "full_bypass"),
}


def run(command: list[str], environment: dict[str, str]) -> dict:
    """Preserve the exact invocation, combined diagnostics, and exit status."""
    result = subprocess.run(command, env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            check=False)
    return {"command": command, "returncode": result.returncode,
            "output": result.stdout}


def prepare(destination: Path, profile: str) -> None:
    """Copy target code; change only labelled synthetic fixture settings."""
    destination.mkdir(parents=True, exist_ok=True)
    for source in TARGET_HEADERS:
        shutil.copy2(source, destination / source.name)
    for name in ("robot_app.c", "app_cfg.c"):
        shutil.copy2(app_file(name), destination / name)
    if profile != "default":
        config_path = destination / "robot_config.h"
        config = config_path.read_text()
        for gate in PROFILE_GATES.get(profile, FIXTURE_GATES):
            pattern = rf"(?m)^(#define\s+{gate}\s+)0\s*$"
            config, count = re.subn(pattern, r"\g<1>1", config)
            if count != 1:
                raise RuntimeError(f"Expected one unconfirmed gate: {gate}")
        if profile.startswith("servo_asymmetric_"):
            replacements = {
                "ROBOT_SERVO_MIN_US": "1100U",
                "ROBOT_SERVO_MAX_US": "2100U",
                "ROBOT_SERVO_ANGLE_SIGN":
                    "-1" if profile.endswith("reverse") else "1",
            }
            for name, value in replacements.items():
                config, count = re.subn(
                    rf"(?m)^#define {name} .+$",
                    f"#define {name} {value}", config)
                if count != 1:
                    raise RuntimeError(f"Expected one servo setting: {name}")
        config_path.write_text(
            "/* HOST FIXTURE ONLY: synthetic input confirmations. */\n" + config)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-only", action="store_true")
    parser.add_argument("--scenario", help="Run one named scenario only")
    parser.add_argument("--motion-phase", type=int, choices=range(5),
                        help="Override the Motion release phase (0-4); "
                             "requires --imu-phase")
    parser.add_argument("--imu-phase", type=int, choices=range(10),
                        help="Override the IMU release phase (0-9); "
                             "requires --motion-phase")
    args = parser.parse_args()
    if (args.motion_phase is None) != (args.imu_phase is None):
        parser.error("--motion-phase and --imu-phase go together")
    phases = ([] if args.motion_phase is None else
              [str(args.motion_phase), str(args.imu_phase)])
    firmware = FIRMWARE
    build_root = firmware / "build-apphost"
    # ROBOT_VALIDATION_DIR redirects reports; the default folder is ignored.
    validation = Path(os.environ.get("ROBOT_VALIDATION_DIR",
                                     firmware / "validation" / "local"))
    validation.mkdir(parents=True, exist_ok=True)
    compiler = os.environ.get("CC", "gcc")
    original_config = app_file("robot_config.h")
    original_hash = hashlib.sha256(original_config.read_bytes()).hexdigest()
    environment = os.environ.copy()
    # LeakSanitizer is off by project choice until it has been tried on the
    # CI runner; ROBOT_LSAN=1 enables it for a local run.
    leak_checks = int(os.environ.get("ROBOT_LSAN") == "1")
    leak_note = ("LeakSanitizer enabled (ROBOT_LSAN=1)." if leak_checks else
                 "LeakSanitizer disabled by project choice pending a CI "
                 "trial; ROBOT_LSAN=1 enables it.")
    environment["ASAN_OPTIONS"] = f"detect_leaks={leak_checks}:halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    portable = [str(path) for path in PORTABLE_SOURCES]
    test_source = firmware / "tests/apphost_test_main.c"
    source_paths = sorted(set(
        list(PORTABLE_SOURCES) + list(PORTABLE_HEADERS) +
        list(TARGET_HEADERS) +
        [app_file("robot_app.c"), app_file("app_cfg.c"),
         test_source, firmware / "tests/apphost_test_main.h",
         Path(__file__).resolve()]))
    source_hashes = {
        str(path.relative_to(REPO)):
        hashlib.sha256(path.read_bytes()).hexdigest() for path in source_paths}
    records = []
    modes = ["release"] if args.release_only else ["release", "sanitized"]
    report = {
        "utc": datetime.now(timezone.utc).isoformat(),
        "scope": "Public robot_app steps; synthetic board/network/locks/time",
        "exclusions": ["ARM build", "GPIO", "RTOS scheduling", "lwIP/radio",
                       "sensor accuracy", "motor tuning", "physical safety"],
        "confirmation_override": "Temporary fixture copy only",
        "original_config_sha256": original_hash,
        "source_sha256": source_hashes,
        "synthetic_geometry": {
            "forward_object_distance_m": 0.20,
            "object_half_angle_degrees": 10,
            "open_side_echo_distance_m": 3.0,
            "silent_side_scenario": (
                "scan_side_silent: no echo beyond 20 degrees; no bypass"),
            "servo_beam_model": (
                "Beam angle inverts the configured servo endpoints and sign"),
            "line_input": "Fixed balanced optical signal; no course model",
            "motor_model": "Deterministic first-order lag and encoder counts",
            "clearance_policy": "Unmodified production bounds",
            "accelerometer_clock_model": (
                "STATUS_REG_A samples at 100.01 Hz, or 99.99 Hz with 0-0.8 ms "
                "read jitter, classified by the production board.h rule"),
            "preemption_model": (
                "A 1 ms tick of Safety/Motion/Line inside a blocked IMU I2C "
                "read (once, or every read; 6 ms, two Motion publishes, in "
                "preempt_steady_double) or VehicleTask button read; "
                "Motion/Line release 1 ms after IMU/Vehicle (preempt_hump_"
                "always: IMU phase 5, Motion phase 1); host ordering only"),
            "steady_plant_model": (
                "preempt_steady*: both wheels pinned at one encoder count "
                "per 5 ms Motion period, so no count quantisation"),
            "crossbar_model": (
                "barcode_crossbar: the side channel goes black 5 mm before "
                "both line channels, after a decoded action"),
            "barcode_pause_model": (
                "Range lost 6 mm into a bar, or after a valid 12 mm bar "
                "before the 15 mm trailing quiet zone closes the frame"),
        },
        "leak_sanitizer": leak_note,
        "phase_override": (None if not phases else
                           {"motion_phase": args.motion_phase,
                            "imu_phase": args.imu_phase}),
        "compiler": run([compiler, "--version"], environment),
        "checks": records,
    }
    failures = 0
    executions = 0
    for profile, available in SCENARIOS.items():
        scenarios = [name for name in available
                     if args.scenario is None or name == args.scenario]
        if not scenarios:
            continue
        destination = build_root / profile
        prepare(destination, profile)
        for mode in modes:
            executable = destination / f"apphost_{mode}"
            options = ["-O2", "-DNDEBUG"]
            if mode == "sanitized":
                options = ["-O1", "-g", "-fno-omit-frame-pointer",
                           "-fsanitize=address,undefined,float-cast-overflow",
                           "-fno-sanitize-recover=all"]
            command = [compiler, *FLAGS, *options,
                       f"-DAPPHOST_FIXTURE={int(profile != 'default')}",
                       f"-I{destination}", *include_flags(),
                       f"-I{firmware / 'tests'}", *portable,
                       str(destination / "robot_app.c"),
                       str(destination / "app_cfg.c"), str(test_source),
                       "-lm", "-o", str(executable)]
            compiled = run(command, environment)
            compiled.update(profile=profile, mode=mode, kind="compile")
            records.append(compiled)
            print(f"{profile}/{mode}: compile exit{compiled['returncode']}")
            if compiled["output"]:
                print(compiled["output"], end="")
            if compiled["returncode"]:
                failures += 1
                continue
            for scenario in scenarios:
                result = run([str(executable), scenario, *phases],
                             environment)
                result.update(profile=profile, mode=mode, scenario=scenario,
                              kind="scenario")
                records.append(result)
                executions += 1
                failures += result["returncode"] != 0
                print(result["output"], end="")
    final_hash = hashlib.sha256(original_config.read_bytes()).hexdigest()
    report["original_configuration_unchanged"] = final_hash == original_hash
    final_source_hashes = {
        str(path.relative_to(REPO)):
        hashlib.sha256(path.read_bytes()).hexdigest() for path in source_paths}
    report["source_files_unchanged"] = final_source_hashes == source_hashes
    if final_source_hashes != source_hashes:
        failures += 1
    report["scenario_executions"] = executions
    report["failures"] = failures
    if final_hash != original_hash:
        failures += 1
        report["failures"] = failures
    if not executions:
        failures += 1
        report["failures"] = failures
    (validation / "application-host-test-report.json").write_text(
        json.dumps(report, indent=2) + "\n")
    log = [f"Application host verification UTC: {report['utc']}",
           report["scope"],
           "Original hardware confirmation gates were not modified.",
           leak_note,
           report["compiler"]["output"]]
    for record in records:
        label = record.get("scenario", "compile")
        log.extend([f"{record['profile']}/{record['mode']}/{label}",
                    "Command: " + " ".join(record["command"]),
                    record["output"],
                    f"Exit: {record['returncode']}"])
    log.append(f"Scenario executions: {executions}; failures: {failures}")
    (validation / "application-host-test-log.txt").write_text("\n".join(log))
    print(f"Scenario executions: {executions}; failures: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

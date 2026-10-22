#!/usr/bin/env python3
"""Run actual board ISR/acquisition with SDK types and host MMIO/I2C doubles.

This regression exercises the packed encoder/echo ISR words, the lock
discipline of their task-side read-and-clear (PRIMASK double), the narrowed
motor command and accelerometer sample status. It is not an ARM build,
hardware test, IRQ latency test or GPIO electrical model. board.c is built
with the project's strict host warning set (-Wconversion proves bit-field
widths). The commissioned variant opens the wiring and calibration gates in
its temporary configuration copy only; shipped gates stay 0.

Logs go to --output, else $ROBOT_VALIDATION_DIR/board_irq, else the
git-ignored firmware/validation/local/board_irq; never into dated evidence.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile

from robot_paths import REPO, app_file, include_flags


SDK_INCLUDES = (
    "common/pico_base_headers", "common/pico_binary_info",
    "common/pico_time", "common/hardware_claim", "rp2040/pico_platform",
    "rp2040/hardware_regs", "rp2040/hardware_structs",
    "rp2_common/pico_platform_common", "rp2_common/pico_platform_compiler",
    "rp2_common/pico_platform_sections", "rp2_common/pico_platform_panic",
    "rp2_common/hardware_base", "rp2_common/hardware_gpio",
    "rp2_common/hardware_adc", "rp2_common/hardware_i2c",
    "rp2_common/hardware_pwm", "rp2_common/hardware_irq",
    "rp2_common/hardware_resets", "rp2_common/hardware_clocks",
    "rp2_common/hardware_timer", "rp2_common/hardware_sync",
    "rp2_common/hardware_sync_spin_lock",
)
# Same warning set as firmware/run_tests.sh (rule: warnings are errors).
STRICT_WARNINGS = (
    "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Wconversion",
    "-Wsign-conversion", "-Wshadow", "-Wstrict-prototypes",
    "-Wmissing-prototypes", "-Wformat=2", "-Wundef", "-Wcast-qual",
    "-Wwrite-strings", "-Wswitch-enum", "-Wnull-dereference",
)
# name, encoder channels, one-channel direction confirmed, gates opened
VARIANTS = (("quadrature", 2, 0, 0), ("single_unconfirmed", 1, 0, 0),
            ("single_confirmed", 1, 1, 0), ("single_commissioned", 1, 1, 1))


def configure(config: str, channels: int, confirmed: int,
              commissioned: int) -> str:
    """Rewrite the copied configuration; every edited line must exist."""
    for name, value in (("ROBOT_ENCODER_CHANNELS", channels),
                        ("ROBOT_SINGLE_CHANNEL_DIRECTION_CONFIRMED",
                         confirmed),
                        ("ROBOT_WIRING_CONFIRMED", commissioned),
                        ("ROBOT_CALIBRATION_CONFIRMED", commissioned)):
        shipped = "#define " + name + " "
        lines = [line for line in config.splitlines()
                 if line.startswith(shipped)]
        if len(lines) != 1:
            raise ValueError("Expected one shipped line for " + name)
        config = config.replace(lines[0], shipped + str(value))
    return config


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=REPO,
                        help="project-template checkout (default: this one)")
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path,
                        help="log folder (default: $ROBOT_VALIDATION_DIR/"
                             "board_irq, else firmware/validation/local/"
                             "board_irq)")
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    firmware = Path(__file__).resolve().parents[1]
    sdk = args.sdk.resolve()
    repository = args.repository.resolve()
    if args.output is None:
        args.output = Path(os.environ.get(
            "ROBOT_VALIDATION_DIR", firmware / "validation" / "local")) / "board_irq"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    version = (sdk / "pico_sdk_version.cmake").read_text()
    values = {}
    for key in ("MAJOR", "MINOR", "REVISION"):
        match = re.search(r"set\(PICO_SDK_VERSION_" + key + r" (\d+)\)",
                          version)
        if match is None:
            raise ValueError("Missing SDK version component " + key)
        values["PICO_SDK_VERSION_" + key] = match[1]
    values["PICO_SDK_VERSION_STRING"] = ".".join(values.values())
    if values["PICO_SDK_VERSION_STRING"] != "2.2.0":
        raise ValueError("This regression requires Pico SDK 2.2.0")
    # LeakSanitizer is off by project choice until it has been tried on the
    # CI runner; ROBOT_LSAN=1 enables it for a local run.
    leak_checks = int(os.environ.get("ROBOT_LSAN") == "1")
    report = {"scope": "host MMIO/I2C, actual board.c ISR/acquisition, SDK headers",
              "hardware_test": False, "arm_build": False, "checks": [],
              "leak_sanitizer": ("enabled (ROBOT_LSAN=1)" if leak_checks else
                                 "disabled by project choice pending a CI "
                                 "trial; ROBOT_LSAN=1 enables it"),
              "source_sha256": {}}
    for path in (app_file("board.c"), app_file("board.h"),
                 app_file("robot_config.h"),
                 firmware / "tests/boardirq_test_main.c",
                 firmware / "tests/boardirq_test_main.h",
                 firmware / "scripts/test_board_irq.py"):
        report["source_sha256"][str(path.relative_to(REPO))] = hashlib.sha256(
            path.read_bytes()).hexdigest()
    failed = False
    with tempfile.TemporaryDirectory(prefix="inf2004_board_irq_") as folder:
        temporary = Path(folder)
        generated = temporary / "pico"
        generated.mkdir()
        template = (sdk / "src/common/pico_base_headers/include/pico/"
                    "version.h.in").read_text()
        for key, value in values.items():
            template = template.replace("${" + key + "}", value)
        (generated / "version.h").write_text(template)
        command = [args.cc, "-std=gnu11", *STRICT_WARNINGS,
                   "-D__unused=__attribute__((__unused__))", "-DNDEBUG",
                   "-DPICO_RP2040=1", "-DPICO_32BIT=1", "-DPICO_ON_DEVICE=1",
                   "-DPICO_NO_HARDWARE=0", "-DTM_WIFI_CYW43=0",
                   "-ffunction-sections", "-fdata-sections",
                   *include_flags(), "-I" + str(temporary),
                   "-I" + str(repository /
                               "lib/libtm/sysdepend/pico_rp2040/usb")]
        for relative in SDK_INCLUDES:
            command += ["-isystem", str(sdk / "src" / relative / "include")]
        command += [str(firmware / "tests/boardirq_test_main.c"),
                    "-Wl,--gc-sections", "-lm"]
        for variant, channels, confirmed, commissioned in VARIANTS:
            # Copy only into a temporary fixture; shipped hardware gates stay 0.
            fixture = temporary / variant
            fixture.mkdir()
            (fixture / "board.c").write_bytes(app_file("board.c").read_bytes())
            config = configure(
                app_file("robot_config.h").read_text(),
                channels, confirmed, commissioned)
            (fixture / "robot_config.h").write_text(config)
            for mode, flags in (("release", ["-O2"]),
                                ("sanitized", ["-O1", "-g",
                                 "-fsanitize=address,undefined,"
                                 "float-cast-overflow",
                                 "-fno-sanitize-recover=all",
                                 "-fno-omit-frame-pointer"])):
                label = variant + "_" + mode
                binary = temporary / label
                build = command[:1] + ["-I" + str(fixture)] + command[1:] + flags + ["-o", str(binary)]
                result = subprocess.run(build, text=True, capture_output=True)
                (output / (label + "_build.log")).write_text(
                    result.stdout + result.stderr)
                record = {"variant": variant, "mode": mode, "command": build,
                          "build_exit": result.returncode}
                if result.returncode == 0:
                    environment = os.environ.copy()
                    environment["ASAN_OPTIONS"] = (f"detect_leaks={leak_checks}"
                                                   ":halt_on_error=1")
                    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
                    run = subprocess.run([str(binary)], text=True,
                                         capture_output=True, env=environment)
                    (output / (label + "_test.log")).write_text(
                        run.stdout + run.stderr)
                    record.update({"test_exit": run.returncode,
                                   "stdout": run.stdout, "stderr": run.stderr})
                    failed = failed or run.returncode != 0
                    print(label + ": " + run.stdout.strip())
                else:
                    failed = True
                    print(label + ": compile failed; see " + str(output))
                report["checks"].append(record)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()

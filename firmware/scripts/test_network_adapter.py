#!/usr/bin/env python3
"""Exercise the MQTT adapter using real SDK types and mocked external calls.

Requires Pico SDK 2.2.0 with the lwIP and CYW43 submodules. The compile line
for net_mqtt.c is taken from a 'make -n' of the project-template build in a
temporary copy, so the host build sees the target's own include paths,
defines and lwipopts.h. No network or credentials are used by the test. This
is NOT an ARM build, MQTT interoperability test or hardware test. UBSan
findings are fatal in the sanitized run (-fno-sanitize-recover=all,
halt_on_error=1).

Logs go to --output, else $ROBOT_VALIDATION_DIR/network, else the git-ignored
firmware/validation/local/network; never into the dated evidence folders.
"""
from pathlib import Path
import argparse
import json
import os
import shlex
import subprocess
import tempfile

from robot_paths import REPO, app_file, copy_project, include_flags

NET_MQTT = "../app_program/subsystem1/net_mqtt.c"


def run(command: list[str], cwd: Path, log: Path, env=None) -> str:
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            capture_output=True)
    log.write_text(result.stdout + result.stderr +
                   f"\nexit={result.returncode}\n")
    if result.returncode != 0:
        raise RuntimeError(f"Command failed; see {log}")
    return result.stdout


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=REPO,
                        help="project-template checkout (default: this one)")
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--output", type=Path,
                        help="log folder (default: $ROBOT_VALIDATION_DIR/"
                             "network, else firmware/validation/local/network)")
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    firmware = Path(__file__).resolve().parents[1]
    repository = args.repository.resolve()
    sdk = args.sdk.resolve()
    if args.output is None:
        args.output = Path(os.environ.get(
            "ROBOT_VALIDATION_DIR", firmware / "validation" / "local")) / "network"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    version = (sdk / "pico_sdk_version.cmake").read_text()
    for key, value in [("MAJOR", "2"), ("MINOR", "2"), ("REVISION", "0")]:
        if f"set(PICO_SDK_VERSION_{key} {value})" not in version:
            raise ValueError("This test requires Pico SDK 2.2.0")
    with tempfile.TemporaryDirectory(prefix="inf2004_mqtt_test_") as temporary:
        project = copy_project(Path(temporary) / "project", repository)
        (project / "config/wifi_credentials.h").write_text(
            '#define UTK_WIFI_SSID "host-test-only"\n'
            '#define UTK_WIFI_PASSWORD "host-test-password"\n'
            '#define UTK_WIFI_AUTH CYW43_AUTH_WPA2_AES_PSK\n')
        config = (project / "config/net_mqtt_config.example.h").read_text()
        config = config.replace("#define NET_MQTT_CONFIGURED 0",
                                "#define NET_MQTT_CONFIGURED 1")
        (project / "config/net_mqtt_config.h").write_text(config)
        working = project / "build_make"
        dry = run(["make", "-n", "SMP=0", "CONSOLE=uart", "WIFI=cyw43",
                   "NET=lwip", "ROBOT_MQTT=1", f"PICO_SDK_PATH={sdk}"],
                  working, output / "make_dry_run.log")
        line = next(line for line in dry.splitlines()
                    if line.startswith("arm-none-eabi-gcc ")
                    and line.endswith('"' + NET_MQTT + '"'))
        command = shlex.split(line)
        command[0] = args.cc
        command = [item for item in command if not item.startswith(
            ("-mcpu=", "-mthumb", "-mfloat-abi=", "-MF", "-MT", "-MMD", "-MP"))]
        position = command.index("-o")
        del command[position:position + 2]
        command = [item for item in command
                   if item not in ["-c", "-ffreestanding", NET_MQTT]]
        # Host-only compatibility: no target ABI/code-generation claim.
        # SDK conversion warnings describe its 32-bit target assumptions.
        command = ["-isystem" + item[2:] if item.startswith("-I") and
                   not item.startswith("-I../app_program") else item
                   for item in command]
        command += ["-D__unused=__attribute__((unused))",
                    "-DSSIZE_MAX=__LONG_MAX__", "-D_POSIX_C_SOURCE=200809L",
                    "-Werror", "-Wextra", "-Wshadow", "-Wconversion",
                    "-Wswitch-enum", *include_flags(),
                    str(app_file("net_mqtt.c")),
                    str(app_file("mqttcmd.c")),
                    str(firmware / "tests/mqttnet_test_main.c")]
        (output / "host_build_command.json").write_text(
            json.dumps(command, indent=2) + "\n")
        for label, extra in [("release", []), ("sanitize", ["-g",
                              "-fsanitize=address,undefined,"
                              "float-cast-overflow",
                              "-fno-sanitize-recover=all",
                              "-fno-omit-frame-pointer"])]:
            binary = Path(temporary) / ("mqttnet_" + label)
            run(command + extra + ["-o", str(binary)], working,
                output / f"{label}_build.log")
            environment = os.environ.copy()
            # LeakSanitizer is off by project choice until it has been tried
            # on the CI runner; ROBOT_LSAN=1 enables it for a local run.
            leak_checks = int(os.environ.get("ROBOT_LSAN") == "1")
            environment["ASAN_OPTIONS"] = (f"detect_leaks={leak_checks}"
                                           ":halt_on_error=1")
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            result = run([str(binary)], working, output / f"{label}_test.log",
                         environment)
            print(f"{label}: {result.strip()}")
        if not leak_checks:
            print("LeakSanitizer disabled by project choice pending a CI "
                  "trial; ROBOT_LSAN=1 enables it.")
    print("PASS: mock adapter tests; no ARM image or network traffic generated")


if __name__ == "__main__":
    main()

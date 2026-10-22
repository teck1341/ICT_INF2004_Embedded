#!/usr/bin/env bash
# Build and structurally inspect the robot firmware; never flash it.
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    printf 'Usage: %s PROJECT_ROOT PICO_SDK_PATH [offline|mqtt]\n' "$0" >&2
    exit 2
fi
robot_scripts=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
robot_repo=$(cd -- "$1" && pwd -P)
robot_sdk=$(cd -- "$2" && pwd -P)
robot_profile=${3:-mqtt}
robot_jobs=${ROBOT_BUILD_JOBS:-4}
# The pinned Arm GNU Toolchain that CI uses (scripts/setup.sh TOOLCHAIN_BANNER).
# Another compiler still builds, but toolchain.json and build-status.json
# record toolchain_matches_pin=false so image hashes are not compared with CI.
robot_toolchain_pin="14.2.Rel1"
# The template makefile uses relative paths, so the project folder may
# contain spaces; PICO_SDK_PATH is expanded unquoted inside make and may not.
if [[ "$robot_sdk" =~ [[:space:]] || ! "$robot_jobs" =~ ^[1-9][0-9]*$ ]]; then
    printf 'Use a Pico SDK path without whitespace and a positive ROBOT_BUILD_JOBS.\n' >&2
    exit 2
fi
case "$robot_profile" in
    offline) robot_network=(WIFI=none NET=none MQTT=0 ROBOT_MQTT=0)
             robot_image=mtk3pico_smp0_uart ;;
    mqtt) robot_network=(WIFI=cyw43 NET=lwip MQTT=0 ROBOT_MQTT=1)
          robot_image=mtk3pico_smp0_uart_wifi_lwip ;;
    *) printf 'Unknown profile: %s\n' "$robot_profile" >&2; exit 2 ;;
esac
mkdir -p -- "$robot_repo/robot_build_evidence/$robot_profile"
robot_evidence="$robot_repo/robot_build_evidence/$robot_profile"
# These are generated evidence/output paths, never application source files.
# Clear previous success artifacts even when this invocation fails preflight.
rm -f -- "$robot_evidence/image-report.json" "$robot_evidence/preflight.json" \
    "$robot_evidence/postflight.json" \
    "$robot_evidence/$robot_image.elf" "$robot_evidence/$robot_image.uf2" \
    "$robot_evidence/$robot_image.map" "$robot_evidence/stack-usage.json" \
    "$robot_evidence/build-status.json" "$robot_evidence/toolchain.json"
robot_log="$robot_evidence/build.log"
{
    date -u '+Build attempt UTC: %Y-%m-%dT%H:%M:%SZ'
    printf 'Profile: %s\n' "$robot_profile"
    if ! command -v python3 >/dev/null 2>&1; then
        printf 'Required tool unavailable: python3; no build was run.\n' >&2
        exit 1
    fi
    robot_stage=toolchain
    robot_finish() {
        python3 - "$robot_evidence" "$robot_profile" "$robot_stage" "$1" \
            "$robot_scripts/build_target.sh" <<'PY'
from datetime import datetime, timezone
from pathlib import Path
import hashlib, json, sys
dest, profile, stage, code, script = sys.argv[1:]
dest = Path(dest)
code = int(code)
status = ('passed' if code == 0 and stage == 'complete' else
          'blocked' if stage == 'toolchain' else 'failed')
evidence = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(dest.iterdir()) if p.is_file() and
            p.name not in ('build.log', 'build-status.json')}
toolchain = dest / 'toolchain.json'
matches = (json.loads(toolchain.read_text()).get('toolchain_matches_pin')
           if toolchain.is_file() else None)
(dest / 'build-status.json').write_text(json.dumps({
    'status': status, 'profile': profile, 'stage': stage, 'exit_code': code,
    'completed_utc': datetime.now(timezone.utc).isoformat(),
    'build_script_sha256': hashlib.sha256(Path(script).read_bytes()).hexdigest(),
    'toolchain_matches_pin': matches,
    'evidence_sha256': evidence,
    'scope': 'Compilation and structural checks only; no hardware validation.'
}, indent=2) + '\n')
PY
    }
    trap 'robot_result=$?; robot_finish "$robot_result" || printf "Could not write build status.\n" >&2; exit "$robot_result"' EXIT
    if ! command -v git >/dev/null 2>&1; then
        printf 'Required tool unavailable: git; no source verification or build was run.\n' >&2
        exit 1
    fi
    # Keep useful source evidence even on a machine without the ARM compiler.
    robot_stage=source_integrity
    python3 "$robot_scripts/source_identity.py" --repo "$robot_repo" \
        --sdk "$robot_sdk" --profile "$robot_profile" \
        --output "$robot_evidence/preflight.json"
    robot_stage=toolchain
    for robot_tool in make g++ arm-none-eabi-gcc arm-none-eabi-size; do
        if ! command -v "$robot_tool" >/dev/null 2>&1; then
            printf 'Required tool unavailable: %s; no build was run.\n' "$robot_tool" >&2
            exit 1
        fi
    done
    arm-none-eabi-gcc --version
    python3 - "$robot_evidence/toolchain.json" "$robot_toolchain_pin" <<'PY'
from pathlib import Path
import json, shutil, subprocess, sys
tools = ('make', 'g++', 'arm-none-eabi-gcc', 'arm-none-eabi-size')
report = {name: {'path': shutil.which(name), 'version': subprocess.check_output(
    [name, '--version'], text=True).splitlines()[0]} for name in tools}
pin = sys.argv[2]
report['pinned_arm_toolchain'] = 'Arm GNU Toolchain ' + pin
report['toolchain_matches_pin'] = pin in report['arm-none-eabi-gcc']['version']
if not report['toolchain_matches_pin']:
    print('WARNING: arm-none-eabi-gcc is not the pinned Arm GNU Toolchain '
          + pin + ' that CI uses; this image hash is not comparable with CI.')
Path(sys.argv[1]).write_text(json.dumps(report, indent=2) + '\n')
PY
    robot_stage=clean
    make -C "$robot_repo/build_make" SMP=0 CONSOLE=uart \
        PICO_SDK_PATH="$robot_sdk" "${robot_network[@]}" clean
    # The upstream clean recipe ignores deletion errors and leaves UF2/stack
    # reports. Enforce fresh products even if that recipe silently kept files.
    rm -f -- "$robot_repo/build_make/$robot_image.elf" \
        "$robot_repo/build_make/$robot_image.uf2" \
        "$robot_repo/build_make/$robot_image.map"
    python3 - "$robot_repo/build_make" <<'PY'
from pathlib import Path
import sys
for suffix in ('*.o', '*.d', '*.su'):
    for path in Path(sys.argv[1]).rglob(suffix):
        path.unlink()
PY
    robot_stage=compile
    make -C "$robot_repo/build_make" -j"$robot_jobs" SMP=0 CONSOLE=uart \
        PICO_SDK_PATH="$robot_sdk" "${robot_network[@]}" all
    robot_stage=postbuild_source_integrity
    python3 "$robot_scripts/source_identity.py" --repo "$robot_repo" \
        --sdk "$robot_sdk" --profile "$robot_profile" \
        --output "$robot_evidence/postflight.json"
    python3 - "$robot_evidence/preflight.json" \
        "$robot_evidence/postflight.json" <<'PY'
from pathlib import Path
import json, sys
before, after = (json.loads(Path(name).read_text()) for name in sys.argv[1:])
if before != after:
    raise SystemExit('Source identity changed during compilation; artifacts not collected')
PY
    robot_stage=image_inspection
    python3 "$robot_scripts/inspect_target.py" \
        --elf "$robot_repo/build_make/$robot_image.elf" \
        --uf2 "$robot_repo/build_make/$robot_image.uf2" \
        --map "$robot_repo/build_make/$robot_image.map" \
        --output "$robot_evidence/image-report.json"
    robot_stage=stack_reports
    python3 - "$robot_repo/build_make" "$robot_evidence" <<'PY'
from pathlib import Path
import json, sys
root, dest = map(Path, sys.argv[1:])
reports = {str(p.relative_to(root)): p.read_text() for p in sorted(root.rglob('*.su'))}
if not reports:
    raise SystemExit('No compiler .su reports produced; stack evidence incomplete')
(dest / 'stack-usage.json').write_text(json.dumps({
    'kind': 'Compiler per-function stack reports; not worst-case call-chain proof',
    'reports': reports}, indent=2) + '\n')
PY
    cp -- "$robot_repo/build_make/$robot_image.elf" \
        "$robot_repo/build_make/$robot_image.uf2" \
        "$robot_repo/build_make/$robot_image.map" "$robot_evidence/"
    robot_stage=complete
    printf 'Target compilation and structural image checks completed.\n'
    printf 'Physical calibration, stack high-water and robot validation remain open.\n'
} 2>&1 | tee "$robot_log"
printf 'Evidence and images: %s\n' "$robot_evidence"

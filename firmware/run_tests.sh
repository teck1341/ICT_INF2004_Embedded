#!/usr/bin/env bash
# Host-only portable module checks; hardware adapters are excluded deliberately.
set -euo pipefail
shopt -s nullglob

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
app_dir="$(cd -- "${project_dir}/../app_program" && pwd)"
build_dir="${project_dir}/build-host"
# ROBOT_VALIDATION_DIR redirects logs. The default is the git-ignored
# validation/local folder, so a direct run never overwrites dated evidence.
log_dir="${ROBOT_VALIDATION_DIR:-${project_dir}/validation/local}"
compiler="${CC:-gcc}"
test_filter="${1:-test_*_main}"
mkdir -p "$build_dir" "$log_dir"
# LeakSanitizer is off by project choice until it has been tried on the CI
# runner; ROBOT_LSAN=1 enables it for a local run.
if [[ "${ROBOT_LSAN:-0}" == 1 ]]; then
    detect_leaks=1
    leak_note='LeakSanitizer enabled (ROBOT_LSAN=1).'
else
    detect_leaks=0
    leak_note='LeakSanitizer disabled by project choice pending a CI trial; ROBOT_LSAN=1 enables it.'
fi

common_flags=(
    -std=c99 -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wundef
    -Wcast-qual -Wwrite-strings -Wswitch-enum -Wnull-dereference
)
for app_subdir in "" subsystem1 subsystem2 subsystem3 subsystem4 \
                  subsystem5; do
    common_flags+=(-I"${app_dir}/${app_subdir}")
done
# Hardware-independent modules; scripts/robot_paths.py keeps the same list.
sources=()
for module in bitpack core subsystem1/mqttcmd \
              subsystem1/telemetry subsystem2/motion subsystem3/barcode \
              subsystem3/line_ctrl subsystem4/terrain subsystem5/scanner \
              subsystem5/vehicle; do
    sources+=("${app_dir}/${module}.c")
done
test_sources=("${project_dir}"/tests/test_*_main.c)

{
    date -u '+Validation UTC: %Y-%m-%dT%H:%M:%SZ'
    "$compiler" --version
    printf '\nStrict host build flags: '
    printf '%q ' "${common_flags[@]}"
    printf '\nSelected test pattern: %s\n' "$test_filter"
    printf 'Compiled portable sources:\n'
    printf '  %s\n' "${sources[@]}"
    test_count=0
    for test_source in "${test_sources[@]}"; do
        test_name="${test_source##*/}"
        test_name="${test_name%.c}"
        if [[ "$test_name" == $test_filter ]]; then
            test_count=$((test_count + 1))
            printf '\n%s: release build (-O2 -DNDEBUG)\n' "$test_name"
            "$compiler" "${common_flags[@]}" -O2 -DNDEBUG \
                "${sources[@]}" "$test_source" -lm \
                -o "${build_dir}/${test_name}_release"
            "${build_dir}/${test_name}_release"
            printf '\n%s: ASan + UBSan build (-O1 -g)\n' "$test_name"
            "$compiler" "${common_flags[@]}" -O1 -g \
                -fno-omit-frame-pointer                 -fsanitize=address,undefined,float-cast-overflow \
                -fno-sanitize-recover=all "${sources[@]}" "$test_source" \
                -lm -o "${build_dir}/${test_name}_sanitized"
            printf '%s\n' "$leak_note"
            ASAN_OPTIONS="detect_leaks=${detect_leaks}:halt_on_error=1" \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                "${build_dir}/${test_name}_sanitized"
        fi
    done
    if [[ "$test_count" -eq 0 ]]; then
        printf 'ERROR: no test main matched the requested pattern.\n' >&2
        exit 1
    fi
    printf '\nPassed %d executable(s) in both host configurations.\n' \
        "$test_count"
    printf 'Hardware, timing, and RTOS behavior remain untested here.\n'
} 2>&1 | tee "${log_dir}/host-test-log.txt"

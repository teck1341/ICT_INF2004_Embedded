#!/usr/bin/env python3
"""Run the radio startup source with mocked driver/kernel boundaries.

Compiles the project-template cyw43_utk.c, with the INF2004 ROBOT_MQTT hooks,
using actual Pico SDK headers and the compile line from a 'make -n' of the
template build in a temporary copy. The kernel-facing files use explicit
host stubs to keep the two size_t ABIs apart. This does not execute ARM
code, PIO/DMA teardown or radio/network hardware.

The owner file is built with -DCNF_SMP=1 on the host only: the single-core
branch defines its memory barrier as an ARM 'dmb' instruction, so the SMP
branch's extern tk_get_prc()/mp_memory_barrir() are used and stubbed here.

The stubs check with assert(). The owner compile line is copied from the
template makefile, which adds -DNDEBUG, so both stubs start with #undef
NDEBUG and the owner command appends -UNDEBUG. Every build also runs a
negative control that must abort on a failed assertion, proving the checks
are live. UBSan findings are fatal (-fno-sanitize-recover=all,
halt_on_error=1).
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import tempfile

from robot_paths import REPO, copy_project

OWNER = "lib/libwifi/sysdepend/pico_rp2040/cyw43_utk.c"
KERNEL = "lib/libwifi/sysdepend/pico_rp2040/cyw43_utk_kernel.c"
DEVICE = "kernel/sysdepend/pico_rp2040/devinit.c"
MARKER = "/* INF2004 ROBOT_MQTT radio startup v2 */"
# Each INF2004 hook must appear exactly once in the committed template files.
HOOKS = {
    OWNER: (MARKER, "result = robot_radio_setup();",
            "net_mqtt_poll(&cyw43_state.netif[CYW43_ITF_STA]);"),
    KERNEL: (MARKER, "(void)tk_del_tsk(task);"),
    DEVICE: (MARKER, "board_boot_brake();"),
}

OWNER_STUB = r'''
/* The checks below must run whatever flags the template makefile adds. */
#undef NDEBUG
#include "cyw43.h"
#include "cyw43_utk.h"
#include "net_mqtt.h"
#include <tm/tmonitor.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

cyw43_t cyw43_state;
void (*cyw43_poll)(void);
volatile uint32_t cyw43_utk_poll_requested;
static jmp_buf finish;
static int scenario;
static unsigned attempts, low_teardowns, full_teardowns, init_calls;
static unsigned joins, mqtt_calls, timeout_calls, lwip_calls, backoffs;
static unsigned owned_resources, thread_depth;
static uint32_t delays[2];

static void fake_poll(void) { }
void mp_memory_barrir(void) { }
int32_t tk_get_prc(void) { return 1; }
uint64_t time_us_64(void) { return 1000; }
INT tm_printf(const UB *format, ...) { (void)format; return 0; }
void cyw43_thread_enter(void) { thread_depth++; }
void cyw43_thread_exit(void) { assert(thread_depth > 0); thread_depth--; }
void lwip_init(void) { lwip_calls++; }
void sys_check_timeouts(void) { timeout_calls++; }
void net_mqtt_poll(struct netif *n)
{
    assert(ROBOT_MQTT == 1);
    assert(n == &cyw43_state.netif[CYW43_ITF_STA]);
    assert(cyw43_state.itf_state != 0);
    mqtt_calls++;
}
void cyw43_init(cyw43_t *s)
{
    assert(owned_resources == 0);
    init_calls++;
    memset(s, 0, sizeof(*s));
    cyw43_poll = NULL;
}
void cyw43_ll_deinit(cyw43_ll_t *s)
{
    assert(s == &cyw43_state.cyw43_ll);
    assert(thread_depth > 0);
    low_teardowns++;
    owned_resources = 0;
}
void cyw43_deinit(cyw43_t *s)
{
    assert(cyw43_poll != NULL);
    assert(thread_depth > 0);
    full_teardowns++;
    owned_resources = 0;
    cyw43_init(s);
}
void cyw43_wifi_set_up(cyw43_t *s, int itf, bool up, uint32_t country)
{
    (void)country;
    assert(up && itf == CYW43_ITF_STA && thread_depth > 0);
    assert(owned_resources == 0);
    owned_resources = 1;
    attempts++;
    if((scenario == 1 || scenario == 2) && attempts < 3) {
        if(scenario == 2) cyw43_poll = fake_poll;
        return;
    }
    if(scenario == 3 || scenario == 4) {
        if(scenario == 4) cyw43_poll = fake_poll;
        return;
    }
    s->itf_state = 1;
    s->netif[itf].state = s;
    cyw43_poll = fake_poll;
}
int cyw43_wifi_get_mac(cyw43_t *s, int itf, uint8_t mac[6])
{
    (void)s; (void)itf;
    memset(mac, 1, 6);
    return 0;
}
int cyw43_wifi_join(cyw43_t *s, size_t a, const uint8_t *b,
                   size_t c, const uint8_t *d, uint32_t e,
                   const uint8_t *f, uint32_t g)
{
    (void)s; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    /* The robot build joins from net_mqtt_poll(), never from the owner. */
    assert(ROBOT_MQTT == 0);
    assert(thread_depth > 0);
    joins++;
    return 0;
}
int cyw43_tcpip_link_status(cyw43_t *s, int itf)
{ (void)s; (void)itf; return CYW43_LINK_DOWN; }
int32_t tk_dly_tsk(uint32_t delay)
{
    assert(thread_depth == 0);
    if(delay == 2) longjmp(finish, 1);
    if(!ROBOT_MQTT || attempts == 3) longjmp(finish, 2);
    assert(backoffs < 2 && owned_resources == 0);
    delays[backoffs++] = delay;
    return 0;
}
int main(int argc, char **argv)
{
    T_CYW43_UTK_STATUS status;
    assert(argc == 2);
    scenario = argv[1][0] - '0';
    if(scenario == 9) {
        /* Negative control: the harness requires this check to abort. */
        assert(scenario != 9);
        printf("NEGATIVE CONTROL NOT TRIGGERED\n");
        return 0;
    }
    int outcome = setjmp(finish);
    if(outcome == 0) cyw43_utk_task(0, NULL);
    cyw43_utk_get_status(&status);
    assert(lwip_calls == 1 && status.init_complete == 1);
    assert(thread_depth == 0 && status.scan_active == 0);
    if(ROBOT_MQTT) {
        assert(joins == 0);
        assert(attempts == (scenario == 0 ? 1U : 3U));
        assert(backoffs == (scenario == 0 ? 0U : 2U));
        if(backoffs != 0) assert(delays[0] == 1000 && delays[1] == 2000);
        if(scenario == 3 || scenario == 4) {
            assert(outcome == 2 && status.ready == 0);
            assert(status.init_result == -CYW43_EIO);
            assert(mqtt_calls == 0 && timeout_calls == 0);
            assert(owned_resources == 0 && init_calls == 4);
        } else {
            assert(outcome == 1 && status.ready == 1);
            assert(status.init_result == 0);
            assert(mqtt_calls == 1 && timeout_calls == 1);
            assert(owned_resources == 1 && init_calls == attempts);
        }
        assert(low_teardowns == (scenario == 1 ? 2U :
                                scenario == 3 ? 3U : 0U));
        assert(full_teardowns == (scenario == 2 ? 2U :
                                 scenario == 4 ? 3U : 0U));
    } else {
        /* The template's own NET=lwip path: one attempt, its own join. */
        assert(attempts == 1 && backoffs == 0 && init_calls == 1);
        assert(low_teardowns == 0 && full_teardowns == 0);
        assert(mqtt_calls == 0);
        if(scenario == 3) {
            assert(outcome == 2 && status.init_result == -CYW43_EIO);
            assert(joins == 0 && timeout_calls == 0);
        } else {
            assert(outcome == 1 && status.init_result == 0);
            assert(joins == 1 && timeout_calls == 1);
        }
    }
    printf("PASS radio profile=%d scenario=%d\n", ROBOT_MQTT, scenario);
    return 0;
}
'''

KERNEL_HEADER = r'''
#ifndef HOST_KERNEL_H
#define HOST_KERNEL_H
#define EXPORT
#define IMPORT extern
#define E_OK 0
#define TA_HLNG 1
#define TA_RNG1 2
#define PICO_RP2040 1
#define TM_WIFI_CYW43 1
#define TM_NET_LWIP 1
#define TK_SUPPORT_SMP 0
#define USE_OBJECT_NAME 0
#define USE_SDEV_DRV 0
#define USE_TMONITOR 0
#define USE_SHUTDOWN 1
typedef int ER;
typedef int ID;
typedef int INT;
typedef void (*FP)(void);
typedef struct {
    int tskatr; FP task; int itskpri; int stksz; int assprc;
} T_CTSK;
int tk_cre_tsk(T_CTSK *definition);
int tk_sta_tsk(ID task, int code);
int tk_del_tsk(ID task);
int cyw43_utk_start(void);
int knl_start_device(void);
#endif
'''

KERNEL_STUB = r'''
/* The checks below must run whatever flags the build adds. */
#undef NDEBUG
#include "host_kernel.h"
#include <assert.h>
#include <stdio.h>
static int create_result, start_result, creates, starts, deletes, brakes;
int tk_cre_tsk(T_CTSK *definition)
{
    /* lwIP runs on the radio task: the template's 8 KiB stack. */
    assert(definition->stksz == 8192);
    /* The robot radio task sits below application priorities 1..8. */
    assert(definition->itskpri == (ROBOT_MQTT ? 9 : 4));
    if(ROBOT_MQTT) assert(brakes == 1);
    creates++;
    return create_result;
}
int tk_sta_tsk(ID task, int code)
{ assert(task == 7 && code == 0); starts++; return start_result; }
int tk_del_tsk(ID task) { assert(task == 7); deletes++; return 0; }
void board_boot_brake(void) { assert(creates == 0); brakes++; }
void cyw43_utk_task(INT a, void *b) { (void)a; (void)b; }
int main(int argc, char **argv)
{
    (void)argv;
    if(argc > 1) {
        /* Negative control: the harness requires this check to abort. */
        assert(argc == 1);
        printf("NEGATIVE CONTROL NOT TRIGGERED\n");
        return 0;
    }
    for(int scenario = 0; scenario < 3; scenario++) {
        creates = starts = deletes = brakes = 0;
        create_result = scenario == 1 ? -10 : 7;
        start_result = scenario == 2 ? -20 : 0;
        int result = knl_start_device();
        int expected = scenario == 1 ? -10 : scenario == 2 ? -20 : 0;
        assert(result == (ROBOT_MQTT ? expected : 0));
        assert(creates == 1 && starts == (scenario == 1 ? 0 : 1));
        assert(deletes == (ROBOT_MQTT && scenario == 2 ? 1 : 0));
        assert(brakes == (ROBOT_MQTT ? 1 : 0));
        printf("PASS kernel profile=%d scenario=%d\n", ROBOT_MQTT, scenario);
    }
    return 0;
}
'''


def run(command: list[str], cwd: Path, log: Path, env=None) -> str:
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            capture_output=True)
    log.write_text(result.stdout + result.stderr +
                   f"\nexit={result.returncode}\n")
    if result.returncode:
        raise RuntimeError(f"Command failed; see {log}")
    return result.stdout


def run_negative_control(command: list[str], cwd: Path, log: Path,
                         env) -> str:
    """Require a deliberately false assert() to abort the binary."""
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            capture_output=True)
    output = result.stdout + result.stderr
    log.write_text(output + f"\nexit={result.returncode}\n")
    if result.returncode == 0 or "NEGATIVE CONTROL NOT TRIGGERED" in output:
        raise RuntimeError("Negative control did not fail: assertions are "
                           f"not live in this build; see {log}")
    if "Assertion" not in output:
        raise RuntimeError("Negative control stopped (exit "
                           f"{result.returncode}) before its assertion, for "
                           f"example on a sanitizer error; see {log}")
    return f"aborted as required (exit {result.returncode})"


def check_hooks(repository: Path) -> dict:
    """Each committed INF2004 hook occurs exactly once; return file hashes."""
    for name, needles in HOOKS.items():
        text = (repository / name).read_text()
        for needle in needles:
            if text.count(needle) != 1:
                raise AssertionError(f"{name}: expected one '{needle}'")
    return {name: hashlib.sha256((repository / name).read_bytes()).hexdigest()
            for name in HOOKS}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=REPO,
                        help="project-template checkout (default: this one)")
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    repository = args.repository.resolve()
    report = {"scope": "Template radio source with host driver/kernel stubs",
              "hardware_test": False, "arm_build": False, "results": []}
    report["integration_sha256"] = check_hooks(repository)
    report["integration_checks"] = ["Each INF2004 hook present exactly once"]
    with tempfile.TemporaryDirectory(prefix="inf2004_radio_") as temporary:
        temp = Path(temporary)
        project = copy_project(temp / "project", repository)
        config = project / "config"
        shutil.copy2(config / "net_mqtt_config.example.h",
                     config / "net_mqtt_config.h")
        shutil.copy2(config / "wifi_secrets.example.h",
                     config / "wifi_secrets.h")
        (config / "wifi_credentials.h").write_text(
            '#define UTK_WIFI_SSID "test"\n'
            '#define UTK_WIFI_PASSWORD "testpass"\n'
            '#define UTK_WIFI_AUTH CYW43_AUTH_WPA2_AES_PSK\n')
        work = project / "build_make"
        plan = run(["make", "-n", "SMP=0", "CONSOLE=uart", "WIFI=cyw43",
                    "NET=lwip", "ROBOT_MQTT=1",
                    f"PICO_SDK_PATH={args.sdk.resolve()}"], work,
                   output / "make-plan.log")
        owner = "../" + OWNER
        line = next(line for line in plan.splitlines()
                    if line.startswith("arm-none-eabi-gcc ")
                    and line.endswith('"' + owner + '"'))
        command = shlex.split(line)
        command[0] = args.cc
        position = command.index("-o")
        del command[position:position + 2]
        command = [item for item in command if not item.startswith(
            ("-mcpu=", "-mthumb", "-mfloat-abi=", "-MF", "-MT", "-MMD", "-MP"))
            and item not in ("-c", "-ffreestanding", owner)]
        command = ["-isystem" + item[2:] if item.startswith("-I") else item
                   for item in command]
        stub = temp / "owner_stub.c"
        stub.write_text(OWNER_STUB)
        # The template makefile passes -DNDEBUG; -UNDEBUG (after it) keeps
        # the stub's assert() checks compiled in.
        command += ["-D__unused=__attribute__((unused))",
                    "-DSSIZE_MAX=__LONG_MAX__", "-D_POSIX_C_SOURCE=200809L",
                    "-DCNF_SMP=1", "-Werror", "-UNDEBUG", owner, str(stub)]
        report["owner_host_command"] = command
        headers = temp / "headers"
        headers.mkdir()
        (headers / "host_kernel.h").write_text(KERNEL_HEADER)
        for name in ["tk/tkernel.h", "sys/machine.h", "sys/sysdef.h",
                     "tm/tmonitor.h", "tk/device.h", "kernel.h", "sysdepend.h"]:
            target = headers / name
            target.parent.mkdir(exist_ok=True)
            target.write_text('#include "host_kernel.h"\n')
        kernel_stub = temp / "kernel_stub.c"
        kernel_stub.write_text(KERNEL_STUB)
        kernel_sources = []
        for name in [KERNEL, DEVICE]:
            target = temp / Path(name).name
            shutil.copy2(repository / name, target)
            kernel_sources.append(str(target))
        env = os.environ.copy()
        # LeakSanitizer is off by project choice until it has been tried on
        # the CI runner; ROBOT_LSAN=1 enables it for a local run.
        leak_checks = os.environ.get("ROBOT_LSAN") == "1"
        env["ASAN_OPTIONS"] = f"detect_leaks={int(leak_checks)}:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        report["leak_sanitizer"] = ("enabled (ROBOT_LSAN=1)" if leak_checks else
                                    "disabled by project choice pending a CI "
                                    "trial; ROBOT_LSAN=1 enables it")
        report["negative_controls"] = []
        for mode, extra in [("release", ["-O2"]),
                            ("sanitize", ["-O1", "-g",
                                          "-fsanitize=address,undefined,"
                                          "float-cast-overflow",
                                          "-fno-sanitize-recover=all",
                                          "-fno-omit-frame-pointer"])]:
            for profile in (1, 0):
                tag = f"{mode}-profile{profile}"
                binary = temp / tag
                profile_command = [item.replace("-DROBOT_MQTT=1",
                    f"-DROBOT_MQTT={profile}") for item in command]
                run(profile_command + extra + ["-o", str(binary)], work,
                    output / (tag + "-build.log"))
                report["negative_controls"].append(f"{tag} owner: " +
                    run_negative_control([str(binary), "9"], work,
                        output / f"{tag}-negative-control.log", env))
                for scenario in (range(5) if profile else (0, 3)):
                    result = run([str(binary), str(scenario)], work,
                                 output / f"{tag}-scenario{scenario}.log", env)
                    report["results"].append(result.strip())
                kernel_binary = temp / (tag + "-kernel")
                kernel_command = [args.cc, "-std=c99", "-Wall", "-Wextra",
                                  "-Werror", f"-DROBOT_MQTT={profile}",
                                  f"-I{headers}"] + kernel_sources + [
                                  str(kernel_stub)] + extra + ["-o", str(kernel_binary)]
                run(kernel_command, work, output / (tag + "-kernel-build.log"))
                report["negative_controls"].append(f"{tag} kernel: " +
                    run_negative_control([str(kernel_binary), "negative"], work,
                        output / f"{tag}-kernel-negative-control.log", env))
                result = run([str(kernel_binary)], work,
                             output / (tag + "-kernel.log"), env)
                report["results"].extend(result.strip().splitlines())
        report["scenario_executions"] = len(report["results"])
        report["status"] = "PASS"
    (output / "radio-startup-report.json").write_text(
        json.dumps(report, indent=2) + "\n")
    print(f"PASS: {report['scenario_executions']} radio/kernel scenario executions; "
          f"{len(report['negative_controls'])} negative controls aborted as required")


if __name__ == "__main__":
    main()

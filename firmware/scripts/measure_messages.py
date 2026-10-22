#!/usr/bin/env python3
"""Measure telemetry and command message sizes before and after the change.

--before points at a firmware tree with the original integer-JSON encoder
(telemetry_format*), --after at one with the binary encoder
(telemetry_encode_*). Each tree's own telemetry.c is compiled on the host and
run on a worst-case and a typical snapshot; payload lengths are measured,
not quoted. Command tokens and presence payloads are read from each tree's
own mqttcmd.c and net_mqtt.c. Full MQTT 3.1.1 QoS0 PUBLISH sizes add the
fixed header (1 byte plus a 1- or 2-byte remaining length), the 2-byte topic
length and the topic.

This is a host measurement of payload bytes; it says nothing about radio
timing or delivery. Each tree may use the original firmware/src, include
and target layout or the project-template app_program/ layout.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from robot_paths import app_file, include_flags


def locate(firmware: Path, old: str) -> Path:
    """A source in the original layout (old, e.g. 'src/telemetry.c'), else
    the same file name in the app_program/ next to that firmware folder."""
    if (firmware / old).is_file():
        return firmware / old
    return app_file(Path(old).name, firmware.parent / "app_program")


def includes(firmware: Path) -> list[str]:
    """Header search flags for either layout."""
    if (firmware / "include").is_dir():
        return [f"-I{firmware / 'include'}"]
    return include_flags(firmware.parent / "app_program")

BEFORE = r'''
#include "telemetry.h"
#include <stdio.h>
static void show (const char * name, size_t length) {
    printf("%s %u\n", name, (unsigned)length);
}
int main (void)
{
    char text[TELEMETRY_BUFFER_SIZE];
    telemetry_status_t s = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN,
        INT32_MIN, false, false, false, false};
    telemetry_status_t t = {1234U, 456789U, 0U, 0U, 40U, 2, 81, 79, 12, 25,
        734, false, false, true, true};
    telemetry_perception_t p = {UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN,
        INT32_MIN, INT32_MIN, UINT32_MAX, UINT32_MAX, false, false, false};
    telemetry_perception_t q = {1235U, 457000U, 3U, 0U, 4, 84, -15, 2, 212,
        388, 1, 61, 0, true, false, true};
    telemetry_diagnostic_t d = {UINT32_MAX, UINT32_MAX, UINT32_MAX,
        INT32_MIN, UINT32_MAX, UINT32_MAX, UINT32_MAX, INT32_MIN, false};
    telemetry_diagnostic_t e = {1235U, 457000U, 3U, 0, 0U, 1U, 2U, 27, true};
    show("status_worst", telemetry_format(&s, text, sizeof(text)));
    show("status_typical", telemetry_format(&t, text, sizeof(text)));
    show("perception_worst",
         telemetry_format_perception(&p, text, sizeof(text)));
    show("perception_typical",
         telemetry_format_perception(&q, text, sizeof(text)));
    show("diagnostic_worst",
         telemetry_format_diagnostic(&d, text, sizeof(text)));
    show("diagnostic_typical",
         telemetry_format_diagnostic(&e, text, sizeof(text)));
    show("buffer", TELEMETRY_BUFFER_SIZE);
    return 0;
}
'''

AFTER = r'''
#include "telemetry.h"
#include <stdio.h>
#include <string.h>
static void show (const char * name, size_t length) {
    printf("%s %u\n", name, (unsigned)length);
}
int main (void)
{
    uint8_t frame[TELEMETRY_FRAME_MAX];
    telemetry_status_t s;
    telemetry_perception_t p;
    telemetry_diagnostic_t d;

    memset(&s, 0xFF, sizeof(s));
    memset(&p, 0xFF, sizeof(p));
    memset(&d, 0xFF, sizeof(d));
    s.b_stop = true; s.b_hardfault = true; s.b_heightvalid = true;
    s.b_rangevalid = true;
    p.b_tilt_valid = true; p.b_hump_active = true; p.b_scan_valid = true;
    p.b_obstacle_seen = true;
    d.b_mission_valid = true;
    show("status_worst", telemetry_encode_status(&s, frame, sizeof(frame)));
    memset(&s, 0, sizeof(s));
    show("status_typical", telemetry_encode_status(&s, frame, sizeof(frame)));
    show("perception_worst",
         telemetry_encode_perception(&p, frame, sizeof(frame)));
    memset(&p, 0, sizeof(p));
    show("perception_typical",
         telemetry_encode_perception(&p, frame, sizeof(frame)));
    show("diagnostic_worst",
         telemetry_encode_diagnostic(&d, frame, sizeof(frame)));
    memset(&d, 0, sizeof(d));
    show("diagnostic_typical",
         telemetry_encode_diagnostic(&d, frame, sizeof(frame)));
    show("buffer", TELEMETRY_FRAME_MAX);
    return 0;
}
'''

COMMAND_NAMES = ("STOP", "START", "RESET", "LEFT", "RIGHT", "STRAIGHT",
                 "UTURN")
PRESENCE = {"online": (r'#define NET_MQTT_ONLINE "([^"]*)"',
                       r'NET_MQTT_STATUS_TOPIC, "([^"]*)"'),
            "offline": (r'#define NET_MQTT_OFFLINE "([^"]*)"',
                        r'\.will_msg\s*=\s*"([^"]*)"')}


def text_commands(firmware: Path) -> dict:
    """Lengths of the ASCII command tokens quoted in a tree's mqttcmd.c."""
    source = locate(firmware, "src/mqttcmd.c").read_text()
    return {name: len(name) for name in COMMAND_NAMES
            if f'"{name}"' in source}


def presence_payloads(firmware: Path) -> dict:
    """Online and offline (will) payload lengths in a tree's net_mqtt.c."""
    source = locate(firmware, "target/net_mqtt.c").read_text()
    sizes = {}
    for name, patterns in PRESENCE.items():
        for pattern in patterns:
            match = re.search(pattern, source)
            if match:
                sizes[name] = len(match.group(1))
                break
        else:
            raise SystemExit(f"{firmware}: no {name} presence payload")
    return sizes


def run_probe(firmware: Path, program: str, extra: list[str]) -> dict:
    """Compile one tree's telemetry.c with the probe and parse its sizes."""
    with tempfile.TemporaryDirectory() as folder:
        probe = Path(folder) / "probe.c"
        binary = Path(folder) / "probe"
        probe.write_text(program)
        sources = [str(locate(firmware, "src/telemetry.c"))] + [
            str(locate(firmware, name)) for name in extra]
        subprocess.run(["gcc", "-std=c99", "-O1", *includes(firmware),
                        str(probe), *sources, "-lm", "-o", str(binary)],
                       check=True)
        output = subprocess.run([str(binary)], check=True, text=True,
                                capture_output=True).stdout
    return {name: int(value) for name, value in
            (line.split() for line in output.splitlines())}


def publish_bytes(topic_bytes: int, payload: int) -> int:
    """MQTT 3.1.1 QoS0 PUBLISH: fixed header, topic length, topic, payload."""
    remaining = 2 + topic_bytes + payload
    length_bytes = 1 if remaining < 128 else 2
    return 1 + length_bytes + remaining


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--topic-bytes", type=int, default=33,
                        help="telemetry topic length (example config: 33)")
    parser.add_argument("--command-topic-bytes", type=int, default=31)
    parser.add_argument("--status-topic-bytes", type=int, default=30)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    before = run_probe(args.before.resolve(), BEFORE, [])
    after = run_probe(args.after.resolve(), AFTER, ["src/bitpack.c"])
    rows = []
    for name in ("status", "perception", "diagnostic"):
        for case in ("worst", "typical"):
            key = f"{name}_{case}"
            rows.append({
                "message": f"{name} frame ({case})",
                "before_payload": before[key], "after_payload": after[key],
                "before_publish": publish_bytes(args.topic_bytes, before[key]),
                "after_publish": publish_bytes(args.topic_bytes, after[key]),
            })
    commands = text_commands(args.before.resolve())
    if sorted(commands) != sorted(COMMAND_NAMES):
        raise SystemExit("before tree: missing command tokens")
    if text_commands(args.after.resolve()):
        raise SystemExit("after tree still contains text command tokens")
    worst = max(commands.values())
    for label, before_bytes in (("command (largest token / any command)",
                                 worst), ("STOP command", commands["STOP"])):
        rows.append({"message": label, "before_payload": before_bytes,
                     "after_payload": 1,
                     "before_publish": publish_bytes(
                         args.command_topic_bytes, before_bytes),
                     "after_publish": publish_bytes(
                         args.command_topic_bytes, 1)})
    old_presence = presence_payloads(args.before.resolve())
    new_presence = presence_payloads(args.after.resolve())
    for name in ("online", "offline"):
        rows.append({"message": f"presence '{name}' (status topic)",
                     "before_payload": old_presence[name],
                     "after_payload": new_presence[name],
                     "before_publish": publish_bytes(
                         args.status_topic_bytes, old_presence[name]),
                     "after_publish": publish_bytes(
                         args.status_topic_bytes, new_presence[name])})
    report = {"topic_bytes": args.topic_bytes,
              "command_topic_bytes": args.command_topic_bytes,
              "status_topic_bytes": args.status_topic_bytes,
              "encoder_buffer_before": before["buffer"],
              "encoder_buffer_after": after["buffer"], "rows": rows,
              "scope": "Host-measured payload lengths from each tree's own "
                       "encoder; MQTT framing computed for QoS0 PUBLISH."}
    print("| Message | Payload before (B) | Payload after (B) | "
          "MQTT PUBLISH before (B) | MQTT PUBLISH after (B) |")
    print("|---|---:|---:|---:|---:|")
    for row in rows:
        print(f"| {row['message']} | {row['before_payload']} | "
              f"{row['after_payload']} | {row['before_publish']} | "
              f"{row['after_publish']} |")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

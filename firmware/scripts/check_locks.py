#!/usr/bin/env python3
"""Check that every write to shared packed data is inside app_main_lock().

The Cortex-M0+ has no exclusive load/store, so a bit-field store, a counter
increment or any read-modify-write of data shared between tasks, or between
a task and the GPIO ISR, is safe only inside the PRIMASK region opened by
app_main_lock() and closed by app_main_unlock(). This lexical checker walks
each function in order, tracks the lock depth (restoring it after a block
that unlocks and then returns), and reports every write to a watched name
outside a locked region.

Exemptions are explicit and narrow: functions that run before any task
starts (initialisation), functions whose name ends in "_locked" (their
contract is that the caller holds the lock; every call to one must itself be
inside a locked region), and ISR-context functions for ISR-owned words (an
ISR cannot be preempted by the tasks that share the word on this single
core). This is a lexical check of the project's coding pattern, not a proof
of absence of races; it complements code review and hardware tests.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# file (from the repository root) -> watched shared names, functions exempt
# from the lock rule
RULES = {
    "app_program/robot_app.c": {
        "watched": ("g_shared",),
        "exempt": ("robot_app_init",),
    },
    "app_program/board.c": {
        "watched": ("g_encoder_word", "g_encoder_totals", "g_encoder_errors",
                    "g_echo_word", "g_motor_directions"),
        "exempt": ("board_init", "board_init_inputs", "board_init_motor",
                   "board_gpio_isr", "board_count_encoder",
                   "board_count_transition", "board_add_count",
                   "board_record_error", "board_echo_edge"),
    },
}
ASSIGN = r"(?:[-+*/%&|^]|<<|>>)?=(?!=)"
FUNCTION = re.compile(r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{", re.S)


def mask(text: str) -> str:
    """Blank comments and string/char literals, keeping offsets."""
    out = list(text)
    index = 0
    while index < len(text):
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = len(text) if end < 0 else end + 2
        elif text.startswith("//", index):
            end = text.find("\n", index)
            end = len(text) if end < 0 else end
        elif text[index] in "\"'":
            quote = text[index]
            end = index + 1
            while end < len(text) and text[end] != quote:
                end += 2 if text[end] == "\\" else 1
            end = min(end + 1, len(text))
        else:
            index += 1
            continue
        for position in range(index, end):
            if out[position] != "\n":
                out[position] = " "
        index = end
    return "".join(out)


def functions(text: str):
    """Yield (name, body_start, body_end) for top-level function bodies."""
    depth = 0
    index = 0
    while index < len(text):
        if depth == 0:
            match = FUNCTION.match(text, index)
            if match and match.group(1) not in ("if", "for", "while",
                                                 "switch"):
                start = match.end() - 1
                level = 0
                for end in range(start, len(text)):
                    if text[end] == "{":
                        level += 1
                    elif text[end] == "}":
                        level -= 1
                        if level == 0:
                            yield match.group(1), start, end
                            index = end + 1
                            break
                else:
                    return
                continue
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
        index += 1


def scan(name: str, body: str, offset: int, source: str, watched,
         locked_helpers) -> tuple[list[dict], set[str]]:
    """Walk one body in order; report unlocked writes and helper calls."""
    events = []
    for match in re.finditer(r"\bapp_main_lock\s*\(", body):
        events.append((match.start(), "lock"))
    for match in re.finditer(r"\bapp_main_unlock\s*\(", body):
        events.append((match.start(), "unlock"))
    for match in re.finditer(r"\breturn\b", body):
        events.append((match.start(), "return"))
    for match in re.finditer(r"[{}]", body):
        events.append((match.start(), match.group(0)))
    pattern = "|".join(re.escape(item) for item in watched)
    write = re.compile(
        rf"(?:\+\+|--)\s*(?:{pattern})\b|\b(?:{pattern})\b(?:\s*(?:\.|->)\s*"
        rf"\w+|\s*\[[^\]]*\])*\s*(?:{ASSIGN}|\+\+|--)")
    for match in write.finditer(body):
        events.append((match.start(), "write"))
    for match in re.finditer(r"\b(\w+_locked)\s*\(", body):
        events.append((match.start(), "call:" + match.group(1)))
    events.sort()
    depth = 0
    stack: list[tuple[int, bool]] = []
    unlocked_return = False
    findings = []
    called = set()
    for position, kind in events:
        line = source.count("\n", 0, offset + position) + 1
        if kind == "lock":
            depth += 1
        elif kind == "unlock":
            depth = max(0, depth - 1)
        elif kind == "{":
            stack.append((depth, unlocked_return))
            unlocked_return = False
        elif kind == "}":
            entry, outer = stack.pop() if stack else (depth, False)
            if unlocked_return:
                # The block unlocked and left the function: the code after
                # the block still runs under the depth it was entered with.
                depth = entry
            unlocked_return = outer
        elif kind == "return":
            if stack and depth < stack[-1][0]:
                unlocked_return = True
        elif kind == "write" and depth == 0:
            findings.append({"function": name, "line": line,
                             "detail": "write to shared data outside "
                                       "app_main_lock()"})
        elif kind.startswith("call:"):
            helper = kind[5:]
            called.add(helper)
            if depth == 0 and helper in locked_helpers:
                findings.append({"function": name, "line": line,
                                 "detail": f"call to {helper} outside "
                                           "app_main_lock()"})
    return findings, called


SELF_TEST = r'''
static void helper_locked (void) { g_shared.a = 1; }
static void good (void)
{
    uint32_t saved = app_main_lock();
    if (x) { app_main_unlock(saved); return; }
    g_shared.a = 1; /* g_shared.z = 9; is a comment */
    helper_locked();
    app_main_unlock(saved);
    if (g_shared.a == 1) { y = g_shared.b; }
}
static void bad (void)
{
    g_shared.a = 1;
    ++g_shared.count;
    g_shared.count++;
    g_shared.health[2] = 4;
    g_shared.faults.mask |= 2U;
    helper_locked();
}
'''


def self_test() -> None:
    """Seeded violations must all be found and the good body accepted."""
    text = mask(SELF_TEST)
    bodies = list(functions(text))
    names = [name for name, _, _ in bodies]
    assert names == ["helper_locked", "good", "bad"], names
    helpers = {"helper_locked"}
    found = {}
    for name, start, end in bodies[1:]:
        result, _ = scan(name, text[start:end + 1], start, SELF_TEST,
                         ("g_shared",), helpers)
        found[name] = result
    assert found["good"] == [], found["good"]
    assert len(found["bad"]) == 6, found["bad"]


def check(relative: str, rule: dict) -> dict:
    path = REPO / relative
    source = path.read_text(encoding="utf-8")
    text = mask(source)
    bodies = list(functions(text))
    helpers = {name for name, _, _ in bodies if name.endswith("_locked")}
    findings = []
    writers = 0
    for name, start, end in bodies:
        if name in rule["exempt"] or name.endswith("_locked"):
            continue
        found, _ = scan(name, text[start:end + 1], start, source,
                        rule["watched"], helpers)
        findings.extend(found)
        writers += 1
    return {"file": relative, "functions_checked": writers,
            "locked_helpers": sorted(helpers), "findings": findings}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, help="write a JSON report")
    args = parser.parse_args()
    self_test()
    print("Lock checker self-test with seeded violations: passed.")
    missing = [name for name in RULES if not (REPO / name).is_file()]
    if missing:
        # A moved file must not turn the check into a silent pass.
        print("Lock discipline: ERROR, missing " + ", ".join(missing))
        return 1
    reports = [check(name, rule) for name, rule in RULES.items()]
    total = sum(len(item["findings"]) for item in reports)
    for item in reports:
        print(f"{item['file']}: {item['functions_checked']} functions, "
              f"{len(item['findings'])} unlocked shared writes; "
              f"locked helpers: {', '.join(item['locked_helpers']) or '-'}")
        for finding in item["findings"]:
            print(f"  line {finding['line']}: {finding['function']}: "
                  f"{finding['detail']}")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(reports, indent=2) + "\n")
    print("Lock discipline: " + ("PASS" if total == 0 else "FINDINGS"))
    return 0 if total == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

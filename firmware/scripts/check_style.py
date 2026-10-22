#!/usr/bin/env python3
"""Check a documented subset of BARR-C-oriented project conventions.

This is a lexical checker, not a C compiler, MISRA tool, BARR certification,
concurrency proof or replacement for manual review. Vendor files are outside
scope. Each report includes hashes identifying the source snapshot checked.

Reports go to --report-dir, else $ROBOT_VALIDATION_DIR/style, else the
git-ignored validation/local/style; never into the dated evidence folders.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import sys
from pathlib import Path

VERSION = "1.0.1"
SOURCE_DIRS = ("app_program", "firmware/tests")
EXCLUDED_PARTS = {"vendor", "third_party", "third-party", "external"}
MANUAL_TOKENS = {
    "auto": "BARR-C 1.7: automatic storage keyword is prohibited",
    "register": "BARR-C 1.7: register keyword is prohibited",
    "short": "BARR-C 5.2: review explicit short type",
    "long": "BARR-C 5.2: review explicit long type or mandated ABI",
    "goto": "BARR-C 8.5: review control-flow exception",
    "continue": "BARR-C 8.5: review control-flow exception",
    "abort": "BARR-C 1.7: inspect termination function use",
    "exit": "BARR-C 1.7: inspect termination function use",
    "setjmp": "BARR-C 1.7: inspect nonlocal control transfer",
    "longjmp": "BARR-C 1.7: inspect nonlocal control transfer",
    "malloc": "Project allocation policy: inspect dynamic allocation",
    "calloc": "Project allocation policy: inspect dynamic allocation",
    "realloc": "Project allocation policy: inspect dynamic allocation",
    "free": "Project allocation policy: inspect dynamic allocation",
}


def mask_lexical(text: str, *, literals: bool = True) -> str:
    """Blank comments and optionally literals, preserving offsets/newlines."""
    output = list(text)
    index = 0
    while index < len(text):
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = len(text) if end < 0 else end
            for position in range(index, end):
                output[position] = " "
            index = end
        elif text.startswith("/*", index):
            close = text.find("*/", index + 2)
            end = len(text) if close < 0 else close + 2
            for position in range(index, end):
                if output[position] not in "\r\n":
                    output[position] = " "
            index = end
        elif text[index] in "\"'":
            quote = text[index]
            end = index + 1
            while end < len(text):
                if text[end] == "\\":
                    end += 2
                elif text[end] == quote:
                    end += 1
                    break
                else:
                    end += 1
            end = min(end, len(text))
            if literals:
                for position in range(index, end):
                    if output[position] not in "\r\n":
                        output[position] = " "
            index = end
        else:
            index += 1
    return "".join(output)


def mask_directives(text: str) -> str:
    """Blank preprocessor directives, including backslash continuations."""
    result = []
    continued = False
    for line in text.splitlines(keepends=True):
        directive = continued or line.lstrip().startswith("#")
        continued = directive and line.rstrip("\r\n").rstrip().endswith("\\")
        if directive:
            result.append("".join(char if char in "\r\n" else " "
                                  for char in line))
        else:
            result.append(line)
    return "".join(result)


def function_name(prefix: str) -> str | None:
    """Recognize ordinary C definitions, including callback parameters.

    Function-generating macros, K&R definitions and post-declarator attributes
    are outside this intentionally limited grammar and require manual review.
    """
    prefix = prefix.rstrip()
    if not prefix.endswith(")") or "=" in prefix:
        return None
    depth = 0
    opening = None
    for index in range(len(prefix) - 1, -1, -1):
        if prefix[index] == ")":
            depth += 1
        elif prefix[index] == "(":
            depth -= 1
            if depth == 0:
                opening = index
                break
    if opening is None:
        return None
    match = re.search(r"\b([A-Za-z_]\w*)\s*$", prefix[:opening])
    if match is None or match[1] in {"if", "for", "while", "switch"}:
        return None
    if not prefix[:match.start()].strip():
        return None
    return match[1]


def scan_functions(text: str) -> tuple[list[dict], list[int]]:
    """Count physical definition lines using balanced lexical braces."""
    masked = mask_directives(mask_lexical(text))
    functions = []
    unmatched = []
    depth = 0
    boundary = 0
    current = None
    for index, char in enumerate(masked):
        if char == "{":
            if depth == 0:
                prefix = masked[boundary:index]
                name = function_name(prefix)
                if name:
                    start = boundary + len(prefix) - len(prefix.lstrip())
                    current = {
                        "name": name,
                        "line": masked.count("\n", 0, start) + 1,
                    }
            depth += 1
        elif char == "}":
            if depth == 0:
                unmatched.append(masked.count("\n", 0, index) + 1)
            else:
                depth -= 1
                if depth == 0:
                    if current:
                        end = masked.count("\n", 0, index) + 1
                        current["end_line"] = end
                        current["physical_lines"] = end - current["line"] + 1
                        functions.append(current)
                        current = None
                    boundary = index + 1
        elif char == ";" and depth == 0:
            boundary = index + 1
    if depth:
        unmatched.append(masked.count("\n") + 1)
    return functions, unmatched


def self_test() -> None:
    """Exercise misleading comments, literals, macros and nested braces."""
    fixture = '''#define FAKE(x) \\
    { x; }
/* int fake (void) { return 0; } */
struct point { int value; };
static int actual (int (* callback)(int))
{
    const char * text = "not } /* a comment */";
    if (text[0] != '\\0') { return callback(1); }
    return 0;
}
int second (void) { return 0; }
'''
    functions, unmatched = scan_functions(fixture)
    assert [item["name"] for item in functions] == ["actual", "second"]
    assert [item["physical_lines"] for item in functions] == [6, 1]
    assert not unmatched
    assert scan_functions("int bad (void) {\n")[1]
    assert "goto" not in mask_lexical('/* goto */ "goto"')
    assert '"core.h"' in mask_lexical('#include "core.h"', literals=False)


def inspect_file(path: Path, root: Path, headers: dict[str, list[Path]]) -> dict:
    data = path.read_bytes()
    text = data.decode("utf-8")
    lines = text.splitlines()
    errors = []
    manual = []

    def issue(rule: str, line: int, detail: str) -> None:
        errors.append({"rule": rule, "line": line, "detail": detail})

    for number, line in enumerate(lines, 1):
        if len(line) > 80:
            issue("columns", number, f"{len(line)} columns exceeds 80")
        if "\t" in line:
            issue("tabs", number, "Tab character; use spaces")
    for match in re.finditer("\r", text):
        issue("line_endings", text.count("\n", 0, match.start()) + 1,
              "Carriage return; use LF line endings")

    eof = re.search(r"(?:/\*(?:(?!\*/)[\s\S])*?(?:end of|eof)"
                    r"(?:(?!\*/)[\s\S])*?\*/|//[^\n]*(?:end of|eof)[^\n]*)"
                    r"\n(?:[ \t]*\n)+\Z", text, re.IGNORECASE)
    if eof is None:
        issue("eof_marker", max(1, len(lines)),
              "Require a final end-of-file comment followed by a blank line")

    comments_masked = mask_lexical(text, literals=False)
    guard = None
    if path.suffix == ".c":
        expected = path.stem + ".h"
        includes = re.findall(r'^\s*#\s*include\s*"([^"\n]+)"',
                              comments_masked, re.MULTILINE)
        matching = [value for value in includes if Path(value).name == expected]
        if not matching:
            issue("matching_header_include", 1,
                  f'Source must include corresponding "{expected}"')
        if expected not in headers:
            issue("matching_header_exists", 1,
                  f"No corresponding project header {expected}")
    else:
        directive_matches = list(re.finditer(
            r"^[ \t]*#[ \t]*(\w+)[ \t]*([^\n]*)",
            comments_masked, re.MULTILINE))
        directives = [(match[1], match[2]) for match in directive_matches]
        if (len(directives) >= 3 and directives[0][0] == "ifndef"
                and directives[1][0] == "define"):
            guard = directives[0][1].strip()
            if (not re.fullmatch(r"[A-Z_][A-Z0-9_]*", guard)
                    or directives[1][1].strip() != guard
                    or directives[-1][0] != "endif"):
                issue("header_guard", 1,
                      "Guard must use matching uppercase ifndef/define/endif")
        else:
            issue("header_guard", 1,
                  "Header must begin with matching ifndef/define directives")
        if directives:
            depth = 0
            valid_nesting = True
            for index, (directive, unused) in enumerate(directives):
                if directive in {"if", "ifdef", "ifndef"}:
                    depth += 1
                elif directive == "endif":
                    depth -= 1
                    if depth < 0 or (depth == 0 and index != len(directives) - 1):
                        valid_nesting = False
            before = comments_masked[:directive_matches[0].start()].strip()
            after = comments_masked[directive_matches[-1].end():].strip()
            if depth or not valid_nesting or before or after:
                issue("header_guard_extent", 1,
                      "Guard must enclose the entire header with balanced ifs")

    functions, unmatched = scan_functions(text)
    for function in functions:
        if function["physical_lines"] > 100:
            issue("function_length", function["line"],
                  f'{function["name"]}: {function["physical_lines"]} lines '
                  "exceeds the selected 100-line gate")
    for line in unmatched:
        manual.append({"rule": "lexical_structure", "line": line,
                       "detail": "Unbalanced lexical braces; inspect source "
                                 "and conditional compilation manually"})
    lexical = mask_lexical(text)
    for match in re.finditer(r"\b[A-Za-z_]\w*\b", lexical):
        if match[0] in MANUAL_TOKENS:
            manual.append({"rule": "token_review",
                           "line": text.count("\n", 0, match.start()) + 1,
                           "token": match[0],
                           "detail": MANUAL_TOKENS[match[0]]})
    return {
        "file": path.relative_to(root).as_posix(),
        "sha256": hashlib.sha256(data).hexdigest(),
        "line_count": len(lines),
        "maximum_columns": max((len(line) for line in lines), default=0),
        "guard": guard,
        "functions": functions,
        "mechanical_findings": errors,
        "manual_review_findings": manual,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[2],
                        help="repository root (default: this repository)")
    parser.add_argument("--report-dir", type=Path,
                        help="default: $ROBOT_VALIDATION_DIR/style, else "
                             "<root>/firmware/validation/local/style")
    options = parser.parse_args()
    root = options.root.resolve()
    if options.report_dir is not None:
        destination = options.report_dir.resolve()
    elif os.environ.get("ROBOT_VALIDATION_DIR"):
        destination = (Path(os.environ["ROBOT_VALIDATION_DIR"])
                       / "style").resolve()
    else:
        destination = (root / "firmware" / "validation" / "local"
                       / "style").resolve()
    self_test()
    missing = [name for name in SOURCE_DIRS if not (root / name).is_dir()]
    if missing:
        print("ERROR: style scope missing: " + ", ".join(missing),
              file=sys.stderr)
        return 1
    files = sorted(path for name in SOURCE_DIRS
                   for path in (root / name).rglob("*")
                   if path.is_file() and path.suffix in {".c", ".h"}
                   and not EXCLUDED_PARTS.intersection(path.relative_to(root).parts))
    headers: dict[str, list[Path]] = {}
    for path in files:
        if path.suffix == ".h":
            headers.setdefault(path.name, []).append(path)
    records = [inspect_file(path, root, headers) for path in files]
    guards: dict[str, list[str]] = {}
    for record in records:
        if record["guard"]:
            guards.setdefault(record["guard"], []).append(record["file"])
    for record in records:
        if record["guard"] and len(guards[record["guard"]]) > 1:
            record["mechanical_findings"].append({
                "rule": "duplicate_guard", "line": 1,
                "detail": "Guard also occurs in " + ", ".join(
                    value for value in guards[record["guard"]]
                    if value != record["file"]),
            })
    errors = sum(len(item["mechanical_findings"]) for item in records)
    manual = sum(len(item["manual_review_findings"]) for item in records)
    report = {
        "checker": "check_style.py", "version": VERSION,
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "scope": [name + "/**/*.[ch]" for name in SOURCE_DIRS],
        "excluded_directory_names": sorted(EXCLUDED_PARTS),
        "status": "PASS_SELECTED_CHECKS" if files and errors == 0 else "FINDINGS",
        "file_count": len(files), "mechanical_finding_count": errors,
        "manual_review_finding_count": manual,
        "lexical_scanner_self_test": "passed",
        "limits": [
            "Selected lexical checks only; not full BARR-C compliance.",
            "Columns count Unicode code points; project C is expected ASCII.",
            "Function lengths include signature, braces, comments and blanks.",
            "Function macros, K&R syntax and trailing attributes need review.",
            "Preprocessor branches are scanned as written, not compiled.",
            "Token findings require manual review; they are not proven defects.",
            "No proof of ABI, timing, concurrency, hardware or functional safety.",
        ],
        "files": records,
    }
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "style-report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    output = [f"Selected project style checks: {report['status']}",
              f"Checker version: {VERSION}", f"UTC: {report['generated_utc']}",
              f"Files: {len(files)}; mechanical findings: {errors}; "
              f"manual-review findings: {manual}",
              "Scope: " + ", ".join(report["scope"]),
              "Lexical scanner regression fixture: passed.", ""]
    for record in records:
        output.append(f"{record['file']} | max columns "
                      f"{record['maximum_columns']} | "
                      f"{len(record['functions'])} definitions")
        for category in ("mechanical_findings", "manual_review_findings"):
            for finding in record[category]:
                output.append(f"  {category}: line {finding['line']}: "
                              f"{finding['rule']}: {finding['detail']}")
    output.extend(["", "Limits:", *["- " + value for value in report["limits"]]])
    (destination / "style-report.txt").write_text("\n".join(output) + "\n",
                                                 encoding="utf-8")
    print("\n".join(output[:7]))
    return 0 if files and errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

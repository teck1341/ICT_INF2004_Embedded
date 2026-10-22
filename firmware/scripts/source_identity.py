#!/usr/bin/env python3
"""Record the identity of the sources a target build compiles.

build_target.sh runs this before and after 'make' and refuses to collect the
image if anything changed in between. It hashes every source file in the
project-template folders (build products excluded) and records the git and
Pico SDK revisions. The local network settings are recorded as present or
absent only, never hashed, so no credential-derived value reaches evidence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

SOURCE_DIRS = ("app_program", "build_make", "config", "device", "etc",
               "include", "kernel", "lib", "tools/elf2uf2")
BUILD_SUFFIXES = (".o", ".d", ".su", ".elf", ".uf2", ".map")
LOCAL_SETTINGS = ("config/wifi_credentials.h", "config/net_mqtt_config.h",
                  "config/wifi_secrets.h")


def git(folder: Path, *arguments: str) -> str | None:
    """Read-only git query; None when the folder is not a git checkout."""
    try:
        return subprocess.check_output(
            ["git", "-C", str(folder), *arguments], text=True,
            stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def is_build_product(relative: str, path: Path) -> bool:
    """Objects, images, profile marker and the host-built elf2uf2 tool."""
    return (path.suffix in BUILD_SUFFIXES or path.name == ".build_profile"
            or relative in LOCAL_SETTINGS
            or (path.name == "elf2uf2" and path.parent.name == "tools"))


def identity(repo: Path, sdk: Path, profile: str) -> dict:
    missing = [name for name in SOURCE_DIRS if not (repo / name).is_dir()]
    if missing:
        raise ValueError("Not a project-template checkout; missing "
                         + ", ".join(missing))
    sources = {}
    for name in SOURCE_DIRS:
        for path in sorted((repo / name).rglob("*")):
            relative = path.relative_to(repo).as_posix()
            if path.is_file() and not is_build_product(relative, path):
                sources[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {
        "status": "passed",
        "profile": profile,
        "git_revision": git(repo, "rev-parse", "HEAD"),
        "pico_sdk_revision": git(sdk, "rev-parse", "HEAD"),
        "pico_sdk_tracked_changes": bool(git(sdk, "diff", "--name-only", "HEAD")),
        "local_settings_present": {name: (repo / name).is_file()
                                   for name in LOCAL_SETTINGS},
        "source_sha256": sources,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = identity(args.repo.resolve(), args.sdk.resolve(), args.profile)
    except (OSError, ValueError) as error:
        report = {"status": "failed", "profile": args.profile,
                  "error": str(error)}
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    if report["status"] != "passed":
        print("Source identity: " + report["error"], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

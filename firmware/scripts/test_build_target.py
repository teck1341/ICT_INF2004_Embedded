#!/usr/bin/env python3
"""Exercise build failure evidence with disposable command fixtures.

No ARM compiler, firmware image, or hardware is used by these tests.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class BuildFailureEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="robot-build-evidence-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.scripts = self.root / "scripts"
        self.scripts.mkdir()
        shutil.copy2(Path(__file__).with_name("build_target.sh"), self.scripts)
        self.repo = self.root / "project"
        self.sdk = self.root / "sdk"
        self.repo.mkdir()
        self.sdk.mkdir()
        self.binaries = self.root / "bin"
        self.binaries.mkdir()
        # Explicit PATH prevents a machine's real ARM compiler being invoked.
        for name in ("bash", "dirname", "mkdir", "rm", "date", "tee",
                     "python3", "git", "make", "g++"):
            self.binaries.joinpath(name).symlink_to(shutil.which(name))
        self.environment = dict(os.environ, PATH=str(self.binaries))
        self.scripts.joinpath("source_identity.py").write_text('''
import json, os, pathlib, sys
failed = os.environ.get('ROBOT_TEST_PREFLIGHT') == 'fail'
repo = pathlib.Path(sys.argv[sys.argv.index('--repo') + 1])
failed = failed or repo.joinpath('source-drift').exists()
path = pathlib.Path(sys.argv[sys.argv.index('--output') + 1])
generation = repo.joinpath('source-generation')
path.write_text(json.dumps({'status': 'failed' if failed else 'passed',
    'source_generation': generation.read_text() if generation.exists() else 'initial'}))
raise SystemExit(1 if failed else 0)
''')
        self.evidence = self.repo / "robot_build_evidence" / "offline"
        self.evidence.mkdir(parents=True)
        self.stale_names = (
            "image-report.json", "toolchain.json", "stack-usage.json",
            "mtk3pico_smp0_uart.elf", "mtk3pico_smp0_uart.uf2",
            "mtk3pico_smp0_uart.map",
        )
        for name in self.stale_names:
            self.evidence.joinpath(name).write_text("stale fixture")
        self.evidence.joinpath("postflight.json").write_text("stale fixture")

    def run_build(self):
        result = subprocess.run(
            [str(self.binaries / "bash"), str(self.scripts / "build_target.sh"),
             str(self.repo), str(self.sdk), "offline"],
            env=self.environment, text=True, capture_output=True, check=False)
        return result, json.loads(self.evidence.joinpath("build-status.json").read_text())

    def assert_no_image(self):
        for name in self.stale_names:
            if name != "toolchain.json":
                self.assertFalse(self.evidence.joinpath(name).exists(), name)

    def fixture_tool(self, name, source):
        path = self.binaries / name
        path.unlink(missing_ok=True)
        path.write_text("#!/usr/bin/env bash\n" + source)
        path.chmod(0o755)

    def test_missing_compiler_preserves_source_evidence(self):
        result, status = self.run_build()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(status["status"], "blocked")
        self.assertEqual(status["stage"], "toolchain")
        self.assertIn("Required tool unavailable: arm-none-eabi-gcc", result.stdout)
        self.assertEqual(json.loads(self.evidence.joinpath("preflight.json").read_text())
                         ["status"], "passed")
        self.assert_no_image()
        self.assertFalse(self.evidence.joinpath("toolchain.json").exists())
        self.assertFalse(self.evidence.joinpath("postflight.json").exists())

    def test_missing_git_is_a_toolchain_block_before_source_check(self):
        self.binaries.joinpath("git").unlink()
        result, status = self.run_build()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(status["status"], "blocked")
        self.assertEqual(status["stage"], "toolchain")
        self.assertIn("Required tool unavailable: git", result.stdout)
        self.assertFalse(self.evidence.joinpath("preflight.json").exists())
        self.assertFalse(self.evidence.joinpath("postflight.json").exists())
        self.assert_no_image()

    def test_failed_source_gate_is_not_a_compiler_block(self):
        self.environment["ROBOT_TEST_PREFLIGHT"] = "fail"
        result, status = self.run_build()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(status["status"], "failed")
        self.assertEqual(status["stage"], "source_integrity")
        self.assert_no_image()

    def test_compile_failure_preserves_exit_code(self):
        build = self.repo / "build_make"
        build.mkdir()
        leftovers = ("stale.o", "stale.d", "stale.su", "mtk3pico_smp0_uart.elf",
                     "mtk3pico_smp0_uart.uf2", "mtk3pico_smp0_uart.map")
        for name in leftovers:
            build.joinpath(name).write_text("old build product")
        for name in ("arm-none-eabi-gcc", "arm-none-eabi-size"):
            self.fixture_tool(name, "printf 'DISPOSABLE TEST TOOL ONLY\\n'\n")
        self.fixture_tool("make", '''
if [[ "$1" == --version ]]; then
    printf 'DISPOSABLE TEST MAKE ONLY\\n'
    exit 0
fi
if [[ "${@: -1}" == clean ]]; then exit 0; fi
exit 7
''')
        result, status = self.run_build()
        self.assertEqual(result.returncode, 7)
        self.assertEqual(status["exit_code"], 7)
        self.assertEqual(status["status"], "failed")
        self.assertEqual(status["stage"], "compile")
        self.assert_no_image()
        for name in leftovers:
            self.assertFalse(build.joinpath(name).exists(), name)
        # A compiler that is not the pinned one is recorded, not hidden.
        toolchain = json.loads(self.evidence.joinpath("toolchain.json").read_text())
        self.assertIs(toolchain["toolchain_matches_pin"], False)
        self.assertIs(status["toolchain_matches_pin"], False)
        self.assertIn("WARNING: arm-none-eabi-gcc is not the pinned", result.stdout)

    def test_pinned_compiler_banner_is_recorded_as_matching(self):
        self.fixture_tool("arm-none-eabi-gcc", "printf 'arm-none-eabi-gcc "
                          "(Arm GNU Toolchain 14.2.Rel1 (Build arm-14.52)) "
                          "14.2.1 20241119 DISPOSABLE TEST TOOL\\n'\n")
        self.fixture_tool("arm-none-eabi-size", "printf 'DISPOSABLE TEST TOOL ONLY\\n'\n")
        self.fixture_tool("make", '''
if [[ "$1" == --version ]]; then
    printf 'DISPOSABLE TEST MAKE ONLY\\n'
    exit 0
fi
if [[ "${@: -1}" == clean ]]; then exit 0; fi
exit 7
''')
        self.repo.joinpath("build_make").mkdir()
        result, status = self.run_build()
        self.assertEqual(result.returncode, 7)
        toolchain = json.loads(self.evidence.joinpath("toolchain.json").read_text())
        self.assertEqual(toolchain["pinned_arm_toolchain"], "Arm GNU Toolchain 14.2.Rel1")
        self.assertIs(toolchain["toolchain_matches_pin"], True)
        self.assertIs(status["toolchain_matches_pin"], True)
        self.assertNotIn("WARNING: arm-none-eabi-gcc is not the pinned", result.stdout)

    def test_missing_compiler_records_no_toolchain_match(self):
        _, status = self.run_build()
        self.assertIsNone(status["toolchain_matches_pin"])

    def test_pin_matches_setup_script(self):
        scripts = Path(__file__).resolve().parent
        build = scripts.joinpath("build_target.sh").read_text()
        setup = scripts.parents[1].joinpath("scripts/setup.sh").read_text()
        pin = re.search(r'^robot_toolchain_pin="([^"]+)"', build, re.M)[1]
        banner = re.search(r'^TOOLCHAIN_BANNER="([^"]+)"', setup, re.M)[1]
        version = re.search(r'^TOOLCHAIN_VERSION="([^"]+)"', setup, re.M)[1]
        self.assertEqual(pin, banner)
        self.assertEqual(pin.lower(), version.lower())

    def test_source_drift_during_make_blocks_image_collection(self):
        for name in ("arm-none-eabi-gcc", "arm-none-eabi-size"):
            self.fixture_tool(name, "printf 'DISPOSABLE TEST TOOL ONLY\\n'\n")
        self.fixture_tool("make", '''
if [[ "$1" == --version ]]; then
    printf 'DISPOSABLE TEST MAKE ONLY\\n'
    exit 0
fi
if [[ "${@: -1}" == clean ]]; then exit 0; fi
printf 'changed during simulated compilation\\n' > "$2/../source-drift"
exit 0
''')
        self.repo.joinpath("build_make").mkdir()
        result, status = self.run_build()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(status["status"], "failed")
        self.assertEqual(status["stage"], "postbuild_source_integrity")
        self.assertEqual(json.loads(self.evidence.joinpath("preflight.json").read_text())
                         ["status"], "passed")
        self.assertEqual(json.loads(self.evidence.joinpath("postflight.json").read_text())
                         ["status"], "failed")
        self.assert_no_image()

    def test_regenerated_source_identity_must_match_initial_preflight(self):
        for name in ("arm-none-eabi-gcc", "arm-none-eabi-size"):
            self.fixture_tool(name, "printf 'DISPOSABLE TEST TOOL ONLY\\n'\n")
        self.fixture_tool("make", '''
if [[ "$1" == --version ]]; then
    printf 'DISPOSABLE TEST MAKE ONLY\\n'
    exit 0
fi
if [[ "${@: -1}" == clean ]]; then exit 0; fi
printf 'changed and regenerated source\\n' > "$2/../source-generation"
exit 0
''')
        self.repo.joinpath("build_make").mkdir()
        result, status = self.run_build()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(status["status"], "failed")
        self.assertEqual(status["stage"], "postbuild_source_integrity")
        for name in ("preflight.json", "postflight.json"):
            self.assertEqual(json.loads(self.evidence.joinpath(name).read_text())
                             ["status"], "passed")
        self.assertIn("Source identity changed during compilation", result.stdout)
        self.assert_no_image()


if __name__ == "__main__":
    unittest.main()

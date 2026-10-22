"""Where the robot application lives in the course project-template layout.

The application is in app_program/: code shared by the whole team at the
top (app_main.c, board.c, robot_app.c, ...) and one folder per subsystem
(subsystem1..5, one per buddy). Every source file name is unique across
those folders, so tools find a file by its name with app_file().
"""
from __future__ import annotations

from pathlib import Path
import shutil

REPO = Path(__file__).resolve().parents[2]
# The buildable project-template tree (kernel, drivers, build files, app).
PROJECT_DIRS = ("app_program", "build_make", "config", "device", "etc",
                "include", "kernel", "lib", "tools")
_SKIPPED_SUFFIXES = (".o", ".d", ".su", ".elf", ".uf2", ".map")
_SKIPPED_NAMES = {".build_profile", "wifi_credentials.h", "net_mqtt_config.h",
                  "__pycache__"}
FIRMWARE = REPO / "firmware"
APP = REPO / "app_program"
APP_DIRS = (APP, APP / "subsystem1", APP / "subsystem2", APP / "subsystem3",
            APP / "subsystem4", APP / "subsystem5")

# Hardware-independent modules: compiled into the host tests.
PORTABLE_SOURCES = tuple(APP / name for name in (
    "bitpack.c", "core.c",
    "subsystem1/mqttcmd.c", "subsystem1/telemetry.c",
    "subsystem2/motion.c",
    "subsystem3/barcode.c", "subsystem3/line_ctrl.c",
    "subsystem4/terrain.c",
    "subsystem5/scanner.c", "subsystem5/vehicle.c",
))
PORTABLE_HEADERS = tuple(path.with_suffix(".h") for path in PORTABLE_SOURCES)

# Board, kernel and radio adapters: built only for the target.
TARGET_SOURCES = tuple(APP / name for name in (
    "app_main.c", "app_cfg.c", "board.c", "robot_app.c",
    "subsystem1/net_mqtt.c", "subsystem1/net_mqtt_ipc.c",
    "subsystem3/course_cfg.c",
))
TARGET_HEADERS = tuple(APP / name for name in (
    "app_main.h", "app_cfg.h", "board.h", "robot_app.h", "robot_config.h",
    "subsystem1/net_mqtt.h", "subsystem1/net_mqtt_ipc.h",
    "subsystem3/course_cfg.h",
))


def app_file(name: str, app: Path = APP) -> Path:
    """Return the one application file called name (e.g. 'board.c')."""
    matches = [folder / name for folder in app_dirs(app)
               if (folder / name).is_file()]
    if len(matches) != 1:
        raise FileNotFoundError(
            f"expected exactly one {name} under {app}, found {len(matches)}")
    return matches[0]


def app_dirs(app: Path = APP) -> tuple[Path, ...]:
    """The application folders, for an app_program/ at any location."""
    return tuple(app / folder.relative_to(APP) for folder in APP_DIRS)


def include_flags(app: Path = APP) -> list[str]:
    """-I flags for every application folder."""
    return ["-I" + str(folder) for folder in app_dirs(app)]


def copy_project(destination: Path, repository: Path = REPO) -> Path:
    """Copy the buildable tree without build products or local settings.

    Fixtures run 'make -n' in the copy: the template makefile records the
    build profile and clears stale objects while it parses, which must not
    touch the real build_make/.
    """
    def skipped(folder: str, names: list[str]) -> set[str]:
        return {name for name in names
                if name in _SKIPPED_NAMES or name.endswith(_SKIPPED_SUFFIXES)
                or (name == "elf2uf2" and (Path(folder) / name).is_file())}

    destination.mkdir(parents=True, exist_ok=True)
    for name in PROJECT_DIRS:
        shutil.copytree(repository / name, destination / name, ignore=skipped)
    return destination

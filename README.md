# INF2004 Embedded Systems Project — IS29

This repository contains the robot firmware, Week 6 design report,
Pre-Tribunal document, tribunal briefing materials and assignment references
for Team IS29.

The project is an autonomous robot built around the Raspberry Pi Pico W and
Robo Pico board. Its intended mission is to follow a line, read navigation
barcodes, make controlled turns, estimate hump height, detect and bypass
obstacles, recover the track and report its state over Wi-Fi/MQTT. The firmware
is written in C using the Pico SDK and the supplied micro T-Kernel RP2040 port.

## Project documents

| Material | File |
|---|---|
| Week 6 design report | [Word document](Week6_Report/INF2004_Week6_Design_Review.docx) · [PDF](Week6_Report/INF2004_Week6_Design_Review.pdf) |
| Tribunal 1 | [Pre-Tribunal](Tribunal1/IS29_Pre-Tribunal1.docx) · [Immediately after](Tribunal1/IS29_Imm-Tribunal1.docx) · [Post-Tribunal](Tribunal1/IS29_Post-Tribunal1.docx) |
| Change requirement 1 | [Minimum-bit data representation](<References/Change Requirement.docx>) · [Week 13 evidence](ChangeRequirement/README.md) |
| Week 6 report feedback | [Original lecturer feedback](<Tribunal1/Sources/IS29_feedback for our week 6 report.txt>) |
| Tribunal briefing | [Notice](Tribunal1/Sources/tribunal.txt) · [Portal screenshot](<Tribunal1/Sources/Screenshot 2026-10-15 221220.png>) |
| Official tribunal template | [Week8_Tribunal1_Template.docx](Tribunal1/Sources/Week8_Tribunal1_Template.docx) |
| Assignment requirements | [Project Write-up](<References/Project Write-up.pdf>) · [Project Briefing](<References/Project Briefing.pdf>) |
| Module and assessment overview | [W0 Module Overview](<References/W0_Module Overview.pdf>) |
| Track and barcode requirements | [Barcode & Line Specification](<References/Barcode & Line Specification.pdf>) · [Barcode Sample](<References/Barcode Sample.pdf>) |
| Reference index | [Project sources](References/README.md) · [Hardware and device guides](References/Device_Guides/README.md) |

The official tribunal template includes the assessment stages. Tribunal 1
has three submissions: the Pre-Tribunal (Version A), the document written
immediately after the live session (Version B), and the Post-Tribunal, which
repeats A and B and adds the AI-vetted gap check (Version C), the final
summary and the implementation evidence.

## Repository layout

The repository follows the course project template
([sirfonzie/INF2004_ProjectTemplate](https://github.com/sirfonzie/INF2004_ProjectTemplate)):
micro T-Kernel 3.0 for the Pico W, its drivers and build files are committed
here, and the robot application is in `app_program/`.

| Location | Contents |
|---|---|
| [app_program/](app_program/) | The robot application. Shared code at the top: `app_main.c` (tasks), `robot_app.c` (task steps), `board.c` (hardware), `robot_config.h` (pins and calibration), `app_cfg.c`, `core.c`, `bitpack.c`. |
| [app_program/subsystem1/](app_program/subsystem1/) | Buddy 1, communications: MQTT client (`net_mqtt*.c`), telemetry frames, command codec. |
| [app_program/subsystem2/](app_program/subsystem2/) | Buddy 2, motion: motion requests, PID, odometry (`motion.c`). |
| [app_program/subsystem3/](app_program/subsystem3/) | Buddy 3, optical: line controller, barcode decoder, course codebook. |
| [app_program/subsystem4/](app_program/subsystem4/) | Buddy 4, terrain: tilt, motion class, hump height (`terrain.c`). |
| [app_program/subsystem5/](app_program/subsystem5/) | Buddy 5, obstacles: scanner and Vehicle mission state machine. |
| [build_make/](build_make/), [config/](config/), [kernel/](kernel/), [lib/](lib/), [device/](device/), [include/](include/), [etc/](etc/), [tools/](tools/) | The project template: kernel, drivers, build files and kernel configuration. `build_make/robot_project.mk` adds the robot's include paths and SDK drivers. |
| [testing/](testing/) and [videos/](videos/) | Test logs per subsystem, integration and system tests, and the video index, in the template's format. |
| [TESTING.md](TESTING.md) | Steps to test the code, set up the car and run each buddy's test cases. |
| [TEAM.md](TEAM.md) | Team, owners, pins, tasks and interfaces. |
| [firmware/tests/](firmware/tests/) and [firmware/scripts/](firmware/scripts/) | Host tests, application scenarios, integration fixtures, build and measurement scripts. |
| [scripts/](scripts/) | Dependency setup and the development command runner. |
| [tools/](tools/) | The template's UF2 converter, plus the barcode sampling-budget tool and report-maintenance scripts. |
| [Week6_Report/](Week6_Report/) | Current report in Word and PDF. |
| [Tribunal1/](Tribunal1/) | Pre-Tribunal, original briefing materials, template and feedback. |
| [References/](References/) | Assignment specifications, lecture transcript, templates and hardware references. |
| [firmware/third_party_notices/](firmware/third_party_notices/) | Dependency licence notices. |

## Development setup

Use Ubuntu 24.04 or Ubuntu under WSL on Windows. Run the commands below in the
Ubuntu terminal from the repository root. The scripts do not support Git Bash.
Python 3.10 or later is required by the project tools.

Clone the repository if you do not already have a local copy:

```sh
git clone https://github.com/teck1341/ICT_INF2004_Embedded.git
cd ICT_INF2004_Embedded
```

Install the host tools and set up the project:

```sh
sudo apt-get update
sudo apt-get install -y build-essential python3 git xz-utils curl ca-certificates
bash scripts/setup.sh
```

Setup downloads Pico SDK 2.2.0 (with its lwIP and CYW43 submodules) and Arm
GNU Toolchain 14.2.Rel1. The kernel and drivers are already in the
repository. Internet access is required. The default dependency directory is
`~/inf2004-deps`; keep it on the Linux filesystem without spaces.

For a different dependency directory, set `ROBOT_DEPS_DIR` before running both
setup and development commands. Using `setup.sh --deps` alone does not configure
later `dev.sh` commands.

Edit the robot code in `app_program/`. The firmware builds directly from this
repository with the template's makefile in `build_make/`; `scripts/dev.sh build`
runs it with the robot's profile (single core, UART console).

## Run checks and build

Run these commands after setup, from the repository root:

| Command | Purpose |
|---|---|
| `bash scripts/dev.sh host-tests` | Portable module tests in release and sanitizer configurations, plus the Python telemetry decoder's golden vectors. |
| `bash scripts/dev.sh style` | Selected BARR-C coding checks and the lock-discipline check for shared packed data. |
| `bash scripts/dev.sh app-tests` | Application scenarios using simulated inputs. |
| `bash scripts/dev.sh integration` | Board, image, build-script, radio and network integration fixtures. |
| `bash scripts/dev.sh build offline` | Build the offline firmware profile. |
| `bash scripts/dev.sh build mqtt --compile-only` | Build the MQTT profile with placeholder network settings. |

Telemetry and commands on MQTT are compact binary frames (see
[ChangeRequirement/README.md](ChangeRequirement/README.md)). Decode them on the
laptop with `mosquitto_sub -t <telemetry topic> -F %x | python3
firmware/scripts/telemetry_codec.py decode -`, and send a command byte with
`python3 firmware/scripts/telemetry_codec.py command STOP --raw |
mosquitto_pub -t <command topic> -s`.

Local check records are written under `firmware/validation/local/`; target build
outputs are under `build-target/offline/` and `build-target/mqtt/`. These generated
locations are ignored by Git. This repository currently supplies local checks;
it does not include a GitHub Actions workflow.

## Configuration and implementation status

Hardware settings are in [robot_config.h](app_program/robot_config.h), and
the course barcode configuration is in [course_cfg.c](app_program/subsystem3/course_cfg.c).
The supplied configuration has all eight hardware/course confirmation flags set
to `0`, and the production barcode codebook is empty. Startup and motion remain
inhibited until the required configuration and commissioning gates are satisfied.
The configured physical values are hypotheses that still require measurement.

Host tests use simulated inputs. A successful target build establishes
compilation and linking; it does not establish physical robot operation,
calibration, live network performance or course completion.

`config/wifi_credentials.example.h` and `config/net_mqtt_config.example.h` are
configuration examples. For a real MQTT build, copy them to
`config/wifi_credentials.h` and `config/net_mqtt_config.h` and configure the
copies. These filenames are ignored by Git. The `--compile-only`
profile uses placeholders and cannot connect to a network. Keep real credentials
and firmware images built with them out of the repository.

## Working with the report

The current editable report is
[Week6_Report/INF2004_Week6_Design_Review.docx](Week6_Report/INF2004_Week6_Design_Review.docx).
Its PDF is included for reading.

The report-generation scripts in `tools/` retain the original
`Part1_Week6_Report/` paths and expect report data and figure assets that are not
included in this simplified repository. They are not required to build the
robot firmware and are not a working regeneration path for this copy. Maintain
the supplied Word document directly, or restore the generator inputs and update
its paths before using those scripts.

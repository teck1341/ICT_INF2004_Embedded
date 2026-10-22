# Team and subsystems

Team IS29. Fill this in at the start of the project and keep it up to date.
It is the single place that says **who owns what**, so that five people can
work in one firmware image without clashing over pins, priorities, or kernel
objects. Pin and calibration values themselves live in
`app_program/robot_config.h`; this file records ownership.

A **subsystem** is one student's part of the product, with its own folder.
It may contain several tasks and own several peripherals (see section 3).

Rule: **only the owner edits a subsystem's folders.** To change another
subsystem, ask its owner or open a pull request for them to review.

## 1. Members and subsystems

| # | Subsystem (what it does) | Student ID | GitHub username | Code | Tests |
|---|---|---|---|---|---|
| 1 | Communications: Wi-Fi, MQTT client, telemetry frames, commands (Subque) | 2500599 | | `app_program/subsystem1/` | `testing/subsystem1/` |
| 2 | Motion: motors, encoders, PID speed control, odometry (Ong Teck Ang) | 2500738 | | `app_program/subsystem2/` | `testing/subsystem2/` |
| 3 | Optical: IR line following, barcode decoding, junction navigation (Tan Yi Jian Ethan) | 2501528 | | `app_program/subsystem3/` | `testing/subsystem3/` |
| 4 | Terrain: accelerometer tilt, motion class, hump height (Dian Ze) | 2500916 | | `app_program/subsystem4/` | `testing/subsystem4/` |
| 5 | Obstacles: sonar scan, bypass, line recovery; Safety and Vehicle supervision, integration (Siang Hong) | 2503147 | | `app_program/subsystem5/` | `testing/subsystem5/` |

Shared files are changed only by agreement of the team; Buddy 5 (integration)
coordinates them. They are this file and, in `app_program/`:

| File | What it is |
|---|---|
| `app_main.c/.h` | `usermain()`: creates the eight application tasks; the interrupt and lock helpers |
| `robot_app.c/.h` | One step function per task and the shared record they exchange data through |
| `board.c/.h` | Every hardware register access: motors, encoders, sonar, servo, ADC, I2C, buttons, watchdog |
| `robot_config.h` | Pins, calibration values and the confirmation flags |
| `app_cfg.c/.h`, `core.c/.h`, `bitpack.c/.h` | Configuration mapping, shared types, bit packing |

The exception is claiming an empty row in this file (a pin, a peripheral, an
interface ID), which anyone may do (section 2).

## 2. Pins

One owner per pin. Claim a pin here **before** you wire or code it.

**Why:** two subsystems using the same pin is one of the most common and
hardest-to-find mistakes in a team project. The code builds without any
error, but the hardware misbehaves. Writing the claim down first means a
clash is spotted when someone edits this table, not hours later on the bench.

| Pin | Owner | Used for |
|---|---|---|
| GP23, GP24, GP25, GP29 | *reserved* | Pico W radio |
| GP0 | S5 | HC-SR04 TRIG (Grove 1). Also UART0 TX: this robot builds with `CONSOLE=uart`, so the console has GP0/GP1 until `board_init()` and again only to print a start-up refusal |
| GP1 | S5 | HC-SR04 ECHO through the voltage divider (Grove 1). Also UART0 RX (see GP0) |
| GP2 | S2 | Left wheel encoder A (Grove 2; must be the M1 wheel) |
| GP3 | S2 | Left wheel encoder B (Grove 2) |
| GP4 | S4 | Accelerometer SDA, I2C0 (Grove 3) |
| GP5 | S4 | Accelerometer SCL, I2C0 (Grove 3) |
| GP6 | S3 | IR left D0 (Grove 5); wired, not read |
| GP7 | S3 | IR barcode D0 (Grove 7); wired, not read |
| GP8 | S2 | Left motor M1A, PWM slice 4 |
| GP9 | S2 | Left motor M1B, PWM slice 4 |
| GP10 | S2 | Right motor M2A, PWM slice 5 |
| GP11 | S2 | Right motor M2B, PWM slice 5 |
| GP12 | S5 | Scan servo, servo port 1, PWM slice 6 |
| GP13 | S3 | IR right D0, moved from Grove 6 to servo port 2; wired, not read |
| GP14 | | Free (servo port 3, kept for servos) |
| GP15 | | Free (servo port 4, kept for servos) |
| GP16 | S2 | Right wheel encoder A (Grove 4) |
| GP17 | S2 | Right wheel encoder B (Grove 4) |
| GP18 | | Free (on-board NeoPixel) |
| GP19 | | Free |
| GP20 | S5 | STOP button (on board, pull-up, active low) |
| GP21 | S5 | START / RESET button (on board, pull-up, active low) |
| GP22 | | Free (on-board buzzer) |
| GP26 | S3 | IR left A0, ADC0 (Grove 5) |
| GP27 | S3 | IR right A0, ADC1 (Grove 6) |
| GP28 | S3 | IR barcode A0, ADC2 (Grove 7). VBAT sense jumper must stay open |

## 3. Peripherals

**Subsystems, tasks and peripherals are three different things:**

| Term | What it is | Example |
|---|---|---|
| **Subsystem** | one student's part of the product, one folder | "motion", "optical" |
| **Task** | one thread of execution. A subsystem may have **as many tasks as it needs** | Safety and Vehicle tasks, both in subsystem 5 |
| **Peripheral** | one hardware block on the RP2040 | I2C0, the ADC, a PWM slice |

**The rule: each peripheral is driven by exactly one task, its *owning
task*.** Every other task asks the owning task through the shared record or a
kernel object (section 5) and never touches that peripheral itself. All
register access is in the shared `board.c`; the table says which task may
call each part of it.

| Peripheral | Owner subsystem | Owning task | Pins |
|---|---|---|---|
| UART0 | *kernel* | console (start-up and start-up refusal only) | GP0, GP1 |
| UART1 | | | |
| I2C0 | S4 | IMU task | GP4, GP5 |
| I2C1 | | | |
| SPI0 | | | |
| SPI1 | | | |
| ADC | S3 | Line task | GP26–GP28 |
| PWM slices 4, 5 | S2 | Motion task (any fault latch may only brake) | GP8–GP11 |
| PWM slice 6 | S5 | Scan task | GP12 |
| GPIO interrupt (IO_BANK0) | S2 + S5 | GPIO ISR in `board.c`: counts encoder edges (S2), times ECHO (S5) | GP2, GP3, GP16, GP17, GP1 |
| Watchdog | S5 | Safety task | |
| WiFi radio, TCP/IP, MQTT | *kernel* + S1 | `cyw43_utk_task` (built in), which also runs the robot MQTT client `net_mqtt_poll()` | GP23–25, GP29 |

### WiFi and MQTT are special

The radio and the network stack belong to the kernel's built-in WiFi service
task, `cyw43_utk_task` (`lib/libwifi/sysdepend/pico_rp2040/`). **No
application task calls `cyw43_...`, lwIP or MQTT functions.**

Subsystem 1's MQTT client (`app_program/subsystem1/net_mqtt.c`) is code that
*runs inside the service task*, as section 3 of the template describes: the
service task calls `net_mqtt_poll()` after `sys_check_timeouts()`, and
application tasks exchange data with it only through three message buffers
read with `TMO_POL` (interface IF-08). It replaces the template's
`mqtt_utk.c` heartbeat demo: build with `NET=lwip ROBOT_MQTT=1` (not
`MQTT=1`); `build_make/pico_rp2040.mk` refuses both together. The hook is
the `ROBOT_MQTT` blocks in `cyw43_utk.c`, `cyw43_utk_kernel.c`, `devinit.c`
and `lwipopts.h`, each marked `INF2004`. *To agree with the instructor.*

## 4. Task priorities

1 is the highest. The template gives the kernel 1–4 (USB console 3, WiFi 4)
and each subsystem a band from 12. **This robot deviates on purpose,** as in
the Week 6 design: the 2 ms Safety task must pre-empt everything, including
the radio. The build has no USB console task (`CONSOLE=uart`), and with
`ROBOT_MQTT=1` the WiFi service task moves from 4 to 9, below every control
task, so radio work can never delay STOP or the motor loop. Remote STOP is
therefore best effort; the GP20 button is the guaranteed stop.

| Subsystem | Tasks (name – priority – period – core) |
|---|---|
| 5 | Safety – 1 – 2 ms – core 0 |
| 2 | Motion – 2 – 5 ms – core 0 |
| 3 | Line – 3 – 2 ms – core 0 |
| 5 | Vehicle – 4 – 10 ms – core 0 |
| 4 | IMU – 5 – 10 ms – core 0 |
| 5 | Scan – 6 – 2 ms – core 0 |
| 1 | Comm – 7 – 5 ms – core 0 |
| 1 | Telemetry – 8 – 200 ms – core 0 |
| *kernel* + 1 | `cyw43_utk_task` – 9 – 2 ms poll – core 0 |

Single core: the robot builds with `SMP=0` (core 0 only); the shared record
relies on core-0 interrupt masking. Every task has a 4 KiB stack; the radio
task has 8 KiB.

## 5. Interfaces between subsystems

Tasks exchange data through one shared record, `g_shared` in `robot_app.c`,
copied in and out under `app_main_lock()` (a short interrupt-masked
section), with **one writing task per field**; `firmware/scripts/check_locks.py`
fails the build check on any unlocked write. The radio uses kernel message
buffers.

| ID | From → To | Mechanism | Object ID name | Data / meaning | Agreed by |
|---|---|---|---|---|---|
| IF-01 | S5 Vehicle → S2 Motion | shared record | `g_shared.vehicle.request`, `request_version` | leased drive, distance and turn requests | Week 6 design |
| IF-02 | S2 Motion → S3, S4, S5 | shared record | `g_shared.motion` | pose, wheel speeds, completion, stationary flag | Week 6 design |
| IF-03 | S3 Line → S5 Vehicle | shared record | `g_shared.line` | line status, steering, junction edge, barcode action | Week 6 design |
| IF-04 | S4 IMU → S5 Safety, Vehicle; S1 Telemetry | shared record | `g_shared.terrain` | tilt, motion class, hump height and validity | Week 6 design |
| IF-05 | S5 Scan → S5 Vehicle | shared record | `g_shared.front`, `g_shared.scan` | front range, scan profile, completion counters | Week 6 design |
| IF-06 | GPIO ISR → S2 Motion, S5 Scan | packed words under `app_main_lock()` | encoder word, echo word in `board.c` | encoder deltas and totals; ECHO pulse times | Tribunal 1 |
| IF-07 | all tasks → S5 Safety | locked helpers | `g_shared.faults`, `g_shared.health` | 16-bit fault mask, first cause, stop source; liveness stamps for the watchdog | Tribunal 1 |
| IF-08 | S1 Comm, Telemetry ↔ `cyw43_utk_task` | 3 message buffers, `TMO_POL` | RX, STOP and TX buffers in `net_mqtt_ipc.c` | 1-byte commands in, STOP first; telemetry frames of at most 20 bytes out | Tribunal 1 |

Each interface is tested by at least one integration test
(`testing/integration/TEST_LOG.md`).

## 6. Kernel object budget

Limits are set in `config/config.h`.

| Object | Limit | S1 | S2 | S3 | S4 | S5 | Shared |
|---|---|---|---|---|---|---|---|
| Task | 32 | 2 | 1 | 1 | 1 | 3 | |
| Semaphore | 16 | | | | | | |
| Event flag | 16 | | | | | | |
| Mailbox | 12 | | | | | | |
| Mutex | 12 | | | | | | |
| Message buffer | 12 | 3 | | | | | |
| Fixed memory pool | 12 | | | | | | |
| Variable memory pool | 12 | | | | | | |
| Cyclic handler | 12 | | | | | | |
| Alarm handler | 12 | | | | | | |
| Interrupt handler | | | | | | | 1 (GPIO) |

(The kernel's WiFi service task and initial task also use a task each.)

# Change Requirement 1: minimum-bit data representation

Source: [Change Requirement.docx](<../References/Change Requirement.docx>).
Team decisions: [Tribunal 1, Version B](../Tribunal1/IS29_Imm-Tribunal1.docx)
and the [Post-Tribunal](../Tribunal1/IS29_Post-Tribunal1.docx).

This folder is the Week 13 evidence for the change: the data representation
table, RAM and message sizes before and after, and the correctness and
performance evidence. Every number below is measured from a build or a test
run; nothing is estimated unless the row says so.

The change was made and measured when the firmware lived in `firmware/src`,
`firmware/include` and `firmware/target` and was installed into the
professor's mtk3smp-rp2040 port. The code now sits in the course project
template's `app_program/` (shared code at the top, one `subsystemN/` folder
per buddy); file names are unchanged, so `src/telemetry.c` below is
`app_program/subsystem1/telemetry.c`. The message sizes in section 8 were
re-measured after the move and are identical; the RAM and code sizes in
sections 7 and 10 are from the port build. Re-measured on the template
build, the application's own RAM and code are unchanged (offline 1,799 B
RAM and 43,520 B code; MQTT 1,865 B RAM). The whole-image .bss differs
because of the template's kernel and lwIP settings: offline 14,780 ->
17,216 B (more kernel objects allowed), MQTT 47,380 -> 77,156 B (larger
lwIP memory and TCP buffers in the template's lwipopts.h, a later RAM
candidate); both still pass the 64 KiB heap guard.

## 1. Summary

Scope, as agreed in Tribunal 1 and confirmed with the lecturer: every field
that is shared between tasks or with an ISR, kept in a long-lived record,
queued between tasks or sent over MQTT uses only the bits its valid range
needs. Task-private working values (PID integrator, pose integration,
terrain and scan geometry) keep binary64 precision, and timestamps stay
32-bit. No task period, priority or safety rule changed.

Measured results: all application variables 10,403 B -> 1,799 B (offline image; .bss 23,392 -> 14,780 B); the shared record 496 -> 132 B; the largest telemetry frame 237 -> 17 B (275 -> 54 B as an MQTT PUBLISH); commands 4-8 B -> 1 B. Flash grows 3.5 KB because the 4,344 B barcode codebook moved from RAM to flash; application code itself shrank.

| Subsystem (owner) | What changed | Main files |
|---|---|---|
| Wi-Fi, MQTT, telemetry (B1) | Integer-JSON telemetry replaced by three versioned binary frames (17/16/17 B); ASCII commands by 1-byte codes; rejected payloads counted; WIFI_LOSS from the radio's session flag; 1-byte presence; IPC slots resized; Python codec with shared golden frames | include/telemetry.h, src/telemetry.c, include/mqttcmd.h, src/mqttcmd.c, target/net_mqtt*.c/.h, scripts/telemetry_codec.py |
| Motors, encoders, motion (B2) | Encoder ISR state in one 16-bit word (6+6-bit saturating deltas, 2+2-bit phases) with sticky error bits and 16-bit wrapping totals; packed motion request and snapshot; 8-bit command ids; packed pose; 31-bit motor command | target/board.c/.h, include/core.h, src/core.c, include/motion.h, src/motion.c, include/vehicle.h, src/vehicle.c |
| Line and barcode (B3) | Exact integer ADC thresholds and Q7 steering; barcode positions and bar widths in encoder half-counts; 2-bit darkness class; 4-bit label; constant codebook in flash; OPTICAL split into two mask bits | include/line_ctrl.h, src/line_ctrl.c, include/barcode.h, src/barcode.c, target/course_cfg.c/.h |
| IMU and terrain (B4) | Shared terrain record: 12-bit mrad pitch/roll rounded away from zero, 13-bit yaw rate, 9-bit heights, 4-bit motion class, flags; IMU, TILT, TILT_UNAVAILABLE and COLLISION as mask bits | include/terrain.h, src/terrain.c |
| Ultrasonic, scan, integration (B5) | 61-entry scan table of 16-bit echo codes; packed scan result and front range (2-bit state + 12-bit mm); completion counters; NO_ECHO; shared record grouped by writer; fault mask, stop source and helpers; bitpack library; lock checker | include/scanner.h, src/scanner.c, target/robot_app.c/.h, include/bitpack.h, src/bitpack.c, scripts/check_locks.py |

The pre-change baseline is commit `baa068a`. All "before" figures come from
that commit, built and measured with the same tools as the "after" figures.

## 2. Representation rules

All packing goes through [`bitpack.h`](../app_program/bitpack.h):

- **Width.** A field gets the fewest bits that hold its valid range at the
  resolution its consumers use (`range / resolution`, not range alone).
- **Rounding toward the safe side.** Every real-valued field states one rule.
  For an integer threshold T in field units, `CEIL` keeps `x <= T` and
  `x > T` exact, `FLOOR` keeps `x >= T` and `x < T`, `AWAY` (magnitude
  rounded up) keeps `|x| > T` and `|x| <= T`. Each field in section 3 is
  marked *identical* (every consumer decision is unchanged) or *bounded*
  (a decision can differ only inside the stated margin; the row says when
  that difference can go either way).
- **Overflow.** Unsigned fields saturate at their largest code. Signed fields
  are two's complement, saturate at +/-(2^(n-1)-1) and reserve the most
  negative code for "invalid" (NaN or unavailable). Wrapping is used only
  for counters, each with a stated difference rule.
- **No platform-dependent conversions.** NaN and +/-Inf are tested before any
  float-to-integer conversion, so the laptop and the Cortex-M0+ cannot
  produce different codes. Host sanitizer builds include
  `-fsanitize=float-cast-overflow`.
- **Memory layout.** In-memory records use C bit-fields grouped by the task
  that writes them; enum fields are declared with their minimum width, which
  GCC checks at compile time. Wire frames never use bit-field structs (their
  layout is implementation-defined): they use the explicit little-endian,
  LSB-first bit stream of `bitpack_write()` / `bitpack_read()`.
- **No read-modify-write races.** A bit-field store rewrites its whole
  storage unit and the Cortex-M0+ has no exclusive load/store, so every
  store to the shared record `g_shared` (including single bit-fields) and to
  the ISR-shared board words happens inside `app_main_lock()` (PRIMASK).
  Packing and unpacking happen outside the lock; only the packed copy is
  stored inside it. [`check_locks.py`](../firmware/scripts/check_locks.py)
  enforces this on every `dev.sh style` run.

## 3. Data representation table

Columns: valid range and resolution of the data, bits used, rounding or
overflow rule, invalid code, writer and readers, and whether consumer
decisions are identical or bounded.

| Data (owner) | Range and resolution | Bits | Rounding, overflow, invalid code | Writer -> readers | Decisions |
|---|---|---|---|---|---|
| Encoder ISR word (B2) | Delta +/-31 counts per read (0.15 m/s cap: 1.3 per 5 ms); phase 0..3 | 6+6+2+2 = 16 | Deltas saturate; a further count sets the wheel's sticky error bit (2 bits) -> ENCODER fault | GPIO ISR -> Motion (read-and-clear under PRIMASK) | Identical up to 31 counts per read; beyond is fail-safe (needs > 3.5 m/s at 5 ms) |
| Encoder totals (B2) | Modulo 2^16 counts, 1 count = 0.567 mm | 16+16 | Wrap; int16 difference, 0x8000 = ENCODER fault | GPIO ISR -> Line optical path | Identical (<= 1 count per 2 ms read) |
| Motor command (B2) | Level 0..6250 PWM counts; direction -1, 0, +1 | 13+13+2+2+1 = 31 | Code 2 reserved -> brake | Motion (task message) | Identical |
| Motion request (B2) | Forward +/-255 mm/s, yaw +/-1023 mrad/s, target +/-8191 mm or mrad, id 1..255, kind | 9+11+14+8+2 = 44 | AWAY (magnitude up), symmetric saturation, most negative code = non-finite; TURN capped at 2 pi; id 0 = none, 255 wraps to 1 | Vehicle -> Motion | Identical for every validity check; bounded: only valid -> invalid within 1.07 mm/s of the wheel limit (fail-safe), completion at most one sample later; bypass targets built from CEIL scan distances are rounded up twice, so up to 2 mm longer (more clearance) |
| Pose (B2) | x, y modulo 65.536 m at 1 mm; yaw +/-pi at 1 mrad | 16+16+13 = 45 | NEAREST; wrap-safe int16 differences; reserved yaw code = invalid pose (NaN, fails every gate) | Motion -> Vehicle, Scan | Bounded: range gate within 1.4 mm / 1 mrad, route corridor within 3.1 mm / 1 mrad |
| Motion snapshot (B2) | Pose; wheel speeds +/-511 mm/s at 1 mm/s; id; state; reason; stationary | 45+10+10+8+2+3+1 = 79 | Speeds NEAREST, reserved code = NaN | Motion -> Vehicle, IMU, Scan, Telemetry | Bounded, both directions: IMU quasistatic within 0.5 mm/s of 0.15 m/s, TURNING within 7.1 mrad/s of 0.25 rad/s (below the encoder's speed resolution; re-evaluated every 10 ms) |
| Centre path (B2) | Modulo 2^16 half-counts (left + right counts) | 16 | Wrap; int16 difference | Motion -> IMU | Identical |
| Line thresholds (B3) | ADC 0..4095 per channel; minimum signal as an exact integer | 12+12 per channel; 24 | Exact: binary search over the original double compare | Line (derived at init) | Identical for every raw value (exhaustive) |
| Line output and shared line word (B3) | Status 6 values; steering -127..127 (Q7); navigation, action 5 values; symbol 0..15; unknown and rejected counts | 3+8+1+3+3+4+8+8 | Steering rounds half away from zero; counters saturate at 255; symbol = canonical codebook index | Line -> Vehicle, Telemetry | Identical except steering: within 0.5/127, never sign-inverted, zero exactly when the original was zero |
| Barcode input and widths (B3) | Class LIGHT, BAND, DARK, INVALID; widths 1..255 half-counts (0.284 mm, production maximum 211) | 2; 8 per run | Limits converted exactly to half-counts (ceil/floor); widths over 255 refused at derive | Line (task-private) | Identical to the exact predicate; production margins >= 0.05 half-count |
| Terrain shared record (B4) | Pitch, roll +/-2047 mrad; yaw +/-4095 mrad/s; heights 0..511 mm; motion 9 classes | 12+12+13+9+9+4+5 flags = 64 | Angles AWAY, saturate, reserved code = invalid; yaw NEAREST; heights FLOOR, NaN or negative -> 0 | IMU -> Safety, Vehicle, Telemetry | Identical (TILT > 600 mrad exact, tested at +/-200,000 ulps) |
| Scan table (B5) | 61 even angles -60..60 deg; echo pulse 1..32766 us | 16 per angle (15 + fine bit) | 0 = no echo, 32767 = not sampled (cleared each scan); pulse saturates | Scan (task-private) | Identical: distances recomputed from the same pulse; compared with the old scanner on 720 random scans and every fixture |
| Scan shared result (B5) | Nearest and width 0..4095 mm; counts 0..61; side | 12+12+6+6+2+5 flags = 43 | CEIL; counts saturate at 63; a distance is read only with its validity | Scan -> Vehicle bypass, Telemetry | Identical except the width gate at exactly 1.30 m + 1 ulp: rejects (conservative), unreachable at the 0.60 m scan distance; bypass length up to 2 mm and offset up to 1.5 mm larger (more clearance) |
| Front range (B5) | Unknown, valid, out of range; 0..4095 mm | 2+12 | CEIL (0 unless valid); a valid reading without a finite distance packs unknown: never produced, and it would relax a FAULT to RANGE_WAIT | Scan -> Vehicle, Telemetry | Identical: front stop <= 250 mm exact over every echo pulse |
| Echo word (B5) | 5 states; pulse 0..30000 us | 3+15 | One ISR store per edge; codes 5..7 read as timeout | GPIO ISR -> Scan (under PRIMASK) | Identical |
| Scan completion counters (B5) | Wrapping events | 2+2 | Scan adds 1 per completed scan / fine request; Vehicle consumes each change once per step | Scan -> Vehicle | Identical (at most one change per Vehicle period) |
| Fault record (all) | 16 mask bits; first cause 15 values; stop source; 5 latch flags | 16+4+2+5 = 27 | Set and clear rules in the fault bitmask table | Locked helpers -> all readers | Identical first cause and stop behaviour |
| Vehicle word (B5/B2) | State 9 values; scan request; request version; run id | 4+2+4+8+3 flags | Version and run id wrap; Motion reads within 5 ms | Vehicle -> Motion, Scan, IMU, Telemetry | Identical |
| Telemetry and commands (B1) | Three frames; 7 commands | 131, 126, 132; 8 | See the frame and command tables | Telemetry -> radio -> laptop; laptop -> radio -> Comm | Identical decisions; text commands rejected and counted |
| Timestamps (all) | 32-bit ms and us, wrap-safe differences | 32 | Kept by team decision | All | Identical |

## 4. Fault bitmask

| Bit | Name | Writer task | Set when | Class | Cleared by |
|---|---|---|---|---|---|
| b0 | OWNER_TIMEOUT | Safety P1 | In an active state, a liveness stamp is stale: Motion or Line > 20 ms, Vehicle > 40 ms, IMU or Scan > 100 ms | Hard stop | Reboot |
| b1 | ENCODER | Motion P2; Line P3 | Sticky ISR edge error or 6-bit delta saturation; invalid encoder sign; a 16-bit total difference of exactly 0x8000 | Hard stop | Reboot |
| b2 | MOTION | Motion P2 | A published request is rejected while not inhibited, or motion_step() reports an error | Hard stop | Reboot |
| b3 | LEFT_STALL | Motion P2 | \|left duty\| >= 0.15 with no left count for > 500 ms | Hard stop | Reboot |
| b4 | RIGHT_STALL | Motion P2 | \|right duty\| >= 0.15 with no right count for > 500 ms | Hard stop | Reboot |
| b5 | MOTOR_OUTPUT | Motion P2 | PWM preparation fails with calibration confirmed | Hard stop | Reboot |
| b6 | OPTICAL_SAMPLE | Line P3 | Invalid optical sample in an active state | Hard stop | Reboot |
| b7 | BARCODE_OVERRUN | Line P3 | A second barcode decodes before Vehicle took the first | Hard stop | Reboot |
| b8 | SUPERVISOR | Vehicle P4 | vehicle_step() fails (hard), or the vehicle enters FAULT, e.g. RANGE_WAIT timeout (soft) | Hard or soft stop | Reboot (hard); approved reset (soft) |
| b9 | IMU | IMU P5 | Invalid (not pending) accelerometer sample, terrain step or re-initialisation failure in an active state | Hard stop | Reboot |
| b10 | TILT | Safety P1 | Valid \|pitch\| or \|roll\| > 600 mrad | Hard stop | Reboot |
| b11 | TILT_UNAVAILABLE | Safety P1; IMU P5 | Tilt invalid or older than 100 ms, or a pending sample past the dt bound, in an active state | Soft stop | Approved reset |
| b12 | COLLISION | Safety P1 | Terrain collision candidate in an active state | Hard stop | Reboot |
| b13 | SCAN | Scan P6 | Scan cycle error, aborted scan or a scan longer than 20 s | Hard stop | Reboot |
| b14 | NO_ECHO | Scan P6 | The latest frontal ping returned no valid echo pulse | Status, never stops | Scan, on a valid echo |
| b15 | WIFI_LOSS | Comm P7 (radio P9 flag) | No subscribed MQTT session (boot value 1; always 1 offline) | Status, never stops | Comm, on SUBACK |

Stopping bits are written only by robot_app_fault_set(), inside one app_main_lock() region that also records the first cause and time (if none), sets the hard latch for hard faults, inhibits drive, latches the stop and brakes. Status bits are written only by robot_app_status_set(), which changes that one bit and nothing else.

First cause: the unchanged 4-bit robot_app_fault_t; b6 and b7 both report OPTICAL. Stop source: 2 bits, bit 0 LOCAL (GP20), bit 1 REMOTE (MQTT STOP); STOP requests are not mask bits.

An approved reset (no stop since the Vehicle snapshot that authorised it, no hard fault, GP20 released) clears the first cause, the stop source and every stopping bit, and keeps the status bits.

## 5. Shared-word ownership

Every shared word, every task that writes it (including consumers that
clear an event) and the lock that protects it.

| Shared word (fields) | Owner (writes) | Other writers | Protection |
|---|---|---|---|
| faults: mask 16, first 4, stop_source 2, b_hard, b_inhibit, b_stop_latched, b_stop_since_snapshot, b_reset_clean; first_fault_ms | robot_app_latch_locked() for every stop (Safety, Motion, Line, Vehicle, IMU, Scan, Comm) | robot_app_status_set(): Scan (NO_ECHO), Comm (WIFI_LOSS). Vehicle: inhibit, clears b_stop_since_snapshot, sets b_reset_clean. Motion: approved reset | app_main_lock() in every writer |
| vehicle: packed motion request 44, state 4, scan_request 2, request_version 4, run_id 8, b_reset_motion, b_start, b_reset | Vehicle P4 | Motion clears b_reset_motion | app_main_lock() |
| line: status 3, steering_q7 8, b_junction_edge, navigation 3, barcode action 3, symbol 4, unknown 8, rejected 8; line_ms | Line P3 | Vehicle clears the junction edge and navigation each step and the barcode action and symbol at run start | app_main_lock() |
| motion snapshot; path_hc, previous_path_hc (16-bit); path_ms, previous_path_ms | Motion P2 | None | app_main_lock(); packed before locking |
| terrain shared record; tilt_ms, imu_ms | IMU P5 | None | app_main_lock(); packed before locking |
| scan shared record | Scan P6 | Vehicle clears b_profile_valid | app_main_lock(); packed before locking |
| front: pose, range (2+12), coarse_done 2, fine_done 2; range_ms | Scan P6 | None (Vehicle compares the counters, never writes them) | app_main_lock() |
| health[6] liveness stamps | Each owner its own entry | None | app_main_lock() |
| ignored_commands 8; dropped_telemetry 8 | Comm P7; Telemetry P8 | None | app_main_lock() |
| Radio session flag and rejected-command counter (net_mqtt.c) | Radio P9 | None (Comm only reads) | Single writer, single aligned byte/bool load and store |
| Board ISR words (board.c): encoder word 16, totals 2 x 16, error bits 2, echo word 3 + 15; motor directions 2 + 2 | GPIO ISR (encoder, totals, errors, echo edges); Motion (motor directions) | Motion reads and clears deltas and errors; Scan triggers and takes the echo result | Task side inside app_main_lock(); ISR stores each word once |

scripts/check_locks.py runs in dev.sh style: every store to a watched shared name outside app_main_lock()/unlock() or a '_locked' helper fails the build; its self-test seeds an unlocked write and must fail.

## 6. Messages

### Telemetry frames

| Frame | Size | Fields in order (bits) |
|---|---|---|
| Header (all) | 4 bits | version 2 \| type 2 (0 status, 1 perception, 2 diagnostic, 3 reserved). Bit stream is LSB first, little-endian; padding bits are zero. |
| Status (every 200 ms) | 131 bits / 17 B | seq 8 (0..249) \| ms 32 \| drop 8 \| ign 8 \| age 10 ms (1023 = older) \| st 4 \| vl 10, vr 10 mm/s \| pt 12 mrad \| pk 9 mm \| rg 12 mm \| stop 1 \| fault 1 \| hv 1 \| rv 1 |
| Perception (seq % 5 == 0) | 126 bits / 16 B | seq 8 \| ms 32 \| run 8 \| cal 8 \| ba 3 \| bl 4 \| yr 13 mrad/s \| mc 4 \| sw 12 mm \| sn 12 mm \| ss 2 \| sv 6 \| su 6 \| tv 1 \| ha 1 \| sc 1 \| so 1 |
| Diagnostic (with perception) | 132 bits / 17 B | seq 8 \| ms 32 \| run 8 \| fc 4 \| fm 32 ms \| bu 8 \| br 8 \| mp 9 mm \| mv 1 \| mask 16 \| src 2 |

Unsigned fields saturate at their largest code. Signed fields are two's complement, saturate at +/-(2^(n-1)-1) and reserve the most negative code for invalid (NaN or unavailable).

Decoders (C telemetry_decode_*, Python telemetry_codec.py) refuse a wrong length, another version, type 3 or nonzero padding. Golden frames are shared by the C and Python tests.

### Commands and presence

| Byte | Meaning | Robot action |
|---|---|---|
| 0xA9 | STOP | Remote STOP latched (stop source REMOTE) |
| 0xAA to 0xAF | START, RESET, LEFT, RIGHT, STRAIGHT, UTURN | Valid but ignored remotely (counted in ign, as is a command dropped by a full queue); START and RESET are local-only by policy |
| 0xA8 and every other byte, any length other than 1, ASCII text | Rejected | Counted by the radio, added to ign |
| '1' / '0' on the status topic | Presence online / offline (MQTT will) | Published by the radio and the broker |

Layout: 10101 in bits 7..3 (a fixed pattern that ASCII letters never match), command code in bits 2..0.

### Inter-task message buffers

The radio task (P9) and the control tasks exchange frames and commands only
through micro T-Kernel message buffers
([`net_mqtt_ipc.c`](../app_program/subsystem1/net_mqtt_ipc.c)). Each message costs
a 4-byte kernel header plus its body rounded up to 4 bytes, so a slot is
`(MAX + 3) / 4 + 1` words. Compile-time checks in `net_mqtt_ipc.c` pin
the slots to the kernel's `HEADERSZ + ROUNDSZ` rule (`messagebuf.h`): 24 B
per frame and 8 B per command.

| Buffer | Before (B) | After (B) | Change |
|---|---|---|---|
| g_tx_storage: Telemetry frames, 4 deep | 1,040 | 96 | -944 (-91 %) |
| g_rx_storage: Commands, 4 deep | 144 | 32 | -112 (-78 %) |
| g_stop_storage: Pending STOP, 1 deep | 8 | 8 | +0 (+0 %) |
| g_pending_tx: Radio's frame being sent | 257 | 20 | -237 (-92 %) |

## 7. RAM before and after

Measured from the linker map, ELF sections and DWARF type sizes of the
`offline` and `mqtt` images built by `scripts/dev.sh build` with the pinned
toolchain (Arm GNU 14.2.Rel1, Pico SDK 2.2.0). "Commissioned" builds set
the seven fixture confirmation gates to 1, as on a calibrated robot, so the
board ISR and sensor drivers are linked; they are measurement-only images.

| Item | Before (B) | After (B) | Change |
|---|---|---|---|
| .data (offline image) | 328 | 328 | +0 (+0 %) |
| .bss (offline image) | 23,392 | 14,780 | -8,612 (-37 %) |
| Application variables (offline image) | 10,403 | 1,799 | -8,604 (-83 %) |
| .data (MQTT image) | 2,744 | 2,744 | +0 (+0 %) |
| .bss (MQTT image) | 56,272 | 47,380 | -8,892 (-16 %) |
| Application variables (MQTT image) | 10,750 | 1,865 | -8,885 (-83 %) |
| g_barcode_config (codebook, now const in flash) | 4,344 | 0 (removed) | -4,344 (-100 %) |
| g_scanner (scan state) | 2,720 | 264 | -2,456 (-90 %) |
| g_tx_storage (telemetry IPC buffer) | 1,040 | 96 | -944 (-91 %) |
| g_shared (shared record) | 496 | 132 | -364 (-73 %) |
| g_barcode (barcode decoder state) | 320 | 64 | -256 (-80 %) |
| g_rx_storage (command IPC buffer) | 144 | 32 | -112 (-78 %) |
| g_pending_tx (radio's pending frame) | 257 | 20 | -237 (-92 %) |
| g_terrain (terrain state) | 248 | 240 | -8 (-3 %) |
| g_vehicle (vehicle state) | 160 | 144 | -16 (-10 %) |
| Task stacks (8 x 4,096, kernel heap) | 32,768 | 32,768 | unchanged |

Sections from arm-none-eabi-size; variables from the linker map (-fdata-sections); both images built by scripts/dev.sh build with Arm GNU 14.2.Rel1 and Pico SDK 2.2.0. The MQTT image is the compile-only build with placeholder settings.

| Type | Before (B) | After (B) | Change |
|---|---|---|---|
| robot_app_shared_t: Shared record copied by every task | 496 | 132 | -364 (-73 %) |
| motion_output_t: Motion output (shared before) | 88 | 88 | +0 (+0 %) |
| motion_snapshot_t: Shared motion snapshot (after) | - | 16 | - |
| motion_request_t: Motion request (shared before) | 32 | 32 | +0 (+0 %) |
| motion_request_packed_t: Shared motion request (after) | - | 8 | - |
| core_pose_t: Pose (shared before) | 32 | 32 | +0 (+0 %) |
| core_pose_packed_t: Shared pose (after) | - | 8 | - |
| terrain_output_t: Terrain output | 72 | 64 | -8 (-11 %) |
| terrain_shared_t: Shared terrain record (after) | - | 8 | - |
| scanner_result_t: Scan result (shared before) | 56 | 56 | +0 (+0 %) |
| scanner_shared_t: Shared scan result (after) | - | 8 | - |
| scanner_state_t: Scan state | 2,720 | 264 | -2,456 (-90 %) |
| barcode_state_t: Barcode decoder state | 320 | 64 | -256 (-80 %) |
| line_ctrl_output_t: Line output | 48 | 8 | -40 (-83 %) |
| board_motor_command_t: Motor command | 16 | 4 | -12 (-75 %) |
| vehicle_t: Vehicle state | 160 | 144 | -16 (-10 %) |

Task stacks are not in `.bss`: the eight application tasks get 4,096 B each
from the kernel heap through `tk_cre_tsk`. This change does not alter stack
sizes, so it cannot undersize a task. Static frames from `-fstack-usage` are
in section 10; run-time high-water marks need the robot (section 12).

## 8. Message size before and after

Payloads are measured by compiling each tree's own encoder and running it on
a worst-case and a typical record
([`measure_messages.py`](../firmware/scripts/measure_messages.py)). Command
tokens and presence payloads are read from each tree's source. Full MQTT
3.1.1 QoS 0 PUBLISH sizes add the fixed header, the 2-byte topic length and
the topic of the example configuration (telemetry 33 B, command 31 B,
status 30 B).

| Message | Payload before (B) | Payload after (B) | PUBLISH before (B) | PUBLISH after (B) |
|---|---|---|---|---|
| status frame (worst) | 237 | 17 | 275 | 54 |
| status frame (typical) | 145 | 17 | 183 | 54 |
| perception frame (worst) | 252 | 16 | 290 | 53 |
| perception frame (typical) | 142 | 16 | 180 | 53 |
| diagnostic frame (worst) | 144 | 17 | 182 | 54 |
| diagnostic frame (typical) | 78 | 17 | 115 | 54 |
| command (largest token / any command) | 8 | 1 | 43 | 36 |
| STOP command | 4 | 1 | 39 | 36 |
| presence 'online' (status topic) | 6 | 1 | 40 | 35 |
| presence 'offline' (status topic) | 7 | 1 | 41 | 35 |

Payloads from each tree's own encoder (worst case: every field at its widest; typical: a mid-run record). PUBLISH adds the MQTT 3.1.1 fixed header, topic length and topic (telemetry 33 B, command 31 B, status 30 B).

## 9. Correctness evidence

Every C suite runs twice: a release build (-O2) and a sanitizer build (ASan + UBSan + float-cast-overflow). "Before" is commit baa068a run with the same scripts. Check counts are not comparable where the thing under test changed form: the JSON telemetry test checked text formatting at every buffer length, while the binary test checks golden frames shared with Python, every buffer capacity, saturation, rejection and 60,000 random round trips (each random loop is one check).

| Check | Before | After | Result |
|---|---|---|---|
| bitpack unit tests (new) | - | 61 | pass |
| Core: pose and range packing | 380 | 488 | pass |
| Motion and vehicle (ids, request, snapshot, bounds) | 2,286 | 2,463 | pass |
| Line and barcode (exhaustive and lockstep) | 230,546 | 364,977 | pass |
| Sensors: terrain and scanner (with legacy scanner copy) | 142 | 346 | pass |
| Telemetry encoder (JSON before, binary after) | 1,636 | 81 | pass (see note) |
| Command codec (text before, byte after) | 199 | 311 | pass |
| Python codec (shared golden frames) | - | 8 tests | pass |
| Application scenarios (release + sanitizer runs) | 106 | 122 | pass, 0 failures |
| Board ISR fixture, quadrature | 63 | 10,664 | pass |
| Board ISR fixture, one channel (unconfirmed / confirmed / commissioned) | 29 / 41 / - | 107 / 120 / 139 | pass |
| MQTT adapter with mocked transport | 72 | 90 | pass |
| Radio start-up scenarios | 26 | 26 | pass |
| Style checker (files, findings) | 52, 0 | 56, 0 | pass |
| Lock checker (new): unlocked shared writes | - | 0 (robot_app.c 54 functions, board.c 36) | pass, self-test passes |
| ARM builds (offline; MQTT compile-only) | build | build | pass, image gate passed |

Decision equivalence, old double predicate against the packed value on the same inputs:

- Front stop (<= 250 mm): every integer echo pulse from 0 to 70,000 us through the real range conversion (test_range_pulses), 0 mismatches.
- Tilt (> 600 mrad): +/-200,000 ulps around +/-0.60 rad, a +/-2.5 rad sweep and NaN, 0 mismatches.
- Line hysteresis and minimum signal: every raw value (4,096 per channel; all 16.8 million production raw pairs), 0 mismatches; four 200,000-sample runs in lockstep with the original line controller.
- Barcode: every fixture replayed in lockstep with a verbatim copy of the original decoder; in review, 507 million boundary samples and 32.6 million production-unit samples, 0 mismatches.
- Scanner: a verbatim copy of the original scanner beside the new one on every fixture scene and 720 random scans, identical outputs.
- Motion requests: every validity check identical at production limits, including nextafter neighbours.
- Bounded, stated and tested: pose gates (within 1.4 mm of the 30 mm range gate, 3.1 mm of the corridor, 1 mrad of angle gates), the IMU's wheel-speed decisions (0.5 mm/s of 0.15 m/s, 7.1 mrad/s of 0.25 rad/s, in either direction), steering (0.5/127), motion completion (at most one sample later), bypass targets (up to 2 mm longer and 1.5 mm wider, more clearance) and the bypass width gate (one ulp, conservative).

Mutation testing during review: board.c 21 of 21 (plus 4 added), scanner and terrain 18 of 18, line and barcode 46 of 47 (the survivor is equivalent under modulo int32 conversion).

## 10. Performance evidence

Static evidence from the commissioned `offline` images: code size of each
function from the linker map and its stack frame from `-fstack-usage`.
Whole-record copies of `g_shared` inside `app_main_lock()` now move 132 B
instead of 496 B, which is why most task frames shrank. The Safety task,
the most frequent, reads only the 37 B of fields it checks, so its frame
is unchanged. The IMU frame grew because the IMU step now unpacks the
motion snapshot into a full 88 B `motion_output_t`; every frame stays far
below the 4,096 B task stacks. The ISR's code size is unchanged.

| Function | Code before (B) | Code after (B) | Stack before (B) | Stack after (B) |
|---|---|---|---|---|
| GPIO ISR: encoders and echo (board_gpio_isr) | 372 | 372 | 48 | 64 |
| Safety task step (robot_app_safety_step) | 396 | 452 | 72 | 72 |
| Motion task step (robot_app_motion_step) | 1,144 | 1,224 | 680 | 352 |
| Line task step (robot_app_line_step) | 840 | 992 | 216 | 224 |
| Vehicle task step (robot_app_vehicle_step) | 1,980 | 2,056 | 928 | 504 |
| IMU task step (robot_app_imu_step) | 1,148 | 1,388 | 368 | 552 |
| Scan task step (robot_app_scan_step) | 900 | 972 | 616 | 248 |
| Comm task step (robot_app_comm_step) | 100 | 212 | 56 | 40 |
| Telemetry task step (robot_app_telemetry_step) | 1,844 | 756 | 808 | 256 |
| All application code | 47,880 | 46,944 | - | - |

Code size from the linker map, stack frame from -fstack-usage, in measurement-only images with the seven fixture gates set to 1 so the ISR and drivers are linked. Static evidence only: execution time, masked time and stack high-water marks need the robot (README section 12).

## 11. Refinements to the Tribunal 1 plan

| Version B decision | Implemented | Why |
|---|---|---|
| Scan table: 2-bit state + 12-bit mm + fine bit | 61 x uint16 storing the raw echo pulse code (0 no echo, 1..32766 us, 32767 not sampled) + fine bit | Whole millimetres changed decisions at the 600/650 mm rules and the side tie-break; the pulse keeps them bit-identical in the same 16 bits |
| Pitch/roll saturate at the 0.60 rad limit | Saturate at the 12-bit field limit (+/-2047 mrad), round away from zero | A clamp at 600 mrad can never satisfy > 600 mrad, so TILT could never latch (Version C gap 4) |
| Fault mask via fault_set()/fault_clear() | Stopping bits only via the latch helper; status bits via a separate status helper; no clear of stopping bits except the approved reset | A general clear could release a reboot-only latch (gap 1) |
| stop_epoch / reset_epoch counters (widths unstated) | Two exact flags: stop since snapshot, reset clean (tested by stop_epoch and stop_snapshot) | A 7-8 bit epoch can alias if Motion is starved for up to the 1 s watchdog (gap 11) |
| Status 17 B, perception 17 B, diagnostic 18 B (calculated) | 17 B, 16 B, 17 B (measured); seq modulo 250 | Field tables summed exactly; 250 keeps the every-fifth pairing across the wrap (gap 2) |
| Encoder ISR word 6+6-bit deltas, 16-bit totals | As planned, plus board_encoders_discard() at the end of init (tested by boot_counts) | Edges counted during boot could saturate a 6-bit delta and latch a false hard ENCODER fault |
| RX_MAX 32 -> 1 B with the existing slot formula | Slot = (MAX + 3) / 4 + 1 words, pinned by compile-time checks | The old formula left no room for the 4-byte kernel header (gap 7) |

## 12. Open items and limits

Everything above is host or build evidence. The car is not assembled yet,
so nothing here was measured on the robot. These measurements are planned
for the Week 10 bring-up; the limits marked *proposed* still need team
agreement.

| Measurement | Method | Pass criterion | Owner |
|---|---|---|---|
| GPIO ISR time | Toggle a spare GPIO at ISR entry and exit; logic analyser while both wheels run at 0.15 m/s and the sonar pings | Worst case reported for the old and new image; *proposed* limit 20 us (an echo edge timestamp late by 1 us is 0.17 mm of range) | B2 + B5 |
| Interrupt-masked time | Same GPIO method around the five longest `app_main_lock()` regions (snapshot copy, motion commit, Vehicle publish, fault latch, encoder read-and-clear) | Not longer than the old image; *proposed* limit 20 us | All |
| Task timing | Read the 1 MHz timer at each release and completion in `robot_app_periodic()`; per-task worst step time, worst lateness and missed releases printed over UART after a 10-minute scripted run (follow, barcode, scan and bypass, 1 Hz frames, IMU timeout) | No missed release; every worst step shorter than its period | All |
| Remote STOP latency | 100 STOP bytes under full load; GPIO toggle at STOP validation and at `board_motor_brake()` | Median, p99 and maximum reported for the old text build and the new build | B1 + B2 |
| Stack high-water | Paint each task stack at creation; read the untouched depth over UART after the scripted run | At least 25 % margin per task (*proposed*) before any stack size is cut | All |
| Frame sizes on the broker | `mosquitto_sub -v` capture during the scripted run | Matches section 8 | B1 |
| WIFI_LOSS | Switch the access point off mid-run, then stop the broker | Bit set without stopping the car; will payload `0` on the broker; bit clears after SUBACK | B1 |
| NO_ECHO | Unplug ECHO; then an open corridor | Bit follows the first silent ping; FOLLOW holds in RANGE_WAIT, SUPERVISOR after 1 s | B5 |
| Fault injection | Hold a wheel, unplug an encoder channel, pull the IMU I2C lines | Expected mask bit, first cause and stop behaviour in the diagnostic frame | B2 + B4 |

Other limits:

- Encoder counts per revolution are still the 360-count hypothesis; the
  narrow-bar margin (about 5.3 counts per 3.0 mm bar) depends on it.
- The barcode symbology is inferred (Code 39); the codebook stays authored
  in metres, in flash. A narrow/wide encoding waits for confirmation.
- The lwIP MQTT buffers (1,024 B out, 256 B in, now set under
  `ROBOT_MQTT` in `lib/liblwip/sysdepend/pico_rp2040/lwipopts.h`) are
  unchanged: CONNECT uses them. They are the next RAM candidate.
- Laptop tools must use `scripts/telemetry_codec.py`. Text commands are
  rejected and counted in `ign`, not acted on.
- Timestamps stay 32-bit by team decision (Tribunal 1).
- GP0/GP1 now belong to the HC-SR04 after `board_init()`, so kernel fault
  messages printed after start-up (hard fault, NMI) reach no UART pin; read
  them over SWD with the Debug Probe. Only the start-up refusal gives the
  pins back. A kernel patch that brakes and restores the console pins in
  the fault handlers is recommended but not done.

## 13. Reproduce

From the repository root in WSL (Ubuntu 24.04) with the pinned toolchain
installed by `scripts/dev.sh setup`:

```bash
bash scripts/dev.sh host-tests       # unit tests, release + ASan/UBSan
bash scripts/dev.sh app-tests        # application scenarios
bash scripts/dev.sh style            # style and lock checks
bash scripts/dev.sh build offline    # and: build mqtt --compile-only
python3 firmware/scripts/measure_representation.py \
    --build build-target/offline --label after --output after.json
python3 firmware/scripts/measure_representation.py \
    --compare before.json after.json --types motion_t scanner_state_t
python3 firmware/scripts/measure_messages.py \
    --before <baseline>/firmware --after firmware
```

The measurement reports used above are in [`evidence/`](evidence/).

# Subsystem 2 — test log

**Subsystem:** Motion: motors, encoders, PID (Buddy 2, Ong Teck Ang)  ·  **Student ID:** 2500738  ·  **Code:** `app_program/subsystem2/` (`motion.c/.h`), the shared `app_program/core.c` and the motor and encoder parts of `app_program/board.c`

See `testing/README.md` for how to run tests and record results, and
`TESTING.md` for the set-up. P2 = after TESTING.md section 4; bench = with a
bench program (TESTING.md section 5).

## Test cases

Define a test here before running it. Test IDs never change.

| ID | What is tested | Requirement / interface | Method | Steps / how | Expected result |
|---|---|---|---|---|---|
| S2-U01 | Motion module: requests, leases, PID, completion (the same executable also tests Vehicle, see S5-U02) | FR4 | auto | `bash scripts/dev.sh host-tests test_motion_main` | "Passed 1 executable(s) in both host configurations" |
| S2-U02 | Core: pose and range packing | FR4 | auto | `bash scripts/dev.sh host-tests test_core_main` | "Passed 1 executable(s) in both host configurations" |
| S2-U03 | Encoder interrupt handler (board fixtures) | FR4 | auto | `bash scripts/dev.sh integration` | "integration host checks passed" |
| S2-U04 | Encoder direction | FR4 | manual | P2: lift the car; turn each wheel forward, then back | `vl` and `vr` positive, then negative |
| S2-U05 | Encoder counts per wheel revolution; channel mode | FR4, A2 | manual | Bench: count edges over 10 wheel turns, each wheel | Counts per revolution entered in `robot_config.h` |
| S2-U06 | Wheel radius and track width | FR4, A2 | manual | Measure with calipers / tape | Values entered in `robot_config.h` |
| S2-U07 | Duty-to-speed sweep and breakaway duty, each wheel | FR4 | manual | Bench: step duty, record speed and the duty where the wheel starts | Table saved; `ROBOT_STALL_DUTY` at or above breakaway |
| S2-U08 | PID speed control | FR4 | manual | Bench: run at 0.08 and 0.15 m/s | Steady speed; gains entered |
| S2-U09 | Distance accuracy | FR4 | manual | Bench: drive 1 m, 10 runs | Distance error recorded |
| S2-U10 | Turn accuracy | FR4 | manual | Bench: 90° and 180° pivots, 10 each | Angle error recorded |

*Method:* **auto** = run by `scripts/dev.sh`, result in the saved log;
**manual** = a person observes the result (telemetry, console, meter, video).

## Results

Add a new row for every run. Never edit or delete old rows.

| Date | Test ID | Commit | Tester | Result | Log / evidence | Notes |
|---|---|---|---|---|---|---|
| YYYY-MM-DD | S2-U01 | `abc1234` | | PASS / FAIL | `logs/YYYY-MM-DD_S2-U01.txt` | |

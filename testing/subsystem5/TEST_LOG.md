# Subsystem 5 — test log

**Subsystem:** Obstacles: sonar, servo, bypass; Safety and Vehicle (Buddy 5, Siang Hong)  ·  **Student ID:** 2503147  ·  **Code:** `app_program/subsystem5/` (`scanner.c/.h`, `vehicle.c/.h`), the shared `app_program/robot_app.c` (Safety, Vehicle and Scan tasks) and the sonar, servo and button parts of `app_program/board.c`

See `testing/README.md` for how to run tests and record results, and
`TESTING.md` for the set-up. P2 = after TESTING.md section 4; bench = with a
bench program (TESTING.md section 5).

## Test cases

Define a test here before running it. Test IDs never change.

| ID | What is tested | Requirement / interface | Method | Steps / how | Expected result |
|---|---|---|---|---|---|
| S5-U01 | Scanner module (the same executable also tests terrain, see S4-U01) | FR7 | auto | `bash scripts/dev.sh host-tests test_sensor_main` | "Passed 1 executable(s) in both host configurations" |
| S5-U02 | Vehicle state machine (the same executable also tests Motion, see S2-U01) | FR8, FR11 | auto | `bash scripts/dev.sh host-tests test_motion_main` | "Passed 1 executable(s) in both host configurations" |
| S5-U03 | STOP button | FR11, A8 | manual | 1. Multimeter: GP20 low only while pressed. 2. P2: press STOP. | `state` FAULT, `stop` 1, `stop_source` LOCAL. FAULT at boot without a press means wrong polarity. |
| S5-U04 | Sonar range | FR7 | manual | P2: box at 200, 250, 500 and 1000 mm | `rg` matches the tape (error recorded); `rv` 1 |
| S5-U05 | No echo | FR7 | manual | P2: unplug ECHO | `NO_ECHO` in `faults`; no stop on its own |
| S5-U06 | Servo endpoints and centre | FR7 | manual | Bench: drive the servo to both ends and centre | Sweeps ±60° without hitting its stops; values entered in `robot_config.h` |

*Method:* **auto** = run by `scripts/dev.sh`, result in the saved log;
**manual** = a person observes the result (telemetry, console, meter, video).

## Results

Add a new row for every run. Never edit or delete old rows.

| Date | Test ID | Commit | Tester | Result | Log / evidence | Notes |
|---|---|---|---|---|---|---|
| YYYY-MM-DD | S5-U01 | `abc1234` | | PASS / FAIL | `logs/YYYY-MM-DD_S5-U01.txt` | |

# Subsystem 4 — test log

**Subsystem:** Terrain: IMU, tilt, hump height (Buddy 4, Dian Ze)  ·  **Student ID:** 2500916  ·  **Code:** `app_program/subsystem4/` (`terrain.c/.h`), the IMU parts of the shared `app_program/board.c` and `app_program/robot_app.c`

See `testing/README.md` for how to run tests and record results, and
`TESTING.md` for the set-up. Bench = with a bench program (TESTING.md
section 5); P4 = after TESTING.md section 6.

## Test cases

Define a test here before running it. Test IDs never change.

| ID | What is tested | Requirement / interface | Method | Steps / how | Expected result |
|---|---|---|---|---|---|
| S4-U01 | Terrain module (the same executable also tests the scanner, see S5-U01) | FR5, FR6 | auto | `bash scripts/dev.sh host-tests test_sensor_main` | "Passed 1 executable(s) in both host configurations" |
| S4-U02 | Accelerometer on the I2C bus | FR6 | manual | Bench: I2C bus scan on GP4/GP5 | Device at 0x19 |
| S4-U03 | Six-face calibration | FR6, A5 | manual | Bench: rest the car on each of six faces, record readings | Bias and scale entered in `robot_config.h` |
| S4-U04 | Axis mapping and signs | FR6, A5 | manual | Bench: tilt nose up, then left side up | Readings map to x forward, y left, z up; axes and signs entered |
| S4-U05 | Tilt fault | FR6 | manual | P4: tilt the car by hand past 0.6 rad | `TILT` in `faults`; car stops; needs a power cycle |
| S4-U06 | IMU failure | FR6 | manual | P4: disconnect the IMU I2C lines | `IMU` in `faults`; car stops |

*Method:* **auto** = run by `scripts/dev.sh`, result in the saved log;
**manual** = a person observes the result (telemetry, console, meter, video).

## Results

Add a new row for every run. Never edit or delete old rows.

| Date | Test ID | Commit | Tester | Result | Log / evidence | Notes |
|---|---|---|---|---|---|---|
| YYYY-MM-DD | S4-U01 | `abc1234` | | PASS / FAIL | `logs/YYYY-MM-DD_S4-U01.txt` | |

# Subsystem 3 — test log

**Subsystem:** Optical: IR line following, barcodes (Buddy 3, Tan Yi Jian Ethan)  ·  **Student ID:** 2501528  ·  **Code:** `app_program/subsystem3/` (`line_ctrl.c/.h`, `barcode.c/.h`, `course_cfg.c/.h`)

See `testing/README.md` for how to run tests and record results, and
`TESTING.md` for the set-up. P2 = after TESTING.md section 4; bench = with a
bench program (TESTING.md section 5).

## Test cases

Define a test here before running it. Test IDs never change.

| ID | What is tested | Requirement / interface | Method | Steps / how | Expected result |
|---|---|---|---|---|---|
| S3-U01 | Line controller and barcode decoder (exhaustive and lockstep) | FR1, FR2 | auto | `bash scripts/dev.sh host-tests test_line_main` | "Passed 1 executable(s) in both host configurations" |
| S3-U02 | IR sensors respond | FR1 | manual | P2: move each sensor between black tape and white paper | Grove status LEDs for GP6 (left) and GP7 (barcode) toggle; the right module's own D0 LED toggles |
| S3-U03 | IR A0 readings on black and white | FR1, A3 | manual | Bench: at least 20 readings per sensor on the track's black and white | Clear separation; `ROBOT_IR_*_WHITE/BLACK` entered |
| S3-U04 | Narrowest bar is resolvable | FR2, R3 | manual | Calculate counts per 3.0 mm bar from S2-U05 at the chosen speed | At least 5 counts per narrow bar |
| S3-U05 | Codebook | FR2, A4 | manual | Measure bar widths on the printed sample; confirm codes and actions with the lecturer; enter in `course_cfg.c` | Lecturer-confirmed codebook entered |
| S3-U06 | Junction approach distance | FR3, A4 | manual | Bench: measure travel from junction-edge detection to the axle over the crossbar centre | `ROBOT_SENSOR_AXLE_OFFSET_M` entered |

*Method:* **auto** = run by `scripts/dev.sh`, result in the saved log;
**manual** = a person observes the result (telemetry, console, meter, video).

## Results

Add a new row for every run. Never edit or delete old rows.

| Date | Test ID | Commit | Tester | Result | Log / evidence | Notes |
|---|---|---|---|---|---|---|
| YYYY-MM-DD | S3-U01 | `abc1234` | | PASS / FAIL | `logs/YYYY-MM-DD_S3-U01.txt` | |

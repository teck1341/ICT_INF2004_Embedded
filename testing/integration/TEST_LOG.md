# Integration — test log

Tests that two or more subsystems work together. The owners on both sides
agree on the test and sign off the result.

See `testing/README.md` for how to run tests and record results, and
`TESTING.md` for the set-up. P4 = after TESTING.md section 6.

## Test cases

Define a test here before running it. Test IDs never change.

| ID | What is tested | Requirement / interface | Method | Steps / how | Expected result |
|---|---|---|---|---|---|
| INT-01 | Remote STOP brakes the car | FR10, FR11; S1 → Safety (S5) → Motion (S2) | manual | P4: while driving, `docker exec -i mqtt mosquitto_pub -t inf2004/is29/command -s < stop.bin` | Car stops; `state` FAULT, `stop_source` REMOTE |
| INT-02 | Remote STOP latency under load | NFR5; S1, S2 | manual | P4: 100 STOPs while driving; GPIO toggle at STOP validation and at motor brake, logic analyser (needs instrumentation) | Median, p99 and maximum recorded |
| INT-03 | Local STOP at speed | NFR5, FR11; Safety (S5), S2 | manual | P4: press GP20 at 0.08 m/s | Drive zero within 10 ms; stopping distance recorded |
| INT-04 | Encoder counts while driving | FR4; GPIO ISR (S5), S2 | manual | P4: drive both wheels; compare counts with wheel turns | No lost counts; no false `ENCODER` fault |
| INT-05 | Stall fault stops the car | FR11; S2, Safety (S5) | manual | P4: hold one wheel while driving | `LEFT_STALL` or `RIGHT_STALL`; car stops |
| INT-06 | Telemetry round trip | FR9; S1 and S3, S4, S5 | manual | P2/P4: decoded telemetry against known conditions (box distance, tilt angle, barcode passed) | Decoded values match the source |
| INT-07 | Barcode decoding while driving | FR2; S3, S2 | manual | P4: each barcode, 10 passes | Zero wrong actions; unknown codes ignored (`bu` increases) |
| INT-08 | Junction turn after a barcode | FR3; S3, S2, Vehicle (S5) | manual | P4: barcode, then junction | Axle over the crossbar at the pivot; line reacquired |
| INT-09 | Hump height | FR5; S4, S2 | manual | P4: humps of known height, e.g. 20, 40, 60 mm, 5 runs each | Reported `pk` against true height, error recorded |
| INT-10 | No false terrain faults | FR6; S4, S2 | manual | P4: braking, pivots, hump crests | **No** `TILT` or `COLLISION` fault |
| INT-11 | Obstacle detection while following | FR7; S5, S3, S2 | manual | P4: obstacle on the line, 10 runs | Detected; `sw` and `ss` correct |
| INT-12 | Bypass and line reacquisition | FR8; S5, S2, S3 | manual | P4: bypass left, right, and with heading error | No collision; no false line lock; original line reacquired; bypass time recorded |
| INT-13 | Broker outage during a run | FR9; S1, Vehicle (S5) | manual | P4: `docker stop mqtt` for 60 s mid-run, then `docker start mqtt` | Car keeps running; reconnects; `drop` recorded |
| INT-14 | Shared fault mask under stress (simulated) | FR11; all | auto | `bash scripts/dev.sh app-tests` | `fault_stress: ... failures=0` |

*Method:* **auto** = run by `scripts/dev.sh`, result in the saved log;
**manual** = a person observes the result (telemetry, console, meter, video).

## Results

Add a new row for every run. Never edit or delete old rows.

| Date | Test ID | Commit | Tester | Result | Log / evidence | Notes |
|---|---|---|---|---|---|---|
| YYYY-MM-DD | INT-01 | `abc1234` | | PASS / FAIL | `logs/YYYY-MM-DD_INT-01.jsonl` | |

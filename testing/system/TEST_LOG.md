# System — test log

Tests of the whole car against its requirements. Run the full set before
every demo and before hand-in.

See `testing/README.md` for how to run tests and record results, and
`TESTING.md` for the set-up and the steps referred to below.

## Test cases

Define a test here before running it. Test IDs never change.

| ID | What is tested | Requirement / interface | Method | Steps / how | Expected result |
|---|---|---|---|---|---|
| SYS-01 | All module tests | FR1–FR11 (module level) | auto | `bash scripts/dev.sh host-tests` | "Passed N executable(s) in both host configurations"; Python tests `OK` |
| SYS-02 | Coding standard and lock discipline | NFR2 | auto | `bash scripts/dev.sh style` | "mechanical findings: 0"; "Lock discipline: PASS" |
| SYS-03 | Application scenarios on simulated sensors | FR1–FR11 | auto | `bash scripts/dev.sh app-tests` | "Scenario executions: 122; failures: 0" |
| SYS-04 | Board, installer, radio and network fixtures | NFR1, FR9 | auto | `bash scripts/dev.sh integration` | "integration host checks passed" |
| SYS-05 | Target images build | NFR1, NFR3 | auto | `bash scripts/dev.sh build offline` and `bash scripts/dev.sh build mqtt --compile-only` | "build passed" for both |
| SYS-06 | First flash: start-up refusal | NFR6 | manual | TESTING.md section 2 | Console prints `ROBOT STARTUP REFUSED: ROBOT_WIRING_CONFIRMED=0` |
| SYS-07 | First power-up with WIRING set | FR9, FR11 | manual | TESTING.md section 4.3 | `state` INIT at about 5 Hz; wheels never move |
| SYS-08 | Line following | FR1 | manual | 5 laps under normal light and 5 under bright light | Line losses counted (target 0) |
| SYS-09 | STOP, RESET, START | FR11 | manual | Drive; STOP; RESET (GP21); START (GP21) | Stops; RESET only when eligible; a separate START needed |
| SYS-10 | Full course against Briefing p.8 bands A–D | FR1–FR9 | manual | Full course runs | Detection and reacquisition 100 / 95 / 90 / 80 %; width error ≤ 10 / 20 / 30 / 40 %; bypass < 3 / 5 / 7 / 10 s |
| SYS-11 | 10-minute continuous run | NFR4 | manual | Run the course for 10 minutes with telemetry saved | No resets; no unexpected faults |
| SYS-12 | Task timing, interrupt time, stack high-water | NFR3, NFR4 | manual | Needs instrumentation (ChangeRequirement/README.md section 12) | No missed release; ISR and masked time ≤ 20 µs (proposed); stack margin ≥ 25 % (proposed) |

*Method:* **auto** = run by `scripts/dev.sh`, result in the saved log;
**manual** = a person observes the result (telemetry, console, meter, video).

## Results

Add a new row for every run. Never edit or delete old rows.

| Date | Test ID | Commit | Tester | Result | Log / evidence | Notes |
|---|---|---|---|---|---|---|
| YYYY-MM-DD | SYS-01 | `abc1234` | | PASS / FAIL | `logs/YYYY-MM-DD_SYS-01.txt` | |

# Testing

All test records for the project live in this folder, in the layout of the
course project template. A test only counts as done when it is **written
down here with its result and a saved log**. How to set up the car and run
the tests is in [TESTING.md](../TESTING.md).

```
testing/
├── README.md              this guide
├── subsystem1/            one folder per subsystem, kept by its owner
│   ├── TEST_LOG.md        test cases + results
│   └── logs/              saved console output, telemetry, photos, captures
├── ...
├── subsystem5/
├── integration/           tests across two or more subsystems (shared)
└── system/                tests of the whole car against its requirements (shared)
```

| Folder | Subsystem | Owner |
|---|---|---|
| `subsystem1/` | Communications: Wi-Fi, MQTT, telemetry | Buddy 1, Subque |
| `subsystem2/` | Motion: motors, encoders | Buddy 2, Ong Teck Ang |
| `subsystem3/` | Optical: IR line following, barcodes | Buddy 3, Tan Yi Jian Ethan |
| `subsystem4/` | Terrain: IMU, tilt, hump height | Buddy 4, Dian Ze |
| `subsystem5/` | Obstacles: sonar, servo, bypass; Safety and Vehicle | Buddy 5, Siang Hong |

## Three levels of test

| Level | ID format | Tests what | Written by | Where |
|---|---|---|---|---|
| Unit | `S1-U01`, `S2-U01`, … | one subsystem on its own | the subsystem owner | `subsystemN/TEST_LOG.md` |
| Integration | `INT-01`, … | an interface between subsystems | the owners on both sides | `integration/TEST_LOG.md` |
| System | `SYS-01`, … | the whole car against a requirement | whole team | `system/TEST_LOG.md` |

Test IDs never change and are never reused, even if a test is dropped.

## How to run and record a test

1. **Define it first.** Add a row to the *Test cases* table in the right
   `TEST_LOG.md`: ID, what it checks, how, and the expected result.
2. **Commit your work** so the result can name the exact code it ran on.
3. **Run it.**
   - *Auto* tests run on the laptop with `scripts/dev.sh` (host tests,
     style, application scenarios, integration fixtures, builds). This
     project has no on-target self-test image (`make TEST=1`); its automated
     tests are host-side. Save the output with `tee`:

     ```bash
     bash scripts/dev.sh host-tests test_motion_main 2>&1 \
       | tee testing/subsystem2/logs/$(date +%F)_S2-U01.txt
     ```

   - *Manual* tests run on the car. Watch telemetry, the console, a meter or
     the car itself. Save telemetry with `tee` (see TESTING.md section 3.4):

     ```bash
     docker exec mqtt mosquitto_sub -t inf2004/is29/telemetry -F %x \
       | python -u firmware/scripts/telemetry_codec.py decode - \
       | tee testing/subsystem5/logs/$(date +%F)_S5-U03.jsonl
     ```

4. **Save the evidence** in that folder's `logs/`, named
   `YYYY-MM-DD_<ID>.txt` (or `.jsonl` for telemetry). Photos and scope
   screenshots go there too (keep each file under a few MB). Videos go in
   [`videos/`](../videos/README.md) instead.
5. **Record the result.** Add a row to the *Results* table: date, test ID,
   the commit you tested (`git rev-parse --short HEAD`), who ran it,
   PASS/FAIL, and the log file. Commit the log and the table together.

## Videos of tests and demos

A manual test (a motor, a sensor reacting, the car on the track) is best
shown with a short video. Record 15–60 s showing the test ID and the
hardware, share it as an **unlisted YouTube** link, list it in
[`videos/README.md`](../videos/README.md), and put the link in the
*Log / evidence* column of the test's result row.

## Rules

- **Never edit or delete an old result.** If a test fails, record the FAIL,
  fix the code, re-run, and add a new PASS row. The history is the evidence.
- **Always record the commit.** Commit your work *before* testing.
- **A FAIL needs a note** saying what went wrong and, later, which commit
  fixed it.
- Before merging a change into `main`, re-run your subsystem's unit tests.
  Before a demo or hand-in, run every test and record the full set.

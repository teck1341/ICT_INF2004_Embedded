# Robot testing guide — IS29

The steps to set up, put the firmware on the car, test it, and run each
buddy's test cases.

## Contents

1. [Test the code](#1-test-the-code)
2. [Put the firmware on the Pico](#2-put-the-firmware-on-the-pico)
3. [Set up Wi-Fi and the laptop](#3-set-up-wi-fi-and-the-laptop)
4. [Wiring check and first power-up](#4-wiring-check-and-first-power-up)
5. [Calibrate and set the flags](#5-calibrate-and-set-the-flags)
6. [First drives](#6-first-drives)
7. [Test cases for each buddy](#7-test-cases-for-each-buddy)

What you need:

- A data (not charge-only) micro-USB cable
- A 3.3 V USB-serial adapter or a Raspberry Pi Debug Probe (console,
  115200 8N1)
- A stand that lifts the wheels
- Multimeter, tape measure, protractor

## 1. Test the code

Run these in Ubuntu (WSL), not Git Bash or PowerShell:

The repository is the course project template: the robot code is in
`app_program/` (shared code at the top, `subsystem1/` to `subsystem5/` one
per buddy) and the kernel and build files are committed with it.

```bash
cd "/mnt/c/<path-to>/ICT_INF2004_Embedded"
bash scripts/setup.sh                          # once: Pico SDK 2.2.0 + ARM compiler

bash scripts/dev.sh host-tests                 # module tests, release + ASan/UBSan
bash scripts/dev.sh style                      # BARR-C subset + lock discipline
bash scripts/dev.sh app-tests                  # whole-application scenarios
bash scripts/dev.sh integration                # board, radio and MQTT fixtures
bash scripts/dev.sh build offline              # ARM image, offline profile
bash scripts/dev.sh build mqtt --compile-only  # ARM image, MQTT placeholders
```

From PowerShell: `wsl.exe --cd "<repo path>" -e bash scripts/dev.sh <command>`.

Pass means:

- `host-tests`: "Passed N executable(s) in both host configurations"
- `style`: "mechanical findings: 0" and "Lock discipline: PASS"
- `app-tests`: "Scenario executions: 122; failures: 0"
- `integration`: "integration host checks passed"
- `build`: "build passed"

These are tests SYS-01 to SYS-05. Save each one's output as evidence, for
example:

```bash
bash scripts/dev.sh host-tests 2>&1 | tee testing/system/logs/$(date +%F)_SYS-01.txt
```

Run `app-tests` on `main`, where every flag in `robot_config.h` is `0`. The
harness stops with "Expected one unconfirmed gate" if a flag is `1`.

## 2. Put the firmware on the Pico

The first flash uses the **offline** image, which needs no Wi-Fi settings.

### 2.1 Build

```bash
bash scripts/dev.sh build offline
```

Output: `build-target/offline/mtk3pico_smp0_uart.uf2`.

### 2.2 Flash

1. Unplug USB.
2. Hold **BOOTSEL** on the Pico W and plug USB in. Release only once it is
   fully connected.
3. Drag the `.uf2` onto the **RPI-RP2** drive.
4. The Pico reboots and the drive disappears. The flash worked.

### 2.3 Check the console

With every flag at `0` the firmware refuses to start, by design. Unplug the
HC-SR04 from Grove 1 and connect the adapter: adapter RX to GP0, adapter TX
to GP1, GND to GND, adapter power not connected. Open PuTTY or Tera Term at
115200 and reset the Pico. Expected:

```
ROBOT STARTUP REFUSED: ROBOT_WIRING_CONFIRMED=0; review wiring
Motors inhibited; review configuration.
```

This proves the build, flash and console all work. Any later start-up
refusal also prints its reason here. Without an adapter, skip this check:
the drive disappearing in 2.2 already shows the flash worked.

## 3. Set up Wi-Fi and the laptop

Needed from section 4 onwards, where the car sends telemetry.

### 3.1 Wi-Fi

The Pico W joins only **2.4 GHz WPA2-Personal** networks; campus enterprise
Wi-Fi will not work. Use a phone hotspot on 2.4 GHz (iPhone: "Maximize
Compatibility"; Android: AP band 2.4 GHz). Connect the laptop to it and note
the IPv4 address under "Wireless LAN adapter Wi-Fi" in `ipconfig`.

### 3.2 MQTT broker (Docker)

The broker and the `mosquitto_sub`/`mosquitto_pub` tools come from the
official `eclipse-mosquitto` image on Docker Hub. If your lecturer provides
a broker, use its IP in 3.3 instead and skip this section.

1. Install and open Docker Desktop. Keep it open whenever you test.
2. Create `C:\Users\<you>\mqtt\mosquitto.conf` containing:

   ```
   listener 1883 0.0.0.0
   allow_anonymous true
   ```

3. Start the broker (once; it restarts with Docker Desktop):

   ```powershell
   docker run -d --name mqtt --restart unless-stopped -p 1883:1883 -v "C:\Users\<you>\mqtt\mosquitto.conf:/mosquitto/config/mosquitto.conf" eclipse-mosquitto:2
   docker logs mqtt
   ```

   The log must show `Opening ipv4 listen socket on port 1883`.

4. In an Administrator PowerShell, open the port:

   ```powershell
   New-NetFirewallRule -DisplayName "MQTT 1883" -Direction Inbound -Protocol TCP -LocalPort 1883 -Action Allow
   ```

5. Test from your phone, not the laptop (a test from the laptop to its own
   IP skips the firewall). Join the same hotspot, open any MQTT client app,
   connect to the laptop IP on port 1883 with no login, and publish to
   topic `test`. In Git Bash, `docker exec mqtt mosquitto_sub -t test -v`
   must print the message.

### 3.3 Network settings for the car

Both files are git-ignored. Never commit them.

```bash
cp config/wifi_credentials.example.h config/wifi_credentials.h
cp config/net_mqtt_config.example.h config/net_mqtt_config.h
```

- `config/wifi_credentials.h`: the hotspot name and password, then
  `UTK_WIFI_CREDENTIALS_SET 1`.
- `config/net_mqtt_config.h`:
  - `NET_MQTT_BROKER_IPV4` to the laptop IP
  - `NET_MQTT_CLIENT_ID` to `"inf2004-is29"`
  - the topics to `inf2004/is29/command`, `inf2004/is29/telemetry` and
    `inf2004/is29/status`
  - `NET_MQTT_CONFIGURED 1`

If the laptop's IP changes, update the file and rebuild.

### 3.4 Telemetry and STOP from the laptop

Use **Git Bash** from the repository folder (PowerShell corrupts the binary
STOP byte). The MQTT tools run inside the `mqtt` container:

```bash
python firmware/scripts/telemetry_codec.py command STOP --raw > stop.bin   # once

# Live telemetry, decoded and saved (one file per test, named by test ID)
docker exec mqtt mosquitto_sub -t inf2004/is29/telemetry -F %x \
  | python -u firmware/scripts/telemetry_codec.py decode - \
  | tee testing/subsystem5/logs/$(date +%F)_S5-U03.jsonl

# Presence: 1 online, 0 offline
docker exec mqtt mosquitto_sub -t inf2004/is29/status

# Remote STOP
docker exec -i mqtt mosquitto_pub -t inf2004/is29/command -s < stop.bin

# Frame sizes in bytes
docker exec mqtt mosquitto_sub -t inf2004/is29/telemetry -F %l

# Stop and restart the broker (B1 outage test)
docker stop mqtt
docker start mqtt
```

Fields used in the tests: `state` (INIT, IDLE, FOLLOW, NAVIGATE, SCAN,
BYPASS, REACQUIRE, FAULT, RANGE_WAIT), `vl`/`vr` wheel speed mm/s, `pt`
pitch mrad, `pk` hump peak mm, `rg`/`rv` range mm and valid, `ign` ignored
commands, `drop` dropped frames, `cal` calibration version, `ba`/`bl` barcode
action and symbol, `sw`/`sn`/`ss` obstacle width, nearest and clear side,
`bu` unknown barcodes, `faults` and `stop_source`.

## 4. Wiring check and first power-up

### 4.1 Work on a branch

```bash
git switch -c commissioning
```

Flag changes and measured values are committed here with a note of their
evidence. `main` keeps all flags at `0` for the tests in section 1.

### 4.2 Check the wiring

Check every connection against the pin table in
`app_program/robot_config.h`. With a multimeter:

- ECHO at the divider output is **at most 3.3 V** before GP1.
- GP20 (STOP) and GP21 (START) read low **only while pressed**.
- The VBAT sense jumper on the back of the Robo Pico is **open**.

### 4.3 Set WIRING, build with Wi-Fi, flash

1. Set `#define ROBOT_WIRING_CONFIRMED 1` in `app_program/robot_config.h`.
2. Build the MQTT image (needs section 3.3):

   ```bash
   bash scripts/dev.sh build mqtt
   ```

   Output: `build-target/mqtt/mtk3pico_smp0_uart_wifi_lwip.uf2`. It contains
   the Wi-Fi password: never share, upload or commit it.
3. Flash it as in 2.2 and plug the sonar back into Grove 1.

Expected: presence shows `1`; status frames at about 5 Hz with
`"state": "INIT"`; the wheels never move. Staying in `INIT` is correct
until section 5.

Now run the **P2** tests in [section 7](#7-test-cases-for-each-buddy). RESET
does not work yet, so power-cycle the car to clear a STOP.

## 5. Calibrate and set the flags

The car leaves `INIT` only when `CALIBRATION`, `SERVO`, `WIRING`,
`IMU_MOUNT` and `COURSE` are `1` and `BARCODE` is `1` with a non-empty
codebook. Bypass also needs `CORRIDOR`.

The mission firmware cannot measure these values (raw IR, raw accelerometer
and encoder counts are not in telemetry, and the motors only spin during a
mission), so the **bench** tests in section 7 use small bench programs.

| Flag | Values to measure and enter | Who |
|---|---|---|
| `ROBOT_CALIBRATION_CONFIRMED` | Encoder counts/rev and channel mode, wheel radius, track width, motor and encoder signs, `ROBOT_STALL_DUTY`, PID gains (B2); IR black/white thresholds, junction approach distance (B3) | B2 + B3 + B4 together |
| `ROBOT_IMU_MOUNT_CONFIRMED` | Accelerometer bias and scale, axis mapping and signs, flat reference | B4 |
| `ROBOT_SERVO_CONFIRMED` | Servo min/centre/max pulse, direction, settle time | B5 |
| `ROBOT_BARCODE_CONFIRMED` | Codebook in `app_program/subsystem3/course_cfg.c` (sample barcodes look like Code 39; confirm codes and actions with the lecturer) | B3 |
| `ROBOT_COURSE_CONFIRMED` | Track measurements | Team |
| `ROBOT_CORRIDOR_CONFIRMED` | Robot width and length, deepest obstacle | B5 |
| `ROBOT_SINGLE_CHANNEL_DIRECTION_CONFIRMED` | Only if `ROBOT_ENCODER_CHANNELS` is `1`: coast and reversal trials | B2 |

- Set `CALIBRATION` only when B2, B3 and B4 have all entered measured values.
- Increase `ROBOT_CALIBRATION_VERSION` whenever values change; it shows in
  telemetry as `cal`.
- Re-check B3's barcode decoding whenever B2's counts per revolution change.

## 6. First drives

### 6.1 Start conditions

- Power on with the car **still and flat**.
- Wait for `"state": "IDLE"`. If it stays in `INIT`, a flag is missing or a
  sensor is not ready: check the console and `faults`.
- Place the car on the line with STOP released.
- The sonar needs a **valid echo more than 250 mm ahead**. Keep a wall or
  board 0.3–3 m in front on straight sections: with no echo the car holds in
  `RANGE_WAIT` and faults after 1 s.

### 6.2 Buttons

| Button | In state | Effect |
|---|---|---|
| GP21 | IDLE | START |
| GP20 | any | STOP |
| GP21 | FAULT | RESET to IDLE (stationary, STOP released, no hard fault) |
| GP21 again | IDLE | START |

Hard faults (tilt, collision, stall, encoder, IMU) clear only with a power
cycle.

### 6.3 Order

Keep a hand near STOP.

1. Straight line, 1 m, at 0.08 m/s (`vl` and `vr` about 80)
2. Curves
3. A barcode followed by a junction
4. A hump
5. An obstacle, from each side
6. The full course

Then run the **P4** tests in section 7.

## 7. Test cases for each buddy

The full test cases (method, steps, expected result) and the results are
in the `TEST_LOG.md` files under [`testing/`](testing/README.md). Record
every run there with its commit and a saved log; see
[`testing/README.md`](testing/README.md) for the rules.

Phase: **P2** after section 4, **bench** in section 5, **P4** after section
6, **auto** on the laptop (section 1).

### B1 Subque — Wi-Fi, MQTT, telemetry ([log](testing/subsystem1/TEST_LOG.md))

| ID | Phase | Test |
|---|---|---|
| S1-U01 | auto | Telemetry encoder and Python decoder |
| S1-U02 | auto | Command codec |
| S1-U03 | auto | MQTT adapter and radio start-up |
| S1-U04 | P2 | Car joins the hotspot and broker |
| S1-U05 | P2 | Frame sizes on the broker |
| S1-U06 | P2 | Frame rates and sequence numbers |
| S1-U07 | P2 | Text command rejected and counted |
| S1-U08 | P2 | Wi-Fi loss and reconnect |
| S1-U09 | P2 | Broker restart |
| S1-U10 | P2 | Last Will on power loss |

### B2 Ong Teck Ang — motors, encoders ([log](testing/subsystem2/TEST_LOG.md))

| ID | Phase | Test |
|---|---|---|
| S2-U01 | auto | Motion module |
| S2-U02 | auto | Pose and range packing |
| S2-U03 | auto | Encoder interrupt handler |
| S2-U04 | P2 | Encoder direction |
| S2-U05 | bench | Encoder counts per wheel revolution |
| S2-U06 | bench | Wheel radius and track width |
| S2-U07 | bench | Duty sweep and breakaway duty |
| S2-U08 | bench | PID speed control |
| S2-U09 | bench | Distance accuracy |
| S2-U10 | bench | Turn accuracy |

### B3 Tan Yi Jian Ethan — IR line, barcodes ([log](testing/subsystem3/TEST_LOG.md))

| ID | Phase | Test |
|---|---|---|
| S3-U01 | auto | Line controller and barcode decoder |
| S3-U02 | P2 | IR sensors respond |
| S3-U03 | bench | IR readings on black and white |
| S3-U04 | bench | Narrowest bar is resolvable |
| S3-U05 | bench | Codebook |
| S3-U06 | bench | Junction approach distance |

### B4 Dian Ze — IMU, terrain ([log](testing/subsystem4/TEST_LOG.md))

| ID | Phase | Test |
|---|---|---|
| S4-U01 | auto | Terrain module |
| S4-U02 | bench | Accelerometer on the I2C bus |
| S4-U03 | bench | Six-face calibration |
| S4-U04 | bench | Axis mapping and signs |
| S4-U05 | P4 | Tilt fault |
| S4-U06 | P4 | IMU failure |

### B5 Siang Hong — sonar, servo, obstacles, safety ([log](testing/subsystem5/TEST_LOG.md))

| ID | Phase | Test |
|---|---|---|
| S5-U01 | auto | Scanner module |
| S5-U02 | auto | Vehicle state machine |
| S5-U03 | P2 | STOP button |
| S5-U04 | P2 | Sonar range |
| S5-U05 | P2 | No echo |
| S5-U06 | bench | Servo endpoints and centre |

### Integration ([log](testing/integration/TEST_LOG.md))

| ID | Phase | Test | Buddies |
|---|---|---|---|
| INT-01 | P4 | Remote STOP brakes the car | B1, B5, B2 |
| INT-02 | P4 | Remote STOP latency under load | B1, B2 |
| INT-03 | P4 | Local STOP at speed | B5, B2 |
| INT-04 | P4 | Encoder counts while driving | B2, B5 |
| INT-05 | P4 | Stall fault stops the car | B2, B5 |
| INT-06 | P2/P4 | Telemetry round trip | B1, B3, B4, B5 |
| INT-07 | P4 | Barcode decoding while driving | B3, B2 |
| INT-08 | P4 | Junction turn after a barcode | B3, B2, B5 |
| INT-09 | P4 | Hump height | B4, B2 |
| INT-10 | P4 | No false terrain faults | B4, B2 |
| INT-11 | P4 | Obstacle detection while following | B5, B3, B2 |
| INT-12 | P4 | Bypass and line reacquisition | B5, B2, B3 |
| INT-13 | P4 | Broker outage during a run | B1, B5 |
| INT-14 | auto | Shared fault mask under stress | All |

### System ([log](testing/system/TEST_LOG.md))

| ID | Phase | Test |
|---|---|---|
| SYS-01 | auto | All module tests |
| SYS-02 | auto | Coding standard and lock discipline |
| SYS-03 | auto | Application scenarios |
| SYS-04 | auto | Board, installer, radio and network fixtures |
| SYS-05 | auto | Target images build |
| SYS-06 | section 2 | First flash: start-up refusal |
| SYS-07 | section 4 | First power-up with WIRING set |
| SYS-08 | P4 | Line following |
| SYS-09 | P4 | STOP, RESET, START |
| SYS-10 | P4 | Full course against bands A–D |
| SYS-11 | P4 | 10-minute continuous run |
| SYS-12 | P4 | Task timing, interrupt time, stack high-water |

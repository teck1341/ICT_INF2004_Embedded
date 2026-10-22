# Subsystem 1 — test log

**Subsystem:** Communications: Wi-Fi, MQTT, telemetry (Buddy 1, Subque)  ·  **Student ID:** 2500599  ·  **Code:** `app_program/subsystem1/` (`net_mqtt*.c/.h`, `telemetry.c/.h`, `mqttcmd.c/.h`), `firmware/scripts/telemetry_codec.py`

See `testing/README.md` for how to run tests and record results, and
`TESTING.md` for the set-up. P2 = after TESTING.md section 4.

## Test cases

Define a test here before running it. Test IDs never change.

| ID | What is tested | Requirement / interface | Method | Steps / how | Expected result |
|---|---|---|---|---|---|
| S1-U01 | Telemetry encoder: binary frames, every buffer size, saturation; Python decoder golden frames | FR9 | auto | `bash scripts/dev.sh host-tests test_telemetry_main` | "Passed 1 executable(s) in both host configurations", then Python tests `OK` |
| S1-U02 | Command codec: 1-byte commands, rejected payloads | FR10 | auto | `bash scripts/dev.sh host-tests test_mqtt_main` | "Passed 1 executable(s) in both host configurations" |
| S1-U03 | MQTT adapter with mocked transport; radio start-up | FR9 | auto | `bash scripts/dev.sh integration` | "mqttnet mocked adapter: 90 checks, 0 failures" (release and sanitize); "PASS: 26 radio/kernel scenario executions" |
| S1-U04 | Car joins the hotspot and broker | FR9 | manual | P2: 1. Flash the MQTT image. 2. `docker exec mqtt mosquitto_sub -t inf2004/is29/status`. 3. `docker logs mqtt`. | Presence `1`; broker log shows client `inf2004-is29` connected |
| S1-U05 | Frame sizes on the broker | FR9 | manual | P2: `docker exec mqtt mosquitto_sub -t inf2004/is29/telemetry -F %l` for 30 s | Only 17, 16 and 17 bytes |
| S1-U06 | Frame rates and sequence numbers | FR9 | manual | P2: save 5 min of decoded telemetry to `.jsonl` | Status about 5 Hz; perception and diagnostic 1 Hz; no gaps in `seq` |
| S1-U07 | Text command rejected and counted | FR10 | manual | P2: `docker exec mqtt mosquitto_pub -t inf2004/is29/command -m STOP` | No stop; `ign` increases by 1 |
| S1-U08 | Wi-Fi loss and reconnect | FR9 | manual | P2: hotspot off for 30 s, then on | `WIFI_LOSS` set without a stop; clears after reconnect; telemetry resumes |
| S1-U09 | Broker restart | FR9 | manual | P2: `docker stop mqtt`, wait 60 s, `docker start mqtt` | Car reconnects; presence `1`; telemetry resumes |
| S1-U10 | Last Will on power loss | FR9 | manual | P2: switch the car off | Presence becomes `0` after the broker's keep-alive timeout |

*Method:* **auto** = run by `scripts/dev.sh`, result in the saved log;
**manual** = a person observes the result (telemetry, console, meter, video).

## Results

Add a new row for every run. Never edit or delete old rows.

| Date | Test ID | Commit | Tester | Result | Log / evidence | Notes |
|---|---|---|---|---|---|---|
| YYYY-MM-DD | S1-U01 | `abc1234` | | PASS / FAIL | `logs/YYYY-MM-DD_S1-U01.txt` | |

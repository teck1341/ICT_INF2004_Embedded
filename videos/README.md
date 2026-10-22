# Videos

Short videos of tests and demos running on the real car. A video shows what
a log cannot: the motor turning, the car following the line, the bypass.
List **every** video in the table at the bottom of this page.

## What to record

- **Short:** 15–60 seconds. One test or one feature per video.
- **Say what it is.** Show or say the test ID (e.g. `S2-U09`, `INT-12`) or
  the demo name at the start.
- **Show the commit.** Run `git rev-parse --short HEAD` before recording and
  keep the terminal in shot for a moment, or put it in the table below.
- **Show the result.** Keep both the car and, if it matters, the telemetry
  in view.
- **Landscape**, steady, good light.

## How to share it: unlisted YouTube only

1. Upload the video to YouTube with visibility set to **Unlisted**. Not
   *Private* (the link won't work for the team or instructor), and not
   *Public*.
2. Copy the video's link (e.g. `https://youtu.be/...`).

**Do not commit video files to this repository.** A video stays in the git
history for good, even if deleted later. Video files are ignored by
`.gitignore`.

Then add a row below. If the video is evidence for a test, also put its link
in the *Log / evidence* column of that test's row in
`testing/.../TEST_LOG.md`.

## Video index

Newest at the bottom. Never delete a row.

| Date | Subsystem | Test ID / demo | Commit | Student ID | Link | Notes |
|---|---|---|---|---|---|---|
| YYYY-MM-DD | S1 | S1-U04 | `abc1234` | | <https://youtu.be/...> | |

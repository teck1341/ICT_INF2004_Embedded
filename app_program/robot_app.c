/** @file robot_app.c
 * @brief Single-core RTOS application: sensor owners and one motion writer.
 * Shared records are copied inside bounded interrupt-masked regions. No I2C,
 * formatting, scan, network or floating-point computation occurs while locked.
 * Shared data uses minimum-width fields grouped by producing task (see
 * robot_app_shared_t). Every write to g_shared, including each bit-field
 * store, happens inside app_main_lock(): a bit-field store rewrites its whole
 * storage unit and the Cortex-M0+ has no exclusive load/store, so PRIMASK is
 * the only safe read-modify-write. Only robot_app_latch_locked(), the
 * status helper, the vehicle inhibit and the approved reset write the fault
 * word.
 * BARR-NUMERIC-01: direct double uses the core.h binary64 precision contract.
 */
#include "robot_app.h"
#include "app_main.h"
#include "app_cfg.h"
#include "bitpack.h"
#include "board.h"
#include "course_cfg.h"
#include "mqttcmd.h"
#include "net_mqtt.h"
#include "robot_config.h"
#include <math.h>
#include <stddef.h>
#include "telemetry.h"
#include <string.h>

#define ROBOT_APP_MG_TO_MPS2 (0.00980665)
#define ROBOT_APP_SERIAL_HALF (UINT32_C(2147483648))
#define ROBOT_APP_COUNT_HALF (UINT32_C(0x8000))
#define ROBOT_APP_COUNT_RANGE (INT32_C(0x10000))
#define ROBOT_APP_TILT_TIMEOUT_MS (100U)
#define ROBOT_APP_ANGLE_BITS (12U)
#define ROBOT_APP_SPEED_BITS (10U)
#define ROBOT_APP_RATE_BITS (13U)
#define ROBOT_APP_STEER_BITS (8U)
#define ROBOT_APP_STEER_SCALE (127.0)
#define ROBOT_APP_AGE_BITS (10U)
#define ROBOT_APP_BYTE_BITS (8U)
#define ROBOT_APP_MASK_BITS (16U)
#define ROBOT_APP_SOURCE_MASK (3U)
#define ROBOT_APP_VERSION_MASK (0xFU)
#define ROBOT_APP_DONE_MASK (3U)

/** Fault record: written only under the lock (see file comment). A reset
 * is approved only if no stop was raised after the Vehicle snapshot that
 * authorised it. Two flags encode that exactly: every stop sets
 * b_stop_since_snapshot and clears b_reset_clean; Vehicle clears the first
 * at its snapshot and sets b_reset_clean = !b_stop_since_snapshot when it
 * publishes the authorisation; Motion reads b_reset_clean live. A counter
 * could alias: a starved Motion task leaves the window open until the
 * 1 s watchdog, long enough for hundreds of soft stops.
 */
typedef struct
{
    uint32_t          mask                  : 16;
    robot_app_fault_t first                 : 4;
    uint32_t          stop_source           : 2;
    bool              b_hard                : 1;
    bool              b_inhibit             : 1;
    bool              b_stop_latched        : 1;
    bool              b_stop_since_snapshot : 1;
    bool              b_reset_clean         : 1;
} robot_app_faults_t;

/** Liveness stamps of the six owners that gate the watchdog; Safety and
 * Telemetry stamps had no reader and are not kept.
 */
typedef enum
{
    ROBOT_APP_HEALTH_MOTION = 0,
    ROBOT_APP_HEALTH_LINE,
    ROBOT_APP_HEALTH_VEHICLE,
    ROBOT_APP_HEALTH_IMU,
    ROBOT_APP_HEALTH_SCAN,
    ROBOT_APP_HEALTH_COMM,
    ROBOT_APP_HEALTH_COUNT
} robot_app_health_t;

/** Vehicle task (P4) word and the packed request it publishes. Motion
 * clears b_reset_motion; the button handler in this task sets b_start and
 * b_reset. request_version wraps: two reads are at most 5 ms apart.
 */
typedef struct
{
    motion_request_packed_t request;
    vehicle_state_t         state           : 4;
    vehicle_scan_t          scan_request    : 2;
    uint32_t                request_version : 4;
    uint32_t                run_id          : 8;
    bool                    b_reset_motion  : 1;
    bool                    b_start         : 1;
    bool                    b_reset         : 1;
} robot_app_vehicle_t;

/** Line task (P3) words. The junction edge and navigation are events that
 * Vehicle clears; Vehicle also resets the barcode action and symbol at run
 * start. Steering is Q7 two's complement; bu/br saturate at 255.
 */
typedef struct
{
    line_ctrl_status_t status           : 3;
    uint32_t           steering_q7      : 8;
    bool               b_junction_edge  : 1;
    vehicle_nav_t      navigation       : 3;
    barcode_action_t   barcode_action   : 3;
    uint32_t           barcode_symbol   : 4;
    uint32_t           barcode_unknown  : 8;
    uint32_t           barcode_rejected : 8;
} robot_app_line_t;

/** Scan task (P6) front reading: the pose at its trigger, the packed range
 * and two completion counters. Scan is their only writer; Vehicle compares
 * them with the values it last acted on.
 */
typedef struct
{
    core_pose_packed_t  pose;
    core_range_packed_t range;
    uint32_t            coarse_done : 2;
    uint32_t            fine_done   : 2;
} robot_app_front_t;

typedef struct
{
    uint32_t            health[ROBOT_APP_HEALTH_COUNT];
    uint32_t            line_ms;
    uint32_t            imu_ms;
    uint32_t            tilt_ms;
    uint32_t            range_ms;
    uint32_t            path_ms;
    uint32_t            previous_path_ms;
    uint32_t            first_fault_ms;
    robot_app_faults_t  faults;
    robot_app_vehicle_t vehicle;
    robot_app_line_t    line;
    motion_snapshot_t   motion;
    terrain_shared_t    terrain;
    scanner_shared_t    scan;
    robot_app_front_t   front;
    /* Motion publishes each signed centre path, in wrapping half-counts
     * (left + right counts), with its release stamp and keeps the previous
     * pair, in one critical section (robot_app_commit).
     */
    uint16_t            path_hc;
    uint16_t            previous_path_hc;
    uint8_t             ignored_commands;
    uint8_t             dropped_telemetry;
} robot_app_shared_t;

/** Scan task flags; one owner, so plain bit-field writes are safe. */
typedef struct
{
    bool b_busy          : 1;
    bool b_result_ready  : 1;
    bool b_front_pending : 1;
    bool b_fine_requested : 1;
    bool b_fine_reported : 1;
    bool b_no_echo       : 1;
} robot_app_scan_flags_t;

/** Vehicle task flags and the completion counters it last acted on. */
typedef struct
{
    bool     b_button_down    : 1;
    bool     b_button_latched : 1;
    bool     b_line_lock      : 1;
    uint32_t coarse_seen      : 2;
    uint32_t fine_seen        : 2;
} robot_app_vehicle_flags_t;

/* First cause reported for each mask bit; status bits never stop. */
static const robot_app_fault_t g_robot_app_causes[ROBOT_APP_MASK_BITS] =
{
    ROBOT_APP_FAULT_OWNER_TIMEOUT, ROBOT_APP_FAULT_ENCODER,
    ROBOT_APP_FAULT_MOTION, ROBOT_APP_FAULT_LEFT_STALL,
    ROBOT_APP_FAULT_RIGHT_STALL, ROBOT_APP_FAULT_MOTOR_OUTPUT,
    ROBOT_APP_FAULT_OPTICAL, ROBOT_APP_FAULT_OPTICAL,
    ROBOT_APP_FAULT_SUPERVISOR, ROBOT_APP_FAULT_IMU, ROBOT_APP_FAULT_TILT,
    ROBOT_APP_FAULT_TILT_UNAVAILABLE, ROBOT_APP_FAULT_COLLISION,
    ROBOT_APP_FAULT_SCAN, ROBOT_APP_FAULT_NONE, ROBOT_APP_FAULT_NONE
};

static robot_app_shared_t g_shared;
static motion_t g_motion;
static vehicle_t g_vehicle;
static line_ctrl_runtime_t g_line_runtime;
static line_ctrl_state_t g_line;
static barcode_runtime_t g_barcode_runtime;
static barcode_state_t g_barcode;
static terrain_state_t g_terrain;
static scanner_state_t g_scanner;
static core_pose_packed_t g_route_anchor;
static core_pose_packed_t g_front_trigger_pose;
static uint64_t g_motion_us;
static double g_half_count_m;
static double g_imu_speed_mps;
static uint32_t g_scan_centred_ms;
static uint32_t g_scan_started_ms;
static uint32_t g_left_stall_ms;
static uint32_t g_right_stall_ms;
static uint32_t g_stationary_since_ms;
static uint32_t g_imu_window_ms;
static uint32_t g_front_trigger_ms;
static uint32_t g_line_lock_ms;
static uint32_t g_button_since_ms;
static uint32_t g_optical_hc;
static uint16_t g_line_left_total;
static uint16_t g_line_right_total;
static uint16_t g_imu_path_hc;
static uint16_t g_imu_window_path_hc;
static uint8_t g_motion_version;
static uint8_t g_imu_run_id;
static uint8_t g_telemetry_sequence;
static robot_app_scan_flags_t g_scan_flags;
static robot_app_vehicle_flags_t g_vehicle_flags;
static uint8_t g_rejected_seen;
static bool gb_optical_valid;
static bool gb_wifi_loss;

static void robot_app_copy (robot_app_shared_t * p_copy);
static void robot_app_latch_locked (bool b_hard, robot_app_fault_t cause,
                                    uint32_t mask, uint32_t source);
static void robot_app_fault_set (bool b_hard, robot_app_bit_t bit);
static void robot_app_stop_set (uint32_t source);
static void robot_app_status_set (robot_app_bit_t bit, bool b_active);
static void robot_app_count_drop (void);
static bool robot_app_active (vehicle_state_t state);
static bool robot_app_tilt_exceeded (const terrain_shared_t * p_terrain);
static bool robot_app_ready (const robot_app_shared_t * p_copy,
                             uint32_t now_ms);
static bool robot_app_motion_init (void);
static void robot_app_commit (const motion_output_t * p_output,
                              int32_t left_delta, int32_t right_delta,
                              uint32_t now_ms);
static void robot_app_motion_health (motion_output_t * p_output,
    int32_t left_delta, int32_t right_delta, uint32_t now_ms);
static void robot_app_reset (const robot_app_shared_t * p_copy);
static int32_t robot_app_signed_count (int32_t count, int32_t sign);
static int32_t robot_app_count_delta (uint16_t newer, uint16_t older);
static int32_t robot_app_path_delta (uint16_t newer, uint16_t older);
static bool robot_app_optical_path (void);
static bool robot_app_codebook_ready (void);
static void robot_app_terrain_run (const robot_app_shared_t * p_copy,
                                   uint16_t path_hc, uint32_t now_ms);
static void robot_app_perception (const robot_app_shared_t * p_copy,
                                  uint32_t now_ms);
static void robot_app_diagnostic_frame (const robot_app_shared_t * p_copy,
                                       uint32_t now_ms);
static bool robot_app_sensors_init (void);
static void robot_app_health (robot_app_health_t index, uint32_t now_ms);
static void robot_app_periodic (void (* p_step)(uint32_t), uint32_t period_ms);
static uint32_t robot_app_now (void);
static void robot_app_button (uint32_t now_ms, vehicle_state_t state);
static vehicle_nav_t robot_app_nav (barcode_action_t action);
static void robot_app_vehicle_input (const robot_app_shared_t * p_copy,
                                     uint32_t now_ms,
                                     vehicle_input_t * p_input);
static void robot_app_bypass_input (const robot_app_shared_t * p_copy,
                                    vehicle_input_t * p_input);
static bool robot_app_range_valid (const robot_app_shared_t * p_copy);
static bool robot_app_original_line (const robot_app_shared_t * p_copy,
                                     uint32_t now_ms);
static void robot_app_scan_cycle (const robot_app_shared_t * p_copy,
                                  uint32_t now_ms, scanner_echo_t echo);
static void robot_app_front_result (bool b_valid, uint32_t pulse_us);
static bool robot_app_servo (int32_t angle_deg);
static int16_t robot_app_wire_signed (uint32_t field, uint32_t width);
static uint32_t robot_app_age (uint32_t now_ms, uint32_t stamp_ms);
static bool robot_app_imu_path (const robot_app_shared_t * p_copy,
                                uint32_t now_ms, uint16_t * p_path_hc);
static void robot_app_imu_publish (const terrain_output_t * p_output,
                                   const robot_app_shared_t * p_copy,
                                   bool b_valid, uint32_t now_ms);

/** @brief Initialize statically-owned modules, then bounded network queues.
 * Runs before any task starts, so g_shared needs no lock here; the GPIO ISR
 * never writes g_shared.
 */
bool robot_app_init (void)
{
    vehicle_config_t config = {0};
    uint32_t now_ms = robot_app_now();
    uint32_t index = 0U;
    bool b_ready = true;

    memset(&g_shared, 0, sizeof(g_shared));
    /* All-zero fields already mean VEHICLE_INIT, an UNKNOWN front range,
     * invalid tilt, no fault, no stop source and no pending event.
     */
    g_shared.faults.b_inhibit = true;
    for (index = 0U; index < (uint32_t)ROBOT_APP_HEALTH_COUNT; ++index)
    {
        g_shared.health[index] = now_ms;
    }
    barcode_init(&g_barcode);
    app_cfg_vehicle(&config,
        robot_app_codebook_ready());
    b_ready = vehicle_init(&g_vehicle, &config);
    b_ready = robot_app_motion_init() && b_ready;
    b_ready = robot_app_sensors_init() && b_ready;
    b_ready = net_mqtt_start() && b_ready;
    g_shared.faults.b_inhibit = !g_motion.config.b_calibration_confirmed;
    g_shared.line_ms = now_ms;
    g_shared.imu_ms = now_ms;
    g_shared.tilt_ms = now_ms;
    g_shared.path_ms = now_ms;
    g_shared.previous_path_ms = now_ms;
    g_stationary_since_ms = now_ms;
    g_imu_window_ms = now_ms;
    g_vehicle_flags.b_button_latched = board_start_pressed();
    g_vehicle_flags.b_button_down = g_vehicle_flags.b_button_latched;
    board_encoder_totals(&g_line_left_total, &g_line_right_total);
    /* Counts since board_init() would otherwise saturate the 6-bit ISR
     * deltas into a false hard ENCODER fault on the first Motion read.
     */
    board_encoders_discard();
    g_motion_us = board_time_us();
    g_scan_centred_ms = now_ms;
    return b_ready;
}

/** @brief A bounded snapshot of the first cause, mask and stop sources. */
void robot_app_diagnostic (robot_app_diagnostic_t * p_output)
{
    robot_app_faults_t faults;
    uint32_t first_fault_ms = 0U;
    uint32_t saved = 0U;

    if (NULL != p_output)
    {
        saved = app_main_lock();
        faults = g_shared.faults;
        first_fault_ms = g_shared.first_fault_ms;
        app_main_unlock(saved);
        memset(p_output, 0, sizeof(*p_output));
        p_output->first_fault_ms = first_fault_ms;
        /* The mask field holds exactly sixteen bits. */
        p_output->mask = (uint16_t)faults.mask;
        p_output->code = faults.first;
        p_output->stop_source = faults.stop_source & ROBOT_APP_SOURCE_MASK;
    }
}

/** @brief STOP preempts normal output; stale local owners latch a hard
 * fault. */
void robot_app_safety_step (uint32_t now_ms)
{
    terrain_shared_t terrain;
    uint32_t health[ROBOT_APP_HEALTH_COUNT] = {0U};
    uint32_t tilt_ms = 0U;
    vehicle_state_t state = VEHICLE_IDLE;
    uint32_t saved = 0U;
    bool b_stale = false;

    /* Only the fields Safety checks, in one short masked region: this runs
     * every 2 ms at the highest priority, so it does not copy the record.
     */
    saved = app_main_lock();
    terrain = g_shared.terrain;
    (void)memcpy(health, g_shared.health, sizeof(health));
    tilt_ms = g_shared.tilt_ms;
    state = g_shared.vehicle.state;
    app_main_unlock(saved);
    if (board_stop_pressed())
    {
        robot_app_stop_set(ROBOT_APP_STOP_LOCAL);
    }
    /* A valid measured limit violation remains hard even after a soft halt. */
    if (terrain.b_tilt_valid && robot_app_tilt_exceeded(&terrain))
    {
        robot_app_fault_set(true, ROBOT_APP_BIT_TILT);
    }
    if (robot_app_active(state))
    {
        b_stale = (robot_app_age(now_ms,
                       health[ROBOT_APP_HEALTH_MOTION]) > 20U) ||
                  (robot_app_age(now_ms,
                       health[ROBOT_APP_HEALTH_LINE]) > 20U) ||
                  (robot_app_age(now_ms,
                       health[ROBOT_APP_HEALTH_VEHICLE]) > 40U) ||
                  (robot_app_age(now_ms,
                       health[ROBOT_APP_HEALTH_IMU]) > 100U) ||
                  (robot_app_age(now_ms,
                       health[ROBOT_APP_HEALTH_SCAN]) > 100U);
        if (b_stale || terrain.b_collision_candidate)
        {
            robot_app_fault_set(true, b_stale ?
                ROBOT_APP_BIT_OWNER_TIMEOUT : ROBOT_APP_BIT_COLLISION);
        }
        else if ((!terrain.b_tilt_valid) ||
                 (robot_app_age(now_ms, tilt_ms) >
                  ROBOT_APP_TILT_TIMEOUT_MS))
        {
            robot_app_fault_set(false, ROBOT_APP_BIT_TILT_UNAVAILABLE);
        }
    }
    /* Motion, Line, Vehicle, IMU, Scan and Comm gate the feed.
     * Telemetry and the radio owner that receives MQTT STOP (cyw43_utk_task,
     * P9) do not: remote STOP is best-effort.
     */
    if ((robot_app_age(now_ms, health[ROBOT_APP_HEALTH_MOTION]) <=
         20U) &&
        (robot_app_age(now_ms, health[ROBOT_APP_HEALTH_LINE]) <= 20U) &&
        (robot_app_age(now_ms, health[ROBOT_APP_HEALTH_VEHICLE]) <=
         40U) &&
        (robot_app_age(now_ms, health[ROBOT_APP_HEALTH_IMU]) <= 100U) &&
        (robot_app_age(now_ms, health[ROBOT_APP_HEALTH_SCAN]) <=
         100U) &&
        (robot_app_age(now_ms, health[ROBOT_APP_HEALTH_COMM]) <= 100U))
    {
        board_watchdog_feed();
    }
}

/** @brief One encoder/PID owner; a renewal is accepted only once per
 * publish. */
void robot_app_motion_step (uint32_t now_ms)
{
    robot_app_shared_t copy = {0};
    motion_output_t output = {0};
    motion_request_t request = {0};
    int32_t left_delta = 0;
    int32_t right_delta = 0;
    uint32_t errors = 0U;
    uint64_t now_us = board_time_us();
    double dt_s = (double)(now_us - g_motion_us) / 1000000.0;
    motion_status_t status = MOTION_OK;

    /* A bounded microsecond interval is accurately rounded to binary64. */
    g_motion_us = now_us;
    robot_app_copy(&copy);
    board_read_encoders(&left_delta, &right_delta, &errors);
    left_delta = robot_app_signed_count(left_delta,
                                        ROBOT_LEFT_ENCODER_SIGN);
    right_delta = robot_app_signed_count(right_delta,
                                         ROBOT_RIGHT_ENCODER_SIGN);
    if (0U != errors)
    {
        robot_app_fault_set(true, ROBOT_APP_BIT_ENCODER);
        copy.faults.b_inhibit = true;
    }
    if ((copy.faults.b_inhibit) &&
        (MOTION_INHIBITED != g_motion.output.state))
    {
        motion_stop(&g_motion, MOTION_STOPPED);
    }
    if (copy.vehicle.b_reset_motion)
    {
        robot_app_reset(&copy);
    }
    if (g_motion_version != copy.vehicle.request_version)
    {
        /* The version field holds four bits, which fit uint8_t. */
        g_motion_version = (uint8_t)copy.vehicle.request_version;
        motion_request_unpack(&copy.vehicle.request, &request);
        status = motion_submit(&g_motion, &request, now_ms);
        if ((MOTION_OK != status) && (!copy.faults.b_inhibit))
        {
            robot_app_fault_set(true, ROBOT_APP_BIT_MOTION);
        }
    }
    status = motion_step(&g_motion, now_ms, dt_s,
                         left_delta, right_delta, &output);
    if ((MOTION_OK != status) && (MOTION_STOPPED != status) &&
        (MOTION_REJECTED != status))
    {
        robot_app_fault_set(true, ROBOT_APP_BIT_MOTION);
    }
    robot_app_motion_health(&output, left_delta, right_delta, now_ms);
    robot_app_commit(&output, left_delta, right_delta, now_ms);
}

/** @brief Decode only during forward following; reverse/turn resets framing.
 * A frame still open when FOLLOW ends (for example at a RANGE_WAIT pause) is
 * discarded and counted as rejected, so br accounts for the lost code. This
 * includes a frame opened by a dark non-barcode feature, such as a junction
 * crossbar under the side sensor as NAVIGATE starts; Buddy 3 decides whether
 * that should count (T-55, T-06).
 */
void robot_app_line_step (uint32_t now_ms)
{
    robot_app_shared_t copy = {0};
    line_ctrl_output_t output = {0};
    barcode_output_t code = {0};
    uint16_t raw[LINE_CTRL_CHANNELS] = {0};
    uint32_t saved = 0U;
    bool b_valid = board_read_ir(raw);
    bool b_frame_lost = false;
    bool b_overrun = false;

    robot_app_copy(&copy);
    b_valid = line_ctrl_step(&g_line_runtime, &g_line, raw, now_ms,
                             &output) && b_valid;
    b_valid = robot_app_optical_path() && b_valid;
    if (!b_valid)
    {
        output.status = LINE_CTRL_FAULT;
    }
    if (b_valid && (VEHICLE_FOLLOW == copy.vehicle.state))
    {
        b_valid = barcode_step(&g_barcode_runtime, &g_barcode,
            barcode_classify(&g_barcode_runtime, raw[2]), g_optical_hc,
            now_ms, &code);
    }
    else
    {
        b_frame_lost = g_barcode.b_frame;
        barcode_init(&g_barcode);
    }
    saved = app_main_lock();
    g_shared.line.status = output.status;
    g_shared.line.steering_q7 = bitpack_signed(output.steering_q7,
                                               ROBOT_APP_STEER_BITS) & 0xFFU;
    if (((VEHICLE_FOLLOW == copy.vehicle.state) ||
         (VEHICLE_RANGE_WAIT == copy.vehicle.state)) && output.b_junction_edge)
    {
        /* The 2 ms producer must retain an edge for the 10 ms consumer. */
        g_shared.line.b_junction_edge = true;
    }
    if ((BARCODE_EVENT_UNKNOWN == code.event) &&
        (UINT8_MAX != g_shared.line.barcode_unknown))
    {
        g_shared.line.barcode_unknown =
            (g_shared.line.barcode_unknown + 1U) & UINT8_MAX;
    }
    if (((BARCODE_EVENT_REJECTED == code.event) || b_frame_lost) &&
        (UINT8_MAX != g_shared.line.barcode_rejected))
    {
        g_shared.line.barcode_rejected =
            (g_shared.line.barcode_rejected + 1U) & UINT8_MAX;
    }
    if (b_valid)
    {
        g_shared.line_ms = now_ms;
    }
    if (BARCODE_EVENT_DECODED == code.event)
    {
        b_overrun = VEHICLE_NAV_NONE != g_shared.line.navigation;
        if (!b_overrun)
        {
            g_shared.line.navigation = robot_app_nav(code.action);
            g_shared.line.barcode_action = code.action;
            g_shared.line.barcode_symbol = code.symbol & 0xFU;
        }
    }
    g_shared.health[ROBOT_APP_HEALTH_LINE] = now_ms;
    app_main_unlock(saved);
    if (b_overrun)
    {
        /* A second decode before Vehicle took the first loses route data. */
        robot_app_fault_set(true, ROBOT_APP_BIT_BARCODE_OVERRUN);
    }
    /* A valid optical sample with an unmatched frame is not sensor failure. */
    if ((!b_valid) && (robot_app_active(copy.vehicle.state)))
    {
        robot_app_fault_set(true, ROBOT_APP_BIT_OPTICAL_SAMPLE);
    }
}

/** @brief Only local button edges start/reset; barcodes provide navigation. */
void robot_app_vehicle_step (uint32_t now_ms)
{
    robot_app_shared_t copy = {0};
    vehicle_input_t input = {0};
    vehicle_output_t output = {0};
    motion_request_packed_t request = {0};
    uint32_t saved = 0U;

    robot_app_copy(&copy);
    robot_app_button(now_ms, copy.vehicle.state);
    saved = app_main_lock();
    copy = g_shared;
    g_shared.vehicle.b_start = false;
    g_shared.vehicle.b_reset = false;
    g_shared.line.navigation = VEHICLE_NAV_NONE;
    g_shared.line.b_junction_edge = false;
    g_shared.faults.b_stop_since_snapshot = false;
    app_main_unlock(saved);
    robot_app_vehicle_input(&copy, now_ms, &input);
    /* Completion events are consumed by remembering the counters acted on. */
    g_vehicle_flags.coarse_seen = copy.front.coarse_done & ROBOT_APP_DONE_MASK;
    g_vehicle_flags.fine_seen = copy.front.fine_done & ROBOT_APP_DONE_MASK;
    if (!vehicle_step(&g_vehicle, now_ms, &input, &output))
    {
        robot_app_fault_set(true, ROBOT_APP_BIT_SUPERVISOR);
    }
    else if ((VEHICLE_FAULT == output.state) &&
             (VEHICLE_FAULT != copy.vehicle.state))
    {
        saved = app_main_lock();
        if (input.b_stop && (!copy.faults.b_stop_latched) &&
            (!copy.faults.b_hard))
        {
            /* Only GP20 explains the stop: Vehicle read it before Safety's
             * next poll, so record the local STOP itself.
             */
            robot_app_latch_locked(false, ROBOT_APP_FAULT_LOCAL_STOP, 0U,
                                   ROBOT_APP_STOP_LOCAL);
        }
        else
        {
            /* FAULT entered because a stop is already latched keeps the old
             * first-cause rule but is not a supervisor fault: no mask bit.
             */
            robot_app_latch_locked(false, ROBOT_APP_FAULT_SUPERVISOR,
                input.b_stop ? 0U :
                (UINT32_C(1) << (uint32_t)ROBOT_APP_BIT_SUPERVISOR), 0U);
        }
        app_main_unlock(saved);
    }
    if (((VEHICLE_SCAN == output.state) &&
         (VEHICLE_SCAN != copy.vehicle.state)) ||
        ((VEHICLE_REACQUIRE == output.state) &&
         (VEHICLE_NAVIGATE == copy.vehicle.state)))
    {
        /* Scan entry, or a completed turn, defines the expected heading. */
        g_route_anchor = copy.motion.pose;
    }
    if (output.b_issue_motion)
    {
        motion_request_pack(&output.request, &request);
    }
    saved = app_main_lock();
    if ((VEHICLE_FOLLOW == output.state) &&
        (VEHICLE_IDLE == copy.vehicle.state))
    {
        g_shared.vehicle.run_id = (g_shared.vehicle.run_id + 1U) & UINT8_MAX;
        g_shared.line.barcode_action = BARCODE_ACTION_UNKNOWN;
        g_shared.line.barcode_symbol = 0U;
        g_shared.scan.b_profile_valid = false;
    }
    g_shared.vehicle.state = output.state;
    g_shared.vehicle.scan_request = output.scan;
    if (output.b_issue_motion)
    {
        g_shared.vehicle.request = request;
        g_shared.vehicle.request_version =
            (g_shared.vehicle.request_version + 1U) & ROBOT_APP_VERSION_MASK;
    }
    if (output.b_reset_motion)
    {
        /* Clean only if no stop was raised since this step's snapshot. */
        g_shared.vehicle.b_reset_motion = true;
        g_shared.faults.b_reset_clean =
            !g_shared.faults.b_stop_since_snapshot;
    }
    if (output.b_stop)
    {
        /* Supervisor stop: inhibit without recording a fault cause. */
        g_shared.faults.b_inhibit = true;
        board_motor_brake();
    }
    g_shared.health[ROBOT_APP_HEALTH_VEHICLE] = now_ms;
    app_main_unlock(saved);
}

/** @brief Fresh calibrated accelerometer sample with encoder path interval. */
void robot_app_imu_step (uint32_t now_ms)
{
    robot_app_shared_t copy = {0};
    terrain_input_t input = {0};
    terrain_output_t output = {0};
    motion_output_t motion = {0};
    int16_t raw[3] = {0};
    double axes[3] = {0.0};
    double speed_mps = 0.0;
    uint16_t path_hc = 0U;
    uint32_t saved = 0U;
    board_accel_status_t sample_status = board_read_accel(raw);
    bool b_valid = BOARD_ACCEL_FRESH == sample_status;

    robot_app_copy(&copy);
    /* Independent sensor clock: PENDING keeps its age; an overrun is FRESH,
     * dt spanning the lost sample. Gaps beyond the dt bound cannot drive.
     */
    input.dt_s = (double)(now_ms - copy.imu_ms) / 1000.0;
    if ((BOARD_ACCEL_PENDING == sample_status) &&
        (input.dt_s <= g_terrain.config.maximum_dt_s))
    {
        robot_app_health(ROBOT_APP_HEALTH_IMU, now_ms);
        return;
    }
    if (!b_valid)
    {
        saved = app_main_lock();
        g_shared.terrain.b_tilt_valid = false;
        g_shared.health[ROBOT_APP_HEALTH_IMU] = now_ms;
        app_main_unlock(saved);
        if (robot_app_active(copy.vehicle.state))
        {
            robot_app_fault_set(BOARD_ACCEL_PENDING != sample_status,
                (BOARD_ACCEL_PENDING == sample_status) ?
                ROBOT_APP_BIT_TILT_UNAVAILABLE : ROBOT_APP_BIT_IMU);
        }
        return;
    }
    input.b_encoder_valid = robot_app_imu_path(&copy, now_ms, &path_hc);
    robot_app_terrain_run(&copy, path_hc, now_ms);
    /* int16_t mg and bias hypotheses are exactly representable in double. */
    axes[0] = ((double)raw[0] - ROBOT_ACCEL_BIAS_X_MG)
              * ROBOT_ACCEL_SCALE_X * ROBOT_APP_MG_TO_MPS2;
    axes[1] = ((double)raw[1] - ROBOT_ACCEL_BIAS_Y_MG)
              * ROBOT_ACCEL_SCALE_Y * ROBOT_APP_MG_TO_MPS2;
    axes[2] = ((double)raw[2] - ROBOT_ACCEL_BIAS_Z_MG)
              * ROBOT_ACCEL_SCALE_Z * ROBOT_APP_MG_TO_MPS2;
    input.accel_x_mps2 = axes[ROBOT_IMU_FORWARD_AXIS]
                        * ROBOT_IMU_FORWARD_SIGN;
    input.accel_y_mps2 = axes[ROBOT_IMU_LEFT_AXIS] * ROBOT_IMU_LEFT_SIGN;
    input.accel_z_mps2 = axes[ROBOT_IMU_UP_AXIS] * ROBOT_IMU_UP_SIGN;
    /* PENDING returned before the baseline moved: no interval is lost. */
    input.signed_distance_m =
        (double)robot_app_path_delta(path_hc, g_imu_path_hc) * g_half_count_m;
    motion_snapshot_unpack(&copy.motion, &motion);
    input.encoder_yaw_rate_rps =
        (motion.right_speed_mps - motion.left_speed_mps) / ROBOT_TRACK_WIDTH_M;
    speed_mps = (motion.left_speed_mps + motion.right_speed_mps) / 2.0;
    input.b_sensor_valid = b_valid;
    /* A 200ms displacement window reduces count quantization in speed.
     * The acceleration threshold is a tuning hypothesis, not gyro evidence.
     */
    if ((now_ms - g_imu_window_ms) >= 200U)
    {
        g_imu_speed_mps = ((double)robot_app_path_delta(path_hc,
                               g_imu_window_path_hc) * g_half_count_m) /
                          ((double)(now_ms - g_imu_window_ms) / 1000.0);
        g_imu_window_path_hc = path_hc;
        g_imu_window_ms = now_ms;
    }
    input.b_quasistatic = motion.b_stationary ||
        ((fabs(speed_mps - g_imu_speed_mps) < 0.15) &&
         (fabs(input.accel_x_mps2) < 4.0));
    g_imu_path_hc = path_hc;
    b_valid = terrain_step(&g_terrain, &input, &output) && b_valid;
    robot_app_imu_publish(&output, &copy, b_valid, now_ms);
    if ((!b_valid) && (robot_app_active(copy.vehicle.state)))
    {
        robot_app_fault_set(true, ROBOT_APP_BIT_IMU);
    }
    else if ((!output.b_tilt_valid) && robot_app_active(copy.vehicle.state))
    {
        /* Unusable orientation brakes; recovery needs local reset. */
        robot_app_fault_set(false, ROBOT_APP_BIT_TILT_UNAVAILABLE);
    }
}

/** @brief One servo/sonar owner; frontal polling resumes after scan
 * settling. */
void robot_app_scan_step (uint32_t now_ms)
{
    robot_app_shared_t copy = {0};
    scanner_echo_t echo = {0};
    uint32_t saved = 0U;
    bool b_valid = false;

    robot_app_copy(&copy);
    if (VEHICLE_SCAN_FINE == copy.vehicle.scan_request)
    {
        g_scan_flags.b_fine_requested = true;
    }
    board_ultrasonic_service();
    echo.b_ready = board_ultrasonic_result(&echo.pulse_us, &b_valid);
    if (!b_valid)
    {
        echo.pulse_us = 0U;
    }
    if ((!g_scan_flags.b_busy) &&
        (VEHICLE_SCAN_COARSE == copy.vehicle.scan_request) &&
        (!g_scan_flags.b_result_ready))
    {
        g_scan_flags.b_busy = scanner_start(&g_scanner, now_ms,
                                            copy.motion.b_stationary);
        g_scan_started_ms = now_ms;
        g_scan_flags.b_front_pending = false;
    }
    if ((copy.faults.b_stop_latched) || (copy.faults.b_hard))
    {
        if (g_scan_flags.b_busy)
        {
            g_scan_flags.b_busy = false;
            g_scanner.phase = SCANNER_ABORTED;
            g_scan_centred_ms = now_ms;
            (void)robot_app_servo(0);
        }
    }
    if (g_scan_flags.b_busy)
    {
        robot_app_scan_cycle(&copy, now_ms, echo);
    }
    else if ((now_ms - g_scan_centred_ms) >= ROBOT_SERVO_SETTLE_MS)
    {
        if (echo.b_ready && g_scan_flags.b_front_pending)
        {
            g_scan_flags.b_front_pending = false;
            robot_app_front_result(b_valid, echo.pulse_us);
        }
        /* A false return means the board's bounded interval is not ready. */
        if (board_ultrasonic_trigger())
        {
            g_scan_flags.b_front_pending = true;
            g_front_trigger_pose = copy.motion.pose;
            g_front_trigger_ms = now_ms;
        }
    }
    if (g_scan_flags.b_fine_requested && g_scan_flags.b_result_ready &&
        (!g_scan_flags.b_fine_reported) &&
        ((now_ms - g_scan_centred_ms) >= ROBOT_SERVO_SETTLE_MS) &&
        (CORE_RANGE_VALID == copy.front.range.state) &&
        ((now_ms - copy.range_ms) < 150U))
    {
        /* One event per request: a 2-bit counter bumped every 2 ms could
         * alias back to the value Vehicle last saw between its 10 ms reads.
         */
        g_scan_flags.b_fine_reported = true;
        saved = app_main_lock();
        g_shared.front.fine_done =
            (g_shared.front.fine_done + 1U) & ROBOT_APP_DONE_MASK;
        app_main_unlock(saved);
    }
    if (VEHICLE_SCAN != copy.vehicle.state)
    {
        g_scan_flags.b_result_ready = false;
        g_scan_flags.b_fine_requested = false;
        g_scan_flags.b_fine_reported = false;
    }
    robot_app_health(ROBOT_APP_HEALTH_SCAN, now_ms);
}

/** @brief Receive at most one command byte; only STOP changes the mission.
 * Valid non-STOP commands and payloads the radio rejected as malformed are
 * both counted as ignored. The radio owner's session flag is mirrored into
 * WIFI_LOSS through the locked status helper, only when it changes.
 */
void robot_app_comm_step (uint32_t now_ms)
{
    uint8_t command = 0U;
    uint8_t rejected = net_mqtt_rejected();
    /* Modulo-256 difference of the radio's wrapping counter. */
    uint32_t ignored = (uint32_t)(uint8_t)(rejected - g_rejected_seen);
    uint32_t saved = 0U;
    bool b_wifi_loss = !net_mqtt_online();

    g_rejected_seen = rejected;
    if (net_mqtt_receive(&command))
    {
        if (MQTTCMD_STOP == mqttcmd_code(command))
        {
            robot_app_stop_set(ROBOT_APP_STOP_REMOTE);
        }
        else
        {
            ++ignored;
        }
    }
    if (0U != ignored)
    {
        saved = app_main_lock();
        g_shared.ignored_commands = (uint8_t)bitpack_saturate(
            g_shared.ignored_commands + ignored, ROBOT_APP_BYTE_BITS);
        app_main_unlock(saved);
    }
    if (b_wifi_loss != gb_wifi_loss)
    {
        gb_wifi_loss = b_wifi_loss;
        robot_app_status_set(ROBOT_APP_BIT_WIFI_LOSS, b_wifi_loss);
    }
    robot_app_health(ROBOT_APP_HEALTH_COMM, now_ms);
}

/** @brief Binary frames in fixed minimum-width fields; no text formatting. */
void robot_app_telemetry_step (uint32_t now_ms)
{
    robot_app_shared_t copy = {0};
    telemetry_status_t status;
    uint8_t frame[TELEMETRY_FRAME_MAX] = {0};
    size_t length = 0U;

    robot_app_copy(&copy);
    /* Wrapping at 250 keeps "every fifth frame" true across the wrap. */
    g_telemetry_sequence = (uint8_t)((g_telemetry_sequence + 1U) %
                                     TELEMETRY_SEQUENCE_MODULO);
    memset(&status, 0, sizeof(status));
    status.seq = g_telemetry_sequence;
    status.now_ms = now_ms;
    status.dropped = copy.dropped_telemetry;
    status.ignored = copy.ignored_commands;
    /* Saturated to the 10-bit field, which fits uint16_t. */
    status.range_age_ms = (uint16_t)bitpack_saturate(
        robot_app_age(now_ms, copy.range_ms), ROBOT_APP_AGE_BITS);
    /* Enumerators 0..8 fit the 4-bit state field. */
    status.state = (uint8_t)copy.vehicle.state;
    status.vleft_mm_s = robot_app_wire_signed(copy.motion.left_mm_s,
                                              ROBOT_APP_SPEED_BITS);
    status.vright_mm_s = robot_app_wire_signed(copy.motion.right_mm_s,
                                               ROBOT_APP_SPEED_BITS);
    status.pitch_mrad = robot_app_wire_signed(copy.terrain.pitch_mrad,
                                              ROBOT_APP_ANGLE_BITS);
    /* Nine- and twelve-bit fields fit uint16_t exactly. */
    status.peak_mm = (uint16_t)copy.terrain.run_maximum_mm;
    status.range_mm = (uint16_t)copy.front.range.distance_mm;
    status.b_stop = copy.faults.b_inhibit;
    status.b_hardfault = copy.faults.b_hard;
    /* hv reports the last completed hump; pk > 0 is already a valid peak. */
    status.b_heightvalid = copy.terrain.b_last_hump_valid;
    status.b_rangevalid = CORE_RANGE_VALID == copy.front.range.state;
    length = telemetry_encode_status(&status, frame, sizeof(frame));
    if ((0U == length) || (!net_mqtt_send(frame, length)))
    {
        robot_app_count_drop();
    }
    if (0U == (g_telemetry_sequence % 5U))
    {
        robot_app_perception(&copy, now_ms);
        robot_app_diagnostic_frame(&copy, now_ms);
    }
}

/** @brief Safety owner with absolute-deadline periodic release. */
void robot_app_safety_task (void)
{
    robot_app_periodic(robot_app_safety_step, 2U);
}

/** @brief Motion owner with absolute-deadline periodic release. */
void robot_app_motion_task (void)
{
    robot_app_periodic(robot_app_motion_step, 5U);
}

/** @brief Line owner with absolute-deadline periodic release. */
void robot_app_line_task (void)
{
    robot_app_periodic(robot_app_line_step, 2U);
}

/** @brief Vehicle owner with absolute-deadline periodic release. */
void robot_app_vehicle_task (void)
{
    robot_app_periodic(robot_app_vehicle_step, 10U);
}

/** @brief Imu owner with absolute-deadline periodic release. */
void robot_app_imu_task (void)
{
    robot_app_periodic(robot_app_imu_step, 10U);
}

/** @brief Scan owner with absolute-deadline periodic release. */
void robot_app_scan_task (void)
{
    robot_app_periodic(robot_app_scan_step, 2U);
}

/** @brief Comm owner with absolute-deadline periodic release. */
void robot_app_comm_task (void)
{
    robot_app_periodic(robot_app_comm_step, 5U);
}

/** @brief Telemetry owner with absolute-deadline periodic release. */
void robot_app_telemetry_task (void)
{
    robot_app_periodic(robot_app_telemetry_step, 200U);
}

/** @brief Snapshot copy has no kernel wait or peripheral transaction. */
static void robot_app_copy (robot_app_shared_t * p_copy)
{
    uint32_t saved = app_main_lock();

    *p_copy = g_shared;
    app_main_unlock(saved);
}

/** @brief Record one stop or stopping fault; the caller holds the lock.
 * First cause and time if none yet, mask and stop-source bits, the hard
 * latch, inhibition, the stop latch and the reset handshake flags, then an
 * immediate brake.
 */
static void robot_app_latch_locked (bool b_hard, robot_app_fault_t cause,
                                    uint32_t mask, uint32_t source)
{
    if (ROBOT_APP_FAULT_NONE == g_shared.faults.first)
    {
        g_shared.faults.first = cause;
        g_shared.first_fault_ms = robot_app_now();
    }
    g_shared.faults.mask = (g_shared.faults.mask | mask) & UINT16_MAX;
    g_shared.faults.stop_source =
        (g_shared.faults.stop_source | source) & ROBOT_APP_SOURCE_MASK;
    g_shared.faults.b_hard = g_shared.faults.b_hard || b_hard;
    g_shared.faults.b_inhibit = true;
    g_shared.faults.b_stop_latched = true;
    g_shared.faults.b_stop_since_snapshot = true;
    g_shared.faults.b_reset_clean = false;
    board_motor_brake();
}

/** @brief Sticky software stop and immediate output commit share one lock;
 * the first cause comes from the bit's entry in g_robot_app_causes.
 */
static void robot_app_fault_set (bool b_hard, robot_app_bit_t bit)
{
    uint32_t saved = app_main_lock();

    robot_app_latch_locked(b_hard, g_robot_app_causes[bit],
                           UINT32_C(1) << (uint32_t)bit, 0U);
    app_main_unlock(saved);
}

/** @brief LOCAL or REMOTE STOP is a soft stop recorded as its source. */
static void robot_app_stop_set (uint32_t source)
{
    uint32_t saved = app_main_lock();

    robot_app_latch_locked(false, (ROBOT_APP_STOP_LOCAL == source) ?
                           ROBOT_APP_FAULT_LOCAL_STOP :
                           ROBOT_APP_FAULT_REMOTE_STOP, 0U, source);
    app_main_unlock(saved);
}

/** @brief Non-stopping status bit set or cleared by its single owner; never
 * touches the first cause, inhibition, the stop latch or the handshake.
 */
static void robot_app_status_set (robot_app_bit_t bit, bool b_active)
{
    uint32_t bit_mask = UINT32_C(1) << (uint32_t)bit;
    uint32_t saved = app_main_lock();

    if (b_active)
    {
        g_shared.faults.mask = (g_shared.faults.mask | bit_mask) & UINT16_MAX;
    }
    else
    {
        g_shared.faults.mask = g_shared.faults.mask & (~bit_mask & UINT16_MAX);
    }
    app_main_unlock(saved);
}

/** @brief Saturating count of telemetry frames that were not queued. */
static void robot_app_count_drop (void)
{
    uint32_t saved = app_main_lock();

    if (UINT8_MAX != g_shared.dropped_telemetry)
    {
        ++g_shared.dropped_telemetry;
    }
    app_main_unlock(saved);
}

/** @brief Active missions, including stopped retries, retain health gates. */
static bool robot_app_active (vehicle_state_t state)
{
    return (VEHICLE_FOLLOW == state) || (VEHICLE_NAVIGATE == state) ||
           (VEHICLE_SCAN == state) || (VEHICLE_BYPASS == state) ||
           (VEHICLE_REACQUIRE == state) || (VEHICLE_RANGE_WAIT == state);
}

/** @brief Integer form of fabs(angle) > ROBOT_TILT_LIMIT_RAD. Angles are
 * packed with magnitudes rounded up, which keeps that decision exact.
 */
static bool robot_app_tilt_exceeded (const terrain_shared_t * p_terrain)
{
    int32_t pitch = bitpack_extend(p_terrain->pitch_mrad,
                                   ROBOT_APP_ANGLE_BITS);
    int32_t roll = bitpack_extend(p_terrain->roll_mrad, ROBOT_APP_ANGLE_BITS);

    /* Fields are at least -2048, so negation cannot overflow. */
    return (((pitch < 0) ? -pitch : pitch) > ROBOT_TILT_LIMIT_MRAD) ||
           (((roll < 0) ? -roll : roll) > ROBOT_TILT_LIMIT_MRAD);
}

/** @brief Every drive authorization requires a fresh usable orientation. */
static bool robot_app_ready (const robot_app_shared_t * p_copy,
                             uint32_t now_ms)
{
    return (0 != ROBOT_CALIBRATION_CONFIRMED) &&
           (0 != ROBOT_IMU_MOUNT_CONFIRMED) &&
           (robot_app_age(now_ms, p_copy->line_ms) <= 20U) &&
           (robot_app_age(now_ms, p_copy->imu_ms) <= 100U) &&
           (robot_app_age(now_ms, p_copy->tilt_ms) <=
            ROBOT_APP_TILT_TIMEOUT_MS) &&
           p_copy->terrain.b_tilt_valid &&
           (!robot_app_tilt_exceeded(&p_copy->terrain)) &&
           (robot_app_age(now_ms,
                          p_copy->health[ROBOT_APP_HEALTH_MOTION]) <= 20U) &&
           (LINE_CTRL_FAULT != p_copy->line.status) &&
           (LINE_CTRL_INHIBITED != p_copy->line.status) &&
           (!p_copy->faults.b_hard);
}

/** @brief Unconfirmed calibration is a normal inhibited configuration. */
static bool robot_app_motion_init (void)
{
    motion_config_t config = {0};
    motion_status_t result = MOTION_INVALID;
    core_pose_t pose = {0.0, 0.0, 0.0, 0.0};
    core_odometry_result_t odometry;

    app_cfg_motion(&config);
    result = motion_init(&g_motion, &config);
    /* Metres per half-count (left + right counts) for the IMU path; the
     * optical path stays valid only with a usable odometry configuration.
     */
    gb_optical_valid = CORE_OK == core_odometry_step(
        &g_motion.config.odometry, &pose, 0, 0, &odometry);
    g_half_count_m = gb_optical_valid ?
        ((CORE_PI * (g_motion.config.odometry.left_radius_m +
                     g_motion.config.odometry.right_radius_m)) /
         (2.0 * (double)g_motion.config.odometry.left_counts_per_revolution))
        : 0.0;
    return (MOTION_OK == result) || (MOTION_REJECTED == result);
}

/** @brief Load measured reference parameters; never auto-calibrate on a
 * hump. */
static bool robot_app_sensors_init (void)
{
    terrain_config_t terrain_config = {0};
    scanner_config_t scan_config = {0};
    line_ctrl_config_t line_config = {0};
    bool b_ready = true;

    app_cfg_line(&line_config);
    line_ctrl_init(&g_line);
    app_cfg_terrain(&terrain_config);
    app_cfg_scanner(&scan_config);
    b_ready = terrain_init(&g_terrain, &terrain_config);
    b_ready = scanner_init(&g_scanner, &scan_config) && b_ready;
    b_ready = line_ctrl_derive(&line_config, &g_line_runtime) && b_ready;
    /* Needs the half-count quantum from robot_app_motion_init(). */
    b_ready = barcode_derive(course_cfg_codebook(), g_half_count_m,
                             &line_config, &g_barcode_runtime) && b_ready;
    if (terrain_config.b_calibrated)
    {
        /* Reference is a stored, reviewed stationary mean in vehicle axes. */
        b_ready = terrain_reference(&g_terrain,
            ROBOT_IMU_REFERENCE_X_MG * ROBOT_APP_MG_TO_MPS2,
            ROBOT_IMU_REFERENCE_Y_MG * ROBOT_APP_MG_TO_MPS2,
            ROBOT_IMU_REFERENCE_Z_MG * ROBOT_APP_MG_TO_MPS2, true)
            && b_ready;
    }
    if (0 != ROBOT_SERVO_CONFIRMED)
    {
        b_ready = robot_app_servo(0) && b_ready;
    }
    return b_ready;
}

/** @brief Publish liveness only after one complete activation. */
static void robot_app_health (robot_app_health_t index, uint32_t now_ms)
{
    uint32_t saved = app_main_lock();

    g_shared.health[index] = now_ms;
    app_main_unlock(saved);
}

/** @brief Absolute releases skip missed periods instead of a catch-up loop. */
static void robot_app_periodic (void (* p_step)(uint32_t), uint32_t period_ms)
{
    uint32_t next_ms = robot_app_now();
    uint32_t now_ms = next_ms;
    uint32_t remaining_ms = 0U;

    for (;;)
    {
        p_step(now_ms);
        next_ms += period_ms;
        now_ms = robot_app_now();
        remaining_ms = next_ms - now_ms;
        if ((0U == remaining_ms) ||
            (remaining_ms >= ROBOT_APP_SERIAL_HALF))
        {
            /* A missed period is skipped, not counted: the owner freshness
             * gates see the late step, which a tight catch-up dispatch would
             * hide. Measure jitter on the board.
             */
            next_ms = now_ms + period_ms;
            remaining_ms = period_ms;
        }
        app_main_delay(remaining_ms);
        now_ms = robot_app_now();
    }
}

/** @brief Millisecond serial clock deliberately wraps every 2^32 ms. */
static uint32_t robot_app_now (void)
{
    /* Unsigned narrowing intentionally implements modulo-2^32 milliseconds. */
    return (uint32_t)(board_time_us() / 1000U);
}

/** @brief One action per debounced press, with release required to rearm. */
static void robot_app_button (uint32_t now_ms, vehicle_state_t state)
{
    bool b_down = board_start_pressed();
    uint32_t saved = 0U;

    if (b_down != g_vehicle_flags.b_button_down)
    {
        g_vehicle_flags.b_button_down = b_down;
        g_button_since_ms = now_ms;
    }
    if ((now_ms - g_button_since_ms) < 30U)
    {
        return;
    }
    if (!b_down)
    {
        g_vehicle_flags.b_button_latched = false;
    }
    else if (!g_vehicle_flags.b_button_latched)
    {
        g_vehicle_flags.b_button_latched = true;
        saved = app_main_lock();
        if (VEHICLE_FAULT == state)
        {
            g_shared.vehicle.b_reset = true;
        }
        else if (VEHICLE_IDLE == state)
        {
            g_shared.vehicle.b_start = true;
        }
        else
        {
            /* A press during a manoeuvre cannot change autonomous intent. */
        }
        app_main_unlock(saved);
    }
    else
    {
        /* Held button cannot start or reset repeatedly. */
    }
}

/** @brief Mapping is from decoded action, never from illustrative A/B/C/D. */
static vehicle_nav_t robot_app_nav (barcode_action_t action)
{
    vehicle_nav_t navigation = VEHICLE_NAV_NONE;

    switch (action)
    {
        case BARCODE_ACTION_LEFT:
            navigation = VEHICLE_NAV_LEFT;
        break;
        case BARCODE_ACTION_RIGHT:
            navigation = VEHICLE_NAV_RIGHT;
        break;
        case BARCODE_ACTION_STRAIGHT:
            navigation = VEHICLE_NAV_STRAIGHT;
        break;
        case BARCODE_ACTION_UTURN:
            navigation = VEHICLE_NAV_UTURN;
        break;
        case BARCODE_ACTION_UNKNOWN:
        default:
            navigation = VEHICLE_NAV_NONE;
        break;
    }
    return navigation;
}

/** @brief Construct evidence gates from the packed shared records. */
static void robot_app_vehicle_input (const robot_app_shared_t * p_copy,
                                     uint32_t now_ms,
                                     vehicle_input_t * p_input)
{
    /* Q7 steering: -127..127 represents -1..1. */
    double line_steering = (double)bitpack_extend(p_copy->line.steering_q7,
                               ROBOT_APP_STEER_BITS) / ROBOT_APP_STEER_SCALE;
    double steering = line_steering * ROBOT_LINE_GAIN_RPS;

    if (steering > ROBOT_TURN_RATE_RPS)
    {
        steering = ROBOT_TURN_RATE_RPS;
    }
    if (steering < -ROBOT_TURN_RATE_RPS)
    {
        steering = -ROBOT_TURN_RATE_RPS;
    }
    motion_snapshot_unpack(&p_copy->motion, &p_input->motion);
    p_input->navigation = p_copy->line.navigation;
    p_input->b_junction_edge = p_copy->line.b_junction_edge;
    p_input->line_forward_mps = ROBOT_LINE_SPEED_MPS *
                               (1.0 - (0.5 * fabs(line_steering)));
    /* Optical positive-right becomes differential-drive positive-left yaw. */
    p_input->line_yaw_rps = -steering;
    p_input->b_start = p_copy->vehicle.b_start;
    p_input->b_reset = p_copy->vehicle.b_reset;
    p_input->b_stop = (p_copy->faults.b_hard) || (board_stop_pressed()) ||
        ((p_copy->faults.b_stop_latched) &&
         (VEHICLE_FAULT != p_copy->vehicle.state));
    p_input->b_fault_clear = (!p_copy->faults.b_hard) &&
                             (!board_stop_pressed());
    p_input->b_sensors_ready = robot_app_ready(p_copy, now_ms);
    p_input->b_line_valid = (LINE_CTRL_TRACKING == p_copy->line.status) ||
                            (LINE_CTRL_JUNCTION == p_copy->line.status);
    p_input->b_line_wait = LINE_CTRL_SEARCHING == p_copy->line.status;
    p_input->b_range_valid = robot_app_range_valid(p_copy);
    p_input->range_age_ms = robot_app_age(now_ms, p_copy->range_ms);
    /* Ceil-mm ranges keep distance_m <= 0.25 and > 0.25 decisions exact. */
    p_input->b_obstacle = p_input->b_range_valid &&
        (p_copy->front.range.distance_mm <= ROBOT_FRONT_STOP_DISTANCE_MM);
    p_input->b_front_clear = p_input->b_range_valid &&
        (p_copy->front.range.distance_mm > ROBOT_FRONT_STOP_DISTANCE_MM);
    p_input->b_coarse_ready =
        p_copy->front.coarse_done != g_vehicle_flags.coarse_seen;
    p_input->b_fine_ready =
        p_copy->front.fine_done != g_vehicle_flags.fine_seen;
    robot_app_bypass_input(p_copy, p_input);
    p_input->b_original_line_verified =
        robot_app_original_line(p_copy, now_ms);
}

/** @brief Bounded geometric bypass hypotheses from the packed profile. */
static void robot_app_bypass_input (const robot_app_shared_t * p_copy,
                                    vehicle_input_t * p_input)
{
    scanner_result_t result;

    scanner_shared_unpack(&p_copy->scan, &result);
    p_input->bypass_offset_m = (result.apparent_width_m / 2.0) +
        (ROBOT_ROBOT_WIDTH_M / 2.0) + ROBOT_OBSTACLE_MARGIN_M;
    p_input->bypass_length_m = result.nearest_distance_m +
        ROBOT_MAX_OBSTACLE_DEPTH_M + ROBOT_ROBOT_LENGTH_M +
        ROBOT_OBSTACLE_MARGIN_M;
    p_input->bypass_side = (SCANNER_SIDE_LEFT == result.preferred_side) ?
                           1 : -1;
    p_input->b_bypass_clearance = (0 != ROBOT_CORRIDOR_CONFIRMED) &&
        (result.b_profile_valid) && (result.b_obstacle_seen) &&
        (SCANNER_SIDE_NONE != result.preferred_side) &&
        (0U == result.unknown_samples);
}

/** @brief A qualified range taken at (nearly) the current pose. */
static bool robot_app_range_valid (const robot_app_shared_t * p_copy)
{
    core_real_t dx_m = 0.0;
    core_real_t dy_m = 0.0;
    core_real_t dyaw_rad = 0.0;

    core_pose_delta(&p_copy->motion.pose, &p_copy->front.pose, &dx_m,
                    &dy_m, &dyaw_rad);
    return (CORE_RANGE_VALID == p_copy->front.range.state) &&
           (fabs(dyaw_rad) <= 0.12) && (hypot(dx_m, dy_m) <= 0.03);
}

/** @brief Require known course corridor, heading and sustained optical lock. */
static bool robot_app_original_line (const robot_app_shared_t * p_copy,
                                     uint32_t now_ms)
{
    core_pose_t anchor;
    core_real_t delta_x = 0.0;
    core_real_t delta_y = 0.0;
    core_real_t yaw_error = 0.0;
    double lateral_m = 0.0;
    bool b_matches = false;

    core_pose_unpack(&g_route_anchor, &anchor);
    core_pose_delta(&p_copy->motion.pose, &g_route_anchor, &delta_x,
                    &delta_y, &yaw_error);
    lateral_m = (-sin(anchor.yaw_rad) * delta_x) +
                (cos(anchor.yaw_rad) * delta_y);
    b_matches = (0 != ROBOT_COURSE_CONFIRMED) &&
        (LINE_CTRL_TRACKING == p_copy->line.status) &&
        (fabs(lateral_m) <= ROBOT_LINE_UNIQUE_CORRIDOR_M) &&
        (fabs(yaw_error) <= ROBOT_LINE_HEADING_TOL_RAD);
    if ((!b_matches) || (VEHICLE_REACQUIRE != p_copy->vehicle.state))
    {
        g_vehicle_flags.b_line_lock = false;
        return false;
    }
    if (!g_vehicle_flags.b_line_lock)
    {
        g_vehicle_flags.b_line_lock = true;
        g_line_lock_ms = now_ms;
    }
    return (now_ms - g_line_lock_ms) >= ROBOT_LINE_LOCK_MS;
}

/** @brief Combined scanner completes both passes before staged events. */
static void robot_app_scan_cycle (const robot_app_shared_t * p_copy,
                                  uint32_t now_ms, scanner_echo_t echo)
{
    scanner_output_t output = {0};
    scanner_shared_t shared;
    uint32_t saved = 0U;
    bool b_ok = scanner_step(&g_scanner, now_ms,
                              p_copy->motion.b_stationary, &echo, &output);

    if (output.b_set_servo)
    {
        b_ok = robot_app_servo(output.angle_deg) && b_ok;
    }
    if (output.b_trigger)
    {
        b_ok = board_ultrasonic_trigger() && b_ok;
    }
    if ((!b_ok) || (g_scanner.result.b_aborted) ||
        ((now_ms - g_scan_started_ms) > 20000U))
    {
        robot_app_fault_set(true, ROBOT_APP_BIT_SCAN);
        g_scan_flags.b_busy = false;
        return;
    }
    if (output.b_done)
    {
        g_scan_flags.b_busy = false;
        g_scan_flags.b_result_ready = true;
        g_scan_centred_ms = now_ms;
        scanner_shared_pack(&g_scanner.result, &shared);
        saved = app_main_lock();
        g_shared.scan = shared;
        g_shared.front.coarse_done =
            (g_shared.front.coarse_done + 1U) & ROBOT_APP_DONE_MASK;
        g_shared.front.range.state = CORE_RANGE_UNKNOWN;
        g_shared.front.range.distance_mm = 0U;
        app_main_unlock(saved);
    }
}

/** @brief Publish one front reading with its trigger pose; NO_ECHO follows
 * the latest reading and changes only through the locked status helper.
 */
static void robot_app_front_result (bool b_valid, uint32_t pulse_us)
{
    core_range_sample_t range = {CORE_RANGE_UNKNOWN, 0.0};
    core_range_packed_t packed;
    uint32_t saved = 0U;

    /* uint32_t microseconds convert exactly, then scale to seconds. */
    (void)core_ultrasonic_range(&g_scanner.config.range, b_valid,
        (double)pulse_us / 1000000.0, &range);
    core_range_pack(&range, &packed);
    saved = app_main_lock();
    g_shared.front.range = packed;
    g_shared.range_ms = g_front_trigger_ms;
    g_shared.front.pose = g_front_trigger_pose;
    app_main_unlock(saved);
    if (b_valid == g_scan_flags.b_no_echo)
    {
        g_scan_flags.b_no_echo = !b_valid;
        robot_app_status_set(ROBOT_APP_BIT_NO_ECHO, !b_valid);
    }
}

/** @brief Calibrated zero and sign map a -60..60 beam sector to PWM. */
static bool robot_app_servo (int32_t angle_deg)
{
    int32_t pulse_us = 0;
    int32_t physical_deg = 0;
    int32_t span_us = 0;

    if ((angle_deg < -60) || (angle_deg > 60) ||
        ((1 != ROBOT_SERVO_ANGLE_SIGN) && (-1 != ROBOT_SERVO_ANGLE_SIGN)) ||
        (0U == ROBOT_SERVO_MIN_US) ||
        (ROBOT_SERVO_MIN_US >= ROBOT_SERVO_CENTRE_US) ||
        (ROBOT_SERVO_CENTRE_US >= ROBOT_SERVO_MAX_US) ||
        (ROBOT_SERVO_MAX_US > UINT16_MAX) ||
        (ROBOT_SERVO_MAX_US >= ROBOT_SERVO_FRAME_US))
    {
        return false;
    }
    physical_deg = angle_deg * ROBOT_SERVO_ANGLE_SIGN;
    /* Calibrated negative and positive endpoint spans need not be equal. */
    span_us = (physical_deg < 0) ?
        (int32_t)(ROBOT_SERVO_CENTRE_US - ROBOT_SERVO_MIN_US) :
        (int32_t)(ROBOT_SERVO_MAX_US - ROBOT_SERVO_CENTRE_US);
    pulse_us = (int32_t)ROBOT_SERVO_CENTRE_US +
        ((physical_deg * span_us) / 60);
    /* Validated endpoints bound the result within the uint16_t board API. */
    return board_servo_us((uint16_t)pulse_us);
}

/** @brief A signed shared field in wire form; the reserved code is invalid.
 */
static int16_t robot_app_wire_signed (uint32_t field, uint32_t width)
{
    if (bitpack_invalid(width) == field)
    {
        return TELEMETRY_INVALID;
    }
    /* Shared signed fields are at most 13 bits, so the value fits int16_t. */
    return (int16_t)bitpack_extend(field, width);
}

/** @brief Wrap-safe age of a shared stamp on the millisecond serial clock.
 * A step reads now_ms at release; a higher-priority owner may publish a
 * newer stamp before the step's snapshot (for example MotionTask or LineTask
 * during VehicleTask's button read). That stamp is fresh (age 0), not about
 * 2^32 ms stale. Genuine staleness below the serial half is unchanged. The
 * IMU step instead selects an encoder path sample (robot_app_imu_path).
 */
static uint32_t robot_app_age (uint32_t now_ms, uint32_t stamp_ms)
{
    uint32_t age = now_ms - stamp_ms;

    return (age >= ROBOT_APP_SERIAL_HALF) ? 0U : age;
}

/** @brief Encoder path at the last Motion publish at or before the IMU
 * release now_ms, so the path interval matches the release-based dt. A
 * publish during this step's blocking I2C read is newer than the release:
 * the previous sample is used instead, so one preemption creates no
 * path/time skew and no spurious deceleration. If even the previous sample
 * is newer (two publishes during one step), this step's encoder interval is
 * invalid and that sample becomes the next baseline. The following step is
 * still marked valid but can under-estimate its path by 5 ms (5 ms of path
 * over a 10 ms dt). After an invalid step it has no acceleration of its
 * own, and with forward-only motion the next step's recovery reads as
 * acceleration, so it cannot latch a false COLLISION; it can misclassify
 * that recovery step and dull its collision test. A sample older than
 * 20 ms is invalid. False means the terrain must not use this interval.
 * Residuals (T-53): the step after a double publish, as above; now_ms is
 * the step's start, so higher-priority work that delays the start past a
 * Motion publish still skews one interval; and a 10-20 ms Motion publish
 * gap gives a valid zero-distance interval, then a valid double-distance
 * one, which with a coincident longitudinal transient below -3 m/s^2 can
 * latch a false COLLISION.
 */
static bool robot_app_imu_path (const robot_app_shared_t * p_copy,
                                uint32_t now_ms, uint16_t * p_path_hc)
{
    uint32_t stamp_ms = p_copy->path_ms;

    *p_path_hc = p_copy->path_hc;
    if ((now_ms - stamp_ms) >= ROBOT_APP_SERIAL_HALF)
    {
        stamp_ms = p_copy->previous_path_ms;
        *p_path_hc = p_copy->previous_path_hc;
    }
    /* A stamp still newer than now_ms wraps to at least the serial half. */
    return (0.0 < g_half_count_m) && ((now_ms - stamp_ms) <= 20U);
}

/** @brief Pack the terrain record outside the lock, then publish it with
 * the IMU stamps in one critical section. The mission maximum is an
 * integer maximum of floor-mm values, equal to the floor of the old fmax.
 */
static void robot_app_imu_publish (const terrain_output_t * p_output,
                                   const robot_app_shared_t * p_copy,
                                   bool b_valid, uint32_t now_ms)
{
    terrain_shared_t shared;
    uint32_t mission_mm = terrain_height_mm(p_output->run_maximum_m);
    uint32_t saved = 0U;

    if (p_copy->terrain.mission_maximum_mm > mission_mm)
    {
        mission_mm = p_copy->terrain.mission_maximum_mm;
    }
    terrain_shared_pack(p_output, mission_mm,
                        p_copy->terrain.b_mission_valid ||
                        (0U != p_output->completed_humps), &shared);
    saved = app_main_lock();
    g_shared.terrain = shared;
    g_shared.imu_ms = now_ms;
    if (b_valid && p_output->b_tilt_valid)
    {
        g_shared.tilt_ms = now_ms;
    }
    g_shared.health[ROBOT_APP_HEALTH_IMU] = now_ms;
    app_main_unlock(saved);
}

/** @brief Software STOP always wins over an older reset authorization.
 * The clean flag is read live and the stopping bits are cleared in the
 * same critical section, so a concurrent stop cannot be erased.
 */
static void robot_app_reset (const robot_app_shared_t * p_copy)
{
    motion_status_t status = MOTION_REJECTED;
    uint32_t saved = 0U;
    bool b_approved = false;

    status = motion_reset(&g_motion, p_copy->motion.b_stationary,
                           (!p_copy->faults.b_hard) &&
                           (!board_stop_pressed()));
    saved = app_main_lock();
    b_approved = (MOTION_OK == status) && g_shared.faults.b_reset_clean &&
        (!g_shared.faults.b_hard) && (!board_stop_pressed());
    g_shared.vehicle.b_reset_motion = false;
    if (b_approved)
    {
        g_shared.faults.b_inhibit = false;
        g_shared.faults.b_stop_latched = false;
        g_shared.faults.first = ROBOT_APP_FAULT_NONE;
        g_shared.faults.stop_source = 0U;
        /* Stopping bits clear; their owners keep NO_ECHO/WIFI_LOSS current. */
        g_shared.faults.mask = g_shared.faults.mask & ROBOT_APP_STATUS_BITS;
        g_shared.first_fault_ms = 0U;
    }
    app_main_unlock(saved);
    if (!b_approved)
    {
        motion_stop(&g_motion, MOTION_STOPPED);
    }
}

/** @brief Separate wheels cannot hide each other's stalls; rest needs dwell. */
static void robot_app_motion_health (motion_output_t * p_output,
    int32_t left_delta, int32_t right_delta, uint32_t now_ms)
{
    if ((0 != left_delta) ||
        (fabs(p_output->left_duty) < ROBOT_STALL_DUTY))
    {
        g_left_stall_ms = now_ms;
    }
    if ((0 != right_delta) ||
        (fabs(p_output->right_duty) < ROBOT_STALL_DUTY))
    {
        g_right_stall_ms = now_ms;
    }
    if (((now_ms - g_left_stall_ms) > ROBOT_STALL_MS) ||
        ((now_ms - g_right_stall_ms) > ROBOT_STALL_MS))
    {
        robot_app_fault_set(true,
            ((now_ms - g_left_stall_ms) > ROBOT_STALL_MS) ?
            ROBOT_APP_BIT_LEFT_STALL : ROBOT_APP_BIT_RIGHT_STALL);
    }
    if ((0 != left_delta) || (0 != right_delta) ||
        (fabs(p_output->left_duty) > 0.01) ||
        (fabs(p_output->right_duty) > 0.01))
    {
        g_stationary_since_ms = now_ms;
    }
    p_output->b_stationary =
        (now_ms - g_stationary_since_ms) >= 200U;
}

/** @brief Prepare soft-float PWM and pack the snapshot outside masking,
 * then commit with the latest inhibit. A valid step's centre path is
 * (left + right) half-counts; an invalid step (signed distance stored as
 * exactly 0.0 by motion_step) adds nothing, as before.
 */
static void robot_app_commit (const motion_output_t * p_output,
                              int32_t left_delta, int32_t right_delta,
                              uint32_t now_ms)
{
    board_motor_command_t command = {0};
    motion_snapshot_t snapshot;
    uint32_t saved = 0U;
    int32_t path_step = (0.0 != p_output->signed_distance_m) ?
                        (left_delta + right_delta) : 0;
    bool b_prepared = board_motor_prepare(p_output->left_duty,
                                          p_output->right_duty, &command);

    motion_snapshot_pack(p_output, &snapshot);
    /* Unconfirmed calibration is a normal inhibited configuration (for
     * example the wiring-only commissioning stage): a rejected command is
     * braked below without a hard fault. With the gate open, a preparation
     * failure is a genuine output fault.
     */
    if ((!b_prepared) && g_motion.config.b_calibration_confirmed)
    {
        robot_app_fault_set(true, ROBOT_APP_BIT_MOTOR_OUTPUT);
    }
    saved = app_main_lock();
    g_shared.motion = snapshot;
    g_shared.previous_path_hc = g_shared.path_hc;
    g_shared.previous_path_ms = g_shared.path_ms;
    /* Modulo-2^16 addition keeps a wrapping path that the IMU differences. */
    g_shared.path_hc = (uint16_t)(g_shared.path_hc + (uint32_t)path_step);
    g_shared.path_ms = now_ms;
    if (g_shared.faults.b_inhibit || (!b_prepared))
    {
        board_motor_brake();
    }
    else
    {
        board_motor_commit(&command);
    }
    g_shared.health[ROBOT_APP_HEALTH_MOTION] = now_ms;
    app_main_unlock(saved);
}

/** @brief A measured sign can reverse a count without signed-overflow UB. */
static int32_t robot_app_signed_count (int32_t count, int32_t sign)
{
    if (((1 != sign) && (-1 != sign)) || (INT32_MIN == count))
    {
        robot_app_fault_set(true, ROBOT_APP_BIT_ENCODER);
        return 0;
    }
    return (1 == sign) ? count : -count;
}

/** @brief Convert a 16-bit modulo position difference with an explicit
 * half-range. 32,767 counts (18.6 m) between 2 ms reads is unreachable, so
 * the ambiguous half-range value is an encoder fault.
 */
static int32_t robot_app_count_delta (uint16_t newer, uint16_t older)
{
    uint32_t difference = (uint32_t)(uint16_t)(newer - older);

    if (ROBOT_APP_COUNT_HALF == difference)
    {
        robot_app_fault_set(true, ROBOT_APP_BIT_ENCODER);
        return 0;
    }
    return robot_app_path_delta(newer, older);
}

/** @brief Signed modulo-2^16 difference of two wrapping counts. */
static int32_t robot_app_path_delta (uint16_t newer, uint16_t older)
{
    /* The masked difference is below 2^16 and fits int32_t exactly. */
    int32_t difference = (int32_t)(uint16_t)(newer - older);

    return (difference >= (int32_t)ROBOT_APP_COUNT_HALF) ?
           (difference - ROBOT_APP_COUNT_RANGE) : difference;
}

/** @brief Two-millisecond optical path from independent encoder totals:
 * the signed centre path in half-counts (left + right counts), wrapping
 * modulo 2^32, so barcode widths are whole half-counts.
 */
static bool robot_app_optical_path (void)
{
    uint16_t left_total = 0U;
    uint16_t right_total = 0U;
    int32_t left_delta = 0;
    int32_t right_delta = 0;

    board_encoder_totals(&left_total, &right_total);
    left_delta = robot_app_signed_count(
        robot_app_count_delta(left_total, g_line_left_total),
        ROBOT_LEFT_ENCODER_SIGN);
    right_delta = robot_app_signed_count(
        robot_app_count_delta(right_total, g_line_right_total),
        ROBOT_RIGHT_ENCODER_SIGN);
    g_line_left_total = left_total;
    g_line_right_total = right_total;
    /* Conversion to uint32_t is modulo 2^32: a wrapping coordinate. */
    g_optical_hc += (uint32_t)(left_delta + right_delta);
    return gb_optical_valid;
}

/** @brief Reject a codebook whose narrowest run cannot be sampled spatially.
 * The gate runs on the authored metre values, as before.
 */
static bool robot_app_codebook_ready (void)
{
    const barcode_config_t * p_codebook = course_cfg_codebook();
    double quantum_m = 0.0;
    double worst_step_m = 0.0;
    double counts_per_revolution = ROBOT_ENCODER_COUNTS_PER_REV;

    if ((NULL == p_codebook) || (counts_per_revolution <= 0.0))
    {
        return false;
    }
    quantum_m = (2.0 * CORE_PI * ROBOT_WHEEL_RADIUS_M) /
                counts_per_revolution;
    /* Qualify the largest accepted gap, not the nominal 2 ms task period.
     * The line-speed bound and encoder calibration still need bench proof.
     */
    worst_step_m = quantum_m + (ROBOT_LINE_SPEED_MPS *
        ((double)p_codebook->maximum_gap_ms / 1000.0));

    return (p_codebook->b_codebook_confirmed) &&
        (0U != p_codebook->symbol_count) &&
        (barcode_config_valid(p_codebook)) &&
        (p_codebook->maximum_step_m >= worst_step_m) &&
        (p_codebook->minimum_run_m >=
         (5.0 * p_codebook->maximum_step_m));
}

/** @brief One-Hz perception summary complements five-Hz motion status. */
static void robot_app_perception (const robot_app_shared_t * p_copy,
                                  uint32_t now_ms)
{
    telemetry_perception_t status;
    uint8_t frame[TELEMETRY_FRAME_MAX] = {0};
    size_t length = 0U;

    memset(&status, 0, sizeof(status));
    status.seq = g_telemetry_sequence;
    status.now_ms = now_ms;
    /* Eight-bit run counter; the calibration version saturates at 255. */
    status.run_id = (uint8_t)p_copy->vehicle.run_id;
    status.cal_version = (uint8_t)bitpack_saturate(ROBOT_CALIBRATION_VERSION,
                                                   ROBOT_APP_BYTE_BITS);
    /* Enumerators and 4-bit fields fit uint8_t exactly. */
    status.barcode_action = (uint8_t)p_copy->line.barcode_action;
    status.barcode_symbol = (uint8_t)p_copy->line.barcode_symbol;
    status.yaw_mrad_s = robot_app_wire_signed(p_copy->terrain.yaw_mrad_s,
                                              ROBOT_APP_RATE_BITS);
    status.motion_class = (uint8_t)p_copy->terrain.motion;
    /* Twelve-bit distances fit uint16_t; six-bit counts fit uint8_t. */
    status.scan_width_mm = (uint16_t)p_copy->scan.width_mm;
    status.scan_nearest_mm = (uint16_t)p_copy->scan.nearest_mm;
    status.scan_side = (uint8_t)p_copy->scan.preferred_side;
    status.scan_valid = (uint8_t)p_copy->scan.valid_samples;
    status.scan_unknown = (uint8_t)p_copy->scan.unknown_samples;
    status.b_tilt_valid = p_copy->terrain.b_tilt_valid;
    status.b_hump_active = p_copy->terrain.b_hump_active;
    status.b_scan_valid = p_copy->scan.b_profile_valid;
    status.b_obstacle_seen = p_copy->scan.b_obstacle_seen;
    length = telemetry_encode_perception(&status, frame, sizeof(frame));
    if ((0U == length) || (!net_mqtt_send(frame, length)))
    {
        robot_app_count_drop();
    }
}

/** @brief One-Hz first cause, active mask and stop sources. */
static void robot_app_diagnostic_frame (const robot_app_shared_t * p_copy,
                                       uint32_t now_ms)
{
    telemetry_diagnostic_t status;
    uint8_t frame[TELEMETRY_FRAME_MAX] = {0};
    size_t length = 0U;

    memset(&status, 0, sizeof(status));
    status.seq = g_telemetry_sequence;
    status.now_ms = now_ms;
    /* Eight-, four- and two-bit fields fit their uint8_t/uint16_t wires. */
    status.run_id = (uint8_t)p_copy->vehicle.run_id;
    status.first_fault_ms = p_copy->first_fault_ms;
    status.barcode_unknown = (uint8_t)p_copy->line.barcode_unknown;
    status.barcode_rejected = (uint8_t)p_copy->line.barcode_rejected;
    status.mission_peak_mm = (uint16_t)p_copy->terrain.mission_maximum_mm;
    status.b_mission_valid = p_copy->terrain.b_mission_valid;
    status.fault_code = (uint8_t)p_copy->faults.first;
    status.fault_mask = (uint16_t)p_copy->faults.mask;
    status.stop_source = (uint8_t)p_copy->faults.stop_source;
    length = telemetry_encode_diagnostic(&status, frame, sizeof(frame));
    if ((0U == length) || (!net_mqtt_send(frame, length)))
    {
        robot_app_count_drop();
    }
}

/** @brief IMU owner resets run history while retaining its measured mount.
 * path_hc is the encoder path robot_app_imu_path selected for the release
 * now_ms. Both 200 ms speed-window bases refer to that same instant.
 */
static void robot_app_terrain_run (const robot_app_shared_t * p_copy,
                                   uint16_t path_hc, uint32_t now_ms)
{
    terrain_config_t config = g_terrain.config;
    double pitch_reference = g_terrain.pitch_reference_rad;
    double roll_reference = g_terrain.roll_reference_rad;
    bool b_reference_valid = g_terrain.b_reference_valid;

    if (p_copy->vehicle.run_id != g_imu_run_id)
    {
        if (!terrain_init(&g_terrain, &config))
        {
            robot_app_fault_set(true, ROBOT_APP_BIT_IMU);
        }
        g_terrain.pitch_reference_rad = pitch_reference;
        g_terrain.roll_reference_rad = roll_reference;
        g_terrain.b_reference_valid = b_reference_valid;
        /* The run counter field holds eight bits. */
        g_imu_run_id = (uint8_t)p_copy->vehicle.run_id;
        g_imu_path_hc = path_hc;
        g_imu_window_path_hc = path_hc;
        g_imu_speed_mps = 0.0;
        g_imu_window_ms = now_ms;
    }
}

/* End of file. */


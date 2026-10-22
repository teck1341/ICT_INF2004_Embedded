/** @file apphost_test_main.c
 * @brief Deterministic public-step integration with explicit hardware doubles.
 * Physical confirmation overrides exist only in the temporary fixture build.
 * Synthetic motor/sonar responses establish software contracts, not accuracy.
 * BARR-ABI-TEST: board/app_main/net_mqtt/course_cfg names match real headers.
 */
#include "apphost_test_main.h"

#include "app_main.h"
#include "app_cfg.h"
#include "barcode.h"
#include "board.h"
#include "course_cfg.h"
#include "mqttcmd.h"
#include "net_mqtt.h"
#include "robot_app.h"
#include "robot_config.h"
#include "telemetry.h"
#include "vehicle.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef APPHOST_FIXTURE
#define APPHOST_FIXTURE (0)
#endif

#define APPHOST_BOOT_MS (400U)
#define APPHOST_BUTTON_MS (50U)
#define APPHOST_STALL_LIMIT_MS (8000U)
#define APPHOST_SCAN_LIMIT_MS (20000U)
#define APPHOST_ROUTE_LIMIT_MS (60000U)
#define APPHOST_CLOCK_RUN_MS (20000U)
#define APPHOST_MHZ_PER_HZ (1000U)
/* board.h: each ISR delta holds +/-31 counts (6 bits); more is an error. */
#define APPHOST_DELTA_MAX (31)

static uint64_t g_now_us;
static uint64_t g_accel_consumed;
static uint32_t g_accel_odr_mhz;
static uint32_t g_accel_phase_us;
static uint32_t g_accel_jitter_us;
static uint32_t g_accel_lcg = 1U;
static uint32_t g_accel_overruns;
static uint32_t g_motion_phase;
static uint32_t g_line_phase;
static uint32_t g_imu_phase;
static uint32_t g_override_motion_phase;
static uint32_t g_override_imu_phase;
static uint32_t g_preemptions;
static uint32_t g_preempt_ms = 1U;
static uint32_t g_preempt_motion_steps;
static uint64_t g_trigger_us;
static uint64_t g_echo_due_us;
static uint64_t g_last_centre_us;
static uint64_t g_centre_result_us;
static uint32_t g_echo_pulse;
static uint32_t g_lock_mask;
static uint32_t g_left_total;
static uint32_t g_right_total;
static uint32_t g_zero_commits;
static uint32_t g_prepare_locked;
static uint32_t g_command_reads;
static uint32_t g_watchdog_feeds;
static uint32_t g_checks;
static uint32_t g_failures;
static uint32_t g_status_frames;
static uint32_t g_perception_frames;
static uint32_t g_diagnostic_frames;
static uint32_t g_same_time_frames;
static uint32_t g_max_same_time_frames;
static uint32_t g_seq_errors;
static uint32_t g_seq_wraps;
static uint32_t g_pair_errors;
static uint8_t g_last_seq;
static uint64_t g_last_send_us;
static int32_t g_left_delta;
static int32_t g_right_delta;
static uint16_t g_servo_us = 1500U;
static uint16_t g_servo_min_us = 1500U;
static uint16_t g_servo_max_us = 1500U;
static uint16_t g_servo_first_us;
static uint16_t g_raw_ir[3] = {2000U, 2000U, 3500U};
static int16_t g_raw_accel[3] = {0, 0, 1000};
static double g_left_duty;
static double g_right_duty;
static double g_left_speed;
static double g_right_speed;
static double g_left_fraction;
static double g_right_fraction;
static double g_robot_yaw;
static double g_robot_x;
static double g_robot_y;
static double g_obstacle_x;
static double g_obstacle_y;
static double g_steady_mps;
static bool g_b_phase_override;
static bool g_b_start;
static bool g_b_stop;
static bool g_b_echo_pending;
static bool g_b_echo_valid;
static bool g_b_triggered;
static bool g_b_no_echo;
static bool g_b_sonar_silent;
static bool g_b_large_barcode_gap;
static barcode_config_t g_apphost_codebook;
static bool g_b_scan_obstacle;
static bool g_b_block_left;
static bool g_b_block_right;
static bool g_b_servo_left_centre;
static bool g_b_centre_after_scan;
static bool g_b_no_return_echo;
static bool g_b_no_side_echo;
static bool g_b_inject_stop_unlock;
/* GP20 press between Safety's poll and Vehicle's read in one tick. */
static bool g_b_press_after_safety;
/* STOP injected at the n-th outermost unlock of a Vehicle step (0 = off). */
static bool g_b_vehicle_step;
static uint32_t g_vehicle_unlocks;
static uint32_t g_inject_vehicle_unlock;
static bool g_b_skip_line;
static bool g_b_skip_telemetry;
static bool g_b_skip_comm;
static bool g_b_accel_pending;
static bool g_b_accel_invalid;
static bool g_b_preempt_accel;
static bool g_b_preempt_accel_always;
static bool g_b_preempt_button;
static bool g_b_prepare_reject;
static bool g_b_command_pending;
static bool g_b_online = true;
static uint8_t g_pending_command;
static uint8_t g_rejected_commands;
static telemetry_status_t g_last_status;
static telemetry_perception_t g_last_perception;
static telemetry_diagnostic_t g_last_diagnostic;

/** Diagnostic-frame fields read after a forced telemetry burst. */
typedef enum
{
    APPHOST_RUN = 0,
    APPHOST_FC,
    APPHOST_BU,
    APPHOST_BR,
    APPHOST_MP
} apphost_key_t;

/* BARR-TEST-01: a single-evaluation macro retains source diagnostics. */
#define CHECK(b_condition) \
    (apphost_check((b_condition), #b_condition, __LINE__))

static void apphost_check (bool b_condition, const char * p_expression,
                           int line);
static void apphost_run (uint32_t duration_ms);
static void apphost_tick (void);
static void apphost_plant (void);
static void apphost_encoder (double * p_fraction, double speed,
                             uint32_t * p_total, int32_t * p_delta);
static void apphost_boot (void);
static void apphost_start (void);
static void apphost_button (void);
static void apphost_send (const char * p_command);
static int32_t apphost_state (void);
static bool apphost_stopped (void);
static void apphost_default (void);
static void apphost_local_control (void);
static void apphost_epoch (void);
static void apphost_junction (void);
static void apphost_stall (bool b_left);
static void apphost_unknown_range (void);
static void apphost_transient_range (bool b_obstacle);
static void apphost_range_stop (void);
static void apphost_sonar_silent (bool b_stale_pose);
static void apphost_range_start (void);
static void apphost_barcode_gap (void);
static void apphost_line_timeout (void);
static void apphost_scan (bool b_unknown_return);
static void apphost_servo_endpoints (void);
static void apphost_full_bypass (void);
static void apphost_place_obstacle (void);
static void apphost_command_policy (void);
static void apphost_pin_policy (void);
static void apphost_tilt_fault (bool b_roll);
static void apphost_invalid_tilt_start (void);
static void apphost_transient_tilt (void);
static void apphost_accel_pending (void);
static void apphost_accel_unavailable (bool b_invalid);
static void apphost_barcode_noise (void);
static void apphost_barcode_junction (void);
static void apphost_hump_history (void);
static void apphost_watchdog_owners (void);
static void apphost_advance (double distance_m);
static int apphost_diagnostic_value (apphost_key_t key);
static void apphost_fault_mask (void);
static void apphost_fault_stress (void);
static void apphost_encoder_wrap (void);
static void apphost_encoder_jumps (void);
static void apphost_boot_counts (void);
static void apphost_stop_race (void);
static void apphost_stop_snapshot (void);
static bool apphost_select_review (const char * p_name);
static uint32_t apphost_mask (void);
static void apphost_fault_diagnostic (void);
static void apphost_telemetry_budget (void);
static void apphost_telemetry_wrap (void);
static void apphost_sequence (uint8_t seq);
static void apphost_partner (uint8_t seq);
static uint32_t apphost_take_delta (int32_t * p_delta, int32_t * p_count,
                                    uint32_t error_bit);
static void apphost_preempt (void);
static board_accel_status_t apphost_accel_clocked (int16_t milligravity[3]);
static void apphost_accel_clock (uint32_t odr_mhz, uint32_t jitter_us);
static void apphost_preempt_hump (bool b_always);
static double apphost_steady_speed (void);
static void apphost_preempt_steady (uint32_t preempt_ms);
static void apphost_barcode_crossbar (void);
static bool apphost_phases (const char * p_motion, const char * p_imu);
static void apphost_preempt_vehicle (void);
static void apphost_collision (bool b_preempt);
static void apphost_wiring_only (void);
static void apphost_motor_output (void);
static void apphost_barcode_range_pause (bool b_trailing);
static bool apphost_select_guards (const char * p_name);
static bool apphost_select_timing (const char * p_name);
static bool apphost_select (const char * p_name);

/** @brief Run one scenario per process so production static startup is real.
 * Optional digits after the name override the Motion release phase (0..4)
 * and the IMU release phase (0..9) for a phase sweep (T-53).
 */
int main (int argument_count, char * p_arguments[])
{
    if (((2 != argument_count) && (4 != argument_count)) ||
        ((4 == argument_count) &&
         (!apphost_phases(p_arguments[2], p_arguments[3]))) ||
        (!apphost_select(p_arguments[1])))
    {
        if (0 > fprintf(stderr, "Expected one known scenario name, "
                        "optionally with Motion (0-4) and IMU (0-9) "
                        "phases.\n"))
        {
            return EXIT_FAILURE;
        }
        return EXIT_FAILURE;
    }
    if (g_b_phase_override &&
        (0 > printf("%s: motion_phase=%u imu_phase=%u\n", p_arguments[1],
                    g_override_motion_phase, g_override_imu_phase)))
    {
        return EXIT_FAILURE;
    }
    CHECK(0U == g_prepare_locked);
    CHECK(0U == g_lock_mask);
    if (0 > printf("%s: checks=%u failures=%u fixture=%d\n",
                    p_arguments[1], g_checks, g_failures, APPHOST_FIXTURE))
    {
        return EXIT_FAILURE;
    }
    return (0U == g_failures) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/** @brief Fixture time advances only under the test scheduler. */
uint64_t board_time_us (void)
{
    return g_now_us;
}

/** @brief Hosted fixture records policy calls but does not emulate reset. */
bool board_watchdog_start (uint32_t timeout_ms)
{
    return (timeout_ms >= 100U) && (timeout_ms <= 8000U);
}

/** @brief Only production SafetyTask is expected to reach this boundary. */
void board_watchdog_feed (void)
{
    g_watchdog_feeds += 1U;
}

/** @brief A fresh process has no physical watchdog reset history. */
bool board_watchdog_rebooted (void)
{
    return false;
}

/** @brief The production configuration gates remain active in this double. */
bool board_init (void)
{
    board_motor_brake();
    return 0 != ROBOT_WIRING_CONFIRMED;
}

/** @brief Startup register setup has no meaning in the hosted fixture. */
void board_boot_timebase (void)
{
    g_now_us = 0U;
}

/** @brief Return current raw signed counts to the single motion owner. */
void board_read_encoders (int32_t * p_left, int32_t * p_right,
                          uint32_t * p_errors)
{
    if ((NULL != p_left) && (NULL != p_right) && (NULL != p_errors))
    {
        *p_errors = apphost_take_delta(&g_left_delta, p_left,
                                       BOARD_ENCODER_ERROR_LEFT) |
                    apphost_take_delta(&g_right_delta, p_right,
                                       BOARD_ENCODER_ERROR_RIGHT);
    }
}

/** @brief Read and clear one fixture delta as the board's 6-bit field: a
 * delta beyond +/-31 counts reads saturated with that wheel's error bit.
 */
static uint32_t apphost_take_delta (int32_t * p_delta, int32_t * p_count,
                                    uint32_t error_bit)
{
    uint32_t error = 0U;

    *p_count = *p_delta;
    if (*p_delta > APPHOST_DELTA_MAX)
    {
        *p_count = APPHOST_DELTA_MAX;
        error = error_bit;
    }
    else if (*p_delta < -APPHOST_DELTA_MAX)
    {
        *p_count = -APPHOST_DELTA_MAX;
        error = error_bit;
    }
    *p_delta = 0;
    return error;
}

/** @brief Boot counts are dropped before the first Motion read. */
void board_encoders_discard (void)
{
    g_left_delta = 0;
    g_right_delta = 0;
}

/** @brief A separate non-consuming position snapshot supports optical edges.
 * The board publishes 16-bit wrapping totals; the fixture keeps 32 bits.
 */
void board_encoder_totals (uint16_t * p_left, uint16_t * p_right)
{
    if ((NULL != p_left) && (NULL != p_right))
    {
        /* Modulo-2^16 narrowing is the board's documented total. */
        *p_left = (uint16_t)g_left_total;
        *p_right = (uint16_t)g_right_total;
    }
}

/** @brief Fixture optical values are independent of simulated wheel motion. */
bool board_read_ir (uint16_t samples[3])
{
    if (NULL == samples)
    {
        return false;
    }
    memcpy(samples, g_raw_ir, sizeof(g_raw_ir));
    return true;
}

/** @brief Synthetic gravity can exercise tilt validity without sensor I/O.
 * The blocking I2C read (about 1.2 ms at 100 kHz) is a legal preemption
 * point, and an optional sensor-clock model supplies STATUS_REG_A timing.
 * A preemption lasts g_preempt_ms ticks (1 unless a scenario lengthens it).
 */
board_accel_status_t board_read_accel (int16_t milligravity[3])
{
    uint32_t tick = 0U;

    if (g_b_preempt_accel || g_b_preempt_accel_always)
    {
        g_b_preempt_accel = false;
        for (tick = 0U; tick < g_preempt_ms; ++tick)
        {
            apphost_preempt();
        }
    }
    if ((NULL == milligravity) || g_b_accel_invalid)
    {
        return BOARD_ACCEL_INVALID;
    }
    if (0U != g_accel_odr_mhz)
    {
        return apphost_accel_clocked(milligravity);
    }
    if (g_b_accel_pending)
    {
        return BOARD_ACCEL_PENDING;
    }
    memcpy(milligravity, g_raw_accel, sizeof(g_raw_accel));
    return BOARD_ACCEL_FRESH;
}

/** @brief Timed valid ranges form a known frontal object with open sides.
 * The beam angle inverts the configured servo endpoints and sign, so the
 * asymmetric servo profiles exercise the real beam geometry.
 */
bool board_ultrasonic_trigger (void)
{
    double span_us = (g_servo_us < ROBOT_SERVO_CENTRE_US) ?
        (double)(ROBOT_SERVO_CENTRE_US - ROBOT_SERVO_MIN_US) :
        (double)(ROBOT_SERVO_MAX_US - ROBOT_SERVO_CENTRE_US);
    double angle_deg = ((double)g_servo_us - (double)ROBOT_SERVO_CENTRE_US)
                       * 60.0 * (double)ROBOT_SERVO_ANGLE_SIGN / span_us;
    double distance = 3.0;
    double bearing = atan2(g_obstacle_y - g_robot_y,
                           g_obstacle_x - g_robot_x) - g_robot_yaw;
    double beam = angle_deg * CORE_PI / 180.0;
    double difference = atan2(sin(bearing - beam), cos(bearing - beam));
    bool b_centre = (1500U == g_servo_us);

    if (g_b_echo_pending ||
        (g_b_triggered &&
         ((g_now_us - g_trigger_us) < ROBOT_TRIGGER_INTERVAL_US)))
    {
        return false;
    }
    if (g_b_scan_obstacle && (fabs(difference) < (CORE_PI / 18.0)))
    {
        distance = hypot(g_obstacle_x - g_robot_x,
                         g_obstacle_y - g_robot_y);
    }
    g_b_echo_valid = (!g_b_no_echo) &&
        (!(g_b_no_return_echo && g_b_centre_after_scan && b_centre)) &&
        (!(g_b_no_side_echo && (fabs(angle_deg) > 20.0)));
    /* 0.2..3.0m gives1166..17492us, within the valid sensor range. */
    g_echo_pulse = (uint32_t)((2.0 * distance * 1000000.0) / 343.0);
    g_trigger_us = g_now_us;
    g_echo_due_us = g_now_us + g_echo_pulse + 100U;
    if (!g_b_echo_valid)
    {
        g_echo_due_us = g_now_us + ROBOT_ECHO_TIMEOUT_US;
    }
    g_b_triggered = true;
    g_b_echo_pending = true;
    return true;
}

/** @brief Deadline completion is consumed by board_ultrasonic_result. */
void board_ultrasonic_service (void)
{
    /* No asynchronous ISR exists in this deterministic fixture. */
}

/** @brief Consume one completed echo/timeout without making no echo clear. */
bool board_ultrasonic_result (uint32_t * p_pulse_us, bool * p_valid)
{
    if (g_b_sonar_silent || (!g_b_echo_pending) ||
        (g_now_us < g_echo_due_us) ||
        (NULL == p_pulse_us) || (NULL == p_valid))
    {
        return false;
    }
    *p_pulse_us = g_b_echo_valid ? g_echo_pulse : 0U;
    *p_valid = g_b_echo_valid;
    if (1500U == g_servo_us)
    {
        g_centre_result_us = g_now_us;
    }
    g_b_echo_pending = false;
    return true;
}

/** @brief Record real servo commands and the final return-to-centre time. */
bool board_servo_us (uint16_t pulse_us)
{
    if ((0 == ROBOT_SERVO_CONFIRMED) ||
        (pulse_us < ROBOT_SERVO_MIN_US) ||
        (pulse_us > ROBOT_SERVO_MAX_US))
    {
        return false;
    }
    if (1500U != pulse_us)
    {
        g_b_servo_left_centre = true;
        if (0U == g_servo_first_us)
        {
            g_servo_first_us = pulse_us;
        }
    }
    if ((1500U == pulse_us) && (g_servo_us > 1500U) &&
        g_b_servo_left_centre)
    {
        g_b_centre_after_scan = true;
        g_last_centre_us = g_now_us;
    }
    g_servo_us = pulse_us;
    if (pulse_us < g_servo_min_us)
    {
        g_servo_min_us = pulse_us;
    }
    if (pulse_us > g_servo_max_us)
    {
        g_servo_max_us = pulse_us;
    }
    return true;
}

/** @brief Assert expensive preparation occurs outside the IRQ mask. */
bool board_motor_prepare (core_real_t left_duty, core_real_t right_duty,
                          board_motor_command_t * p_command)
{
    if (0U != g_lock_mask)
    {
        g_prepare_locked += 1U;
    }
    if (NULL == p_command)
    {
        return false;
    }
    memset(p_command, 0, sizeof(*p_command));
    /* g_b_prepare_reject models a PWM preparation failure with open gates. */
    if ((0 == ROBOT_CALIBRATION_CONFIRMED) ||
        (0 == ROBOT_WIRING_CONFIRMED) || (!isfinite(left_duty)) ||
        (!isfinite(right_duty)) || (fabs(left_duty) > 1.0) ||
        (fabs(right_duty) > 1.0) || g_b_prepare_reject)
    {
        return false;
    }
    /* Clamped magnitudes are at most 6250, inside the 13-bit level field. */
    p_command->left_level = ((uint32_t)(fmin(fabs(left_duty), ROBOT_MAX_DUTY)
                                        * (ROBOT_PWM_WRAP + 1U))) & 0x1FFFU;
    p_command->right_level = ((uint32_t)(fmin(fabs(right_duty),
                                              ROBOT_MAX_DUTY)
                                         * (ROBOT_PWM_WRAP + 1U))) & 0x1FFFU;
    p_command->left_direction =
        BOARD_DIRECTION_ENCODE((left_duty < 0.0) ? -1 : 1);
    p_command->right_direction =
        BOARD_DIRECTION_ENCODE((right_duty < 0.0) ? -1 : 1);
    p_command->b_valid = true;
    return true;
}

/** @brief Capture applied motor demand after the application's final guard. */
void board_motor_commit (const board_motor_command_t * p_command)
{
    if ((NULL == p_command) || (!p_command->b_valid) || g_b_stop)
    {
        board_motor_brake();
        return;
    }
    g_left_duty = ((double)p_command->left_level /
                   (ROBOT_PWM_WRAP + 1U)) *
                  board_direction_decode(p_command->left_direction);
    g_right_duty = ((double)p_command->right_level /
                    (ROBOT_PWM_WRAP + 1U)) *
                   board_direction_decode(p_command->right_direction);
    if ((0U == p_command->left_level) && (0U == p_command->right_level))
    {
        g_zero_commits += 1U;
    }
}

/** @brief Legacy convenience API still obeys the same boundary contracts. */
void board_motor_duty (core_real_t left_duty, core_real_t right_duty)
{
    board_motor_command_t command;
    bool b_prepared = board_motor_prepare(left_duty, right_duty, &command);

    if (b_prepared)
    {
        board_motor_commit(&command);
    }
    else
    {
        board_motor_brake();
    }
}

/** @brief Record zero demand synchronously when a stop is latched. */
void board_motor_brake (void)
{
    g_left_duty = 0.0;
    g_right_duty = 0.0;
    g_zero_commits += 1U;
}

/** @brief Physical STOP level can be varied independently of MQTT. */
bool board_stop_pressed (void)
{
    return g_b_stop;
}

/** @brief Physical START level exercises production debounce. VehicleTask
 * reads it after capturing now_ms and before its snapshot: a legal
 * preemption point for higher-priority owners.
 */
bool board_start_pressed (void)
{
    if (g_b_preempt_button)
    {
        g_b_preempt_button = false;
        apphost_preempt();
    }
    return g_b_start;
}

/** @brief No task loop should reach the fake delay in a public-step test. */
void app_main_delay (uint32_t delay_ms)
{
    /* Preserve a detectable error instead of entering a real wait. */
    g_failures += 1U;
    g_now_us += (uint64_t)delay_ms * 1000U;
}

/** @brief Model PRIMASK save/restore, including nested critical regions. */
uint32_t app_main_lock (void)
{
    uint32_t previous = g_lock_mask;

    g_lock_mask = 1U;
    return previous;
}

/** @brief Inject a STOP at an explicit legal preemption boundary when armed. */
void app_main_unlock (uint32_t saved_mask)
{
    g_lock_mask = saved_mask;
    if ((0U == g_lock_mask) && g_b_inject_stop_unlock)
    {
        g_b_inject_stop_unlock = false;
        apphost_send("STOP");
        /* Millisecond fixture time is kept well below uint32_t wrap. */
        robot_app_comm_step((uint32_t)(g_now_us / 1000U));
    }
    if ((0U == g_lock_mask) && g_b_vehicle_step &&
        (0U != g_inject_vehicle_unlock))
    {
        ++g_vehicle_unlocks;
        if (g_vehicle_unlocks == g_inject_vehicle_unlock)
        {
            g_inject_vehicle_unlock = 0U;
            apphost_send("STOP");
            robot_app_comm_step((uint32_t)(g_now_us / 1000U));
        }
    }
}

/** @brief IRQ installation is a board boundary, excluded from host claims. */
bool app_main_install_gpio_irq (void (* p_handler)(uint32_t))
{
    return NULL != p_handler;
}

/** @brief Network queue creation succeeds independently of association. */
bool net_mqtt_start (void)
{
    return true;
}

/** @brief Decode each actual binary frame with the production decoder;
 * every frame the application sends must decode exactly.
 */
bool net_mqtt_send (const uint8_t * p_frame, size_t length)
{
    telemetry_frame_t type = TELEMETRY_FRAME_STATUS;

    if ((NULL == p_frame) || (0U == length) ||
        (length > TELEMETRY_FRAME_MAX))
    {
        return false;
    }
    if (g_now_us != g_last_send_us)
    {
        g_same_time_frames = 0U;
        g_last_send_us = g_now_us;
    }
    ++g_same_time_frames;
    if (g_same_time_frames > g_max_same_time_frames)
    {
        g_max_same_time_frames = g_same_time_frames;
    }
    CHECK(telemetry_frame_type(p_frame, length, &type));
    switch (type)
    {
        case TELEMETRY_FRAME_STATUS:
            CHECK(telemetry_decode_status(p_frame, length, &g_last_status));
            apphost_sequence(g_last_status.seq);
            ++g_status_frames;
        break;
        case TELEMETRY_FRAME_PERCEPTION:
            CHECK(telemetry_decode_perception(p_frame, length,
                                              &g_last_perception));
            apphost_partner(g_last_perception.seq);
            ++g_perception_frames;
        break;
        case TELEMETRY_FRAME_DIAGNOSTIC:
            CHECK(telemetry_decode_diagnostic(p_frame, length,
                                              &g_last_diagnostic));
            apphost_partner(g_last_diagnostic.seq);
            ++g_diagnostic_frames;
        break;
        default:
            CHECK(false);
        break;
    }
    return true;
}

/** @brief Deliver one command byte through the production API. */
bool net_mqtt_receive (uint8_t * p_command)
{
    if ((NULL == p_command) || (!g_b_command_pending))
    {
        return false;
    }
    *p_command = g_pending_command;
    g_b_command_pending = false;
    g_command_reads += 1U;
    return true;
}

/** @brief Session state of the simulated radio owner. */
bool net_mqtt_online (void)
{
    return g_b_online;
}

/** @brief Wrapping count of payloads the simulated radio rejected. */
uint8_t net_mqtt_rejected (void)
{
    return g_rejected_commands;
}

/** @brief No lwIP stack is part of the application-step host fixture. */
void net_mqtt_poll (struct netif * p_interface)
{
    /* Opaque pointer is unused; there is no network simulation. */
    (void)p_interface;
}

/** @brief One labelled synthetic pattern enables fixture INIT only. The
 * table is refilled on every call so g_b_large_barcode_gap takes effect;
 * the runtime keeps this pointer, as it keeps the flash table on target.
 */
const barcode_config_t * course_cfg_codebook (void)
{
    barcode_defaults(&g_apphost_codebook);
    g_apphost_codebook.b_calibration_confirmed =
        0 != ROBOT_CALIBRATION_CONFIRMED;
    g_apphost_codebook.b_codebook_confirmed = 0 != ROBOT_BARCODE_CONFIRMED;
    if (0 != APPHOST_FIXTURE)
    {
        g_apphost_codebook.symbol_count = 1U;
        g_apphost_codebook.symbols[0].label = 'T';
        g_apphost_codebook.symbols[0].action = BARCODE_ACTION_STRAIGHT;
        g_apphost_codebook.symbols[0].run_count = 1U;
        g_apphost_codebook.symbols[0].width_m[0] = 0.012;
        /* Fixture half-count is 0.284 mm; widths are not course values. */
        g_apphost_codebook.minimum_run_m = 0.010;
        g_apphost_codebook.maximum_step_m = 0.002;
        g_apphost_codebook.maximum_gap_ms =
            g_b_large_barcode_gap ? 50U : 2U;
    }
    return &g_apphost_codebook;
}

/** @brief Keep checks enabled in optimized release builds. */
static void apphost_check (bool b_condition, const char * p_expression,
                           int line)
{
    g_checks += 1U;
    if (!b_condition)
    {
        g_failures += 1U;
        /* Diagnostic failure cannot erase an already counted test failure. */
        /* Fixture time is below 2^32 microseconds; narrowing is exact. */
        (void)fprintf(stderr, "FAIL line %d time %u: %s; st=%u stop=%d "
            "fault=%d rv=%d rg=%u\n", line, (uint32_t)g_now_us,
            p_expression, g_last_status.state, g_last_status.b_stop,
            g_last_status.b_hardfault, g_last_status.b_rangevalid,
            g_last_status.range_mm);
        /* Diagnostic failure cannot change the recorded test verdict. */
        (void)fprintf(stderr, "Diagnostic: fc=%u mask=0x%04x src=%u\n",
            g_last_diagnostic.fault_code, g_last_diagnostic.fault_mask,
            g_last_diagnostic.stop_source);
    }
}

/** @brief Advance one millisecond with production task periods/priorities. */
static void apphost_tick (void)
{
    uint32_t now_ms = 0U;

    g_now_us += 1000U;
    apphost_plant();
    /* Scenario millisecond times fit uint32_t; conversion is exact. */
    now_ms = (uint32_t)(g_now_us / 1000U);
    if (0U == (now_ms % 2U))
    {
        robot_app_safety_step(now_ms);
    }
    /* Release phases default to 0; timing scenarios offset Motion/Line,
     * and a phase sweep may offset Motion and the IMU. A press armed for
     * the gap after Safety's poll lands here.
     */
    if (g_b_press_after_safety)
    {
        g_b_press_after_safety = false;
        g_b_stop = true;
    }
    if (g_motion_phase == (now_ms % 5U))
    {
        robot_app_motion_step(now_ms);
    }
    if ((g_line_phase == (now_ms % 2U)) && (!g_b_skip_line))
    {
        robot_app_line_step(now_ms);
    }
    if (0U == (now_ms % 10U))
    {
        g_b_vehicle_step = true;
        g_vehicle_unlocks = 0U;
        robot_app_vehicle_step(now_ms);
        g_b_vehicle_step = false;
    }
    if (g_imu_phase == (now_ms % 10U))
    {
        robot_app_imu_step(now_ms);
    }
    if (0U == (now_ms % 2U))
    {
        robot_app_scan_step(now_ms);
    }
    if ((0U == (now_ms % 5U)) && (!g_b_skip_comm))
    {
        robot_app_comm_step(now_ms);
    }
    if ((0U == (now_ms % 200U)) && (!g_b_skip_telemetry))
    {
        robot_app_telemetry_step(now_ms);
    }
}

/** @brief Run a finite scheduler window with no wall-clock sleeping. */
static void apphost_run (uint32_t duration_ms)
{
    uint32_t elapsed = 0U;

    for (elapsed = 0U; elapsed < duration_ms; elapsed += 1U)
    {
        apphost_tick();
    }
}

/** @brief A small deterministic lag produces signed encoder feedback. */
static void apphost_plant (void)
{
    double left_target = g_b_block_left ? 0.0 : (0.8 * g_left_duty);
    double right_target = g_b_block_right ? 0.0 : (0.8 * g_right_duty);

    g_left_speed += 0.08 * (left_target - g_left_speed);
    g_right_speed += 0.08 * (right_target - g_right_speed);
    if (g_steady_mps > 0.0)
    {
        /* A steady plant: both wheels pinned, independent of demand. */
        g_left_speed = g_steady_mps;
        g_right_speed = g_steady_mps;
    }
    apphost_encoder(&g_left_fraction, g_left_speed,
                     &g_left_total, &g_left_delta);
    apphost_encoder(&g_right_fraction, g_right_speed,
                     &g_right_total, &g_right_delta);
    g_robot_x += 0.0005 * (g_left_speed + g_right_speed) * cos(g_robot_yaw);
    g_robot_y += 0.0005 * (g_left_speed + g_right_speed) * sin(g_robot_yaw);
    g_robot_yaw += (g_right_speed - g_left_speed) * 0.001 /
                   ROBOT_TRACK_WIDTH_M;
}

/** @brief Convert fixture wheel displacement to modulo raw quadrature counts.
 */
static void apphost_encoder (double * p_fraction, double speed,
                             uint32_t * p_total, int32_t * p_delta)
{
    double counts = speed * 0.001 * ROBOT_ENCODER_COUNTS_PER_REV /
                    (2.0 * CORE_PI * ROBOT_WHEEL_RADIUS_M);

    *p_fraction += counts;
    while (*p_fraction >= 1.0)
    {
        *p_total += 1U;
        *p_delta += 1;
        *p_fraction -= 1.0;
    }
    while (*p_fraction <= -1.0)
    {
        *p_total -= 1U;
        *p_delta -= 1;
        *p_fraction += 1.0;
    }
}

/** @brief Exercise initialization and fresh sensing before any START.
 * A phase-sweep override replaces the scenario's own release phases.
 */
static void apphost_boot (void)
{
    bool b_board = false;
    bool b_app = false;

    if (g_b_phase_override)
    {
        g_motion_phase = g_override_motion_phase;
        g_imu_phase = g_override_imu_phase;
    }
    b_board = board_init();
    b_app = robot_app_init();
    if (0 != APPHOST_FIXTURE)
    {
        CHECK(b_board);
        CHECK(b_app);
    }
    else
    {
        CHECK(!b_board);
    }
    apphost_run(APPHOST_BOOT_MS);
}

/** @brief Press and release the actual debounced local-control input. */
static void apphost_button (void)
{
    g_b_start = true;
    apphost_run(APPHOST_BUTTON_MS);
    g_b_start = false;
    apphost_run(APPHOST_BUTTON_MS);
}

/** @brief A fixture starts through public input and observes real telemetry. */
static void apphost_start (void)
{
    apphost_boot();
    CHECK(VEHICLE_IDLE == apphost_state());
    apphost_button();
    CHECK(VEHICLE_FOLLOW == apphost_state());
    apphost_run(200U);
    CHECK(!apphost_stopped());
}

/** @brief Queue the binary command byte for a named command. */
static void apphost_send (const char * p_command)
{
    static const char * const names[] =
    {
        "STOP", "START", "RESET", "LEFT", "RIGHT", "STRAIGHT", "UTURN"
    };
    uint32_t index = 0U;
    bool b_found = false;

    for (index = 0U; index < (sizeof(names) / sizeof(names[0])); ++index)
    {
        if (0 == strcmp(p_command, names[index]))
        {
            /* index + 1 is the declared mqttcmd_code_t value 1..7. */
            g_pending_command = mqttcmd_byte((mqttcmd_code_t)(index + 1U));
            g_b_command_pending = true;
            b_found = true;
        }
    }
    CHECK(b_found);
}

/** @brief Inspect the state field of the actual decoded status frame. */
static int32_t apphost_state (void)
{
    uint32_t now_ms = 0U;

    /* Test horizons below2^32ms make the narrowing exact. */
    now_ms = (uint32_t)(g_now_us / 1000U);
    robot_app_telemetry_step(now_ms);
    robot_app_telemetry_step(now_ms);
    return (int32_t)g_last_status.state;
}

/** @brief Zero commanded demand; mechanical stop is deliberately not claimed.
 */
static bool apphost_stopped (void)
{
    return (fabs(g_left_duty) < 1.0e-12) &&
           (fabs(g_right_duty) < 1.0e-12);
}

/** @brief Unmodified confirmation gates never enable a local start. */
static void apphost_default (void)
{
    robot_app_diagnostic_t diagnostic = {0};

    CHECK(0 == APPHOST_FIXTURE);
    apphost_boot();
    apphost_button();
    /* Real usermain would not launch tasks after unconfirmed board_init.
     * Even directly exercising steps here must remain inhibited and braked.
     * A PWM preparation rejected by an unconfirmed calibration gate is the
     * normal inhibited configuration, not a latched output fault.
     */
    CHECK(VEHICLE_INIT == apphost_state());
    CHECK(apphost_stopped());
    CHECK(0U != g_zero_commits);
    robot_app_diagnostic(&diagnostic);
    CHECK(ROBOT_APP_FAULT_NONE == diagnostic.code);
}

/** @brief STOP/reset/start require distinct local presses and measured rest. */
static void apphost_local_control (void)
{
    apphost_start();
    apphost_send("STOP");
    apphost_run(40U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
    apphost_run(250U);
    apphost_button();
    CHECK(VEHICLE_IDLE == apphost_state());
    CHECK(apphost_stopped());
    apphost_run(100U);
    CHECK(VEHICLE_IDLE == apphost_state());
    apphost_button();
    CHECK(VEHICLE_FOLLOW == apphost_state());
}

/** @brief A new STOP between snapshot and reset cannot be acknowledged away. */
static void apphost_epoch (void)
{
    uint32_t now_ms = 0U;

    apphost_start();
    apphost_send("STOP");
    apphost_run(300U);
    CHECK(VEHICLE_FAULT == apphost_state());
    g_b_start = true;
    /* Stop just after vehicle publishes reset, before next motion
     * activation.
     */
    apphost_run(40U);
    CHECK(VEHICLE_IDLE == apphost_state());
    g_b_inject_stop_unlock = true;
    g_now_us += 5000U;
    /* Bounded fixture millisecond time is exactly representable. */
    now_ms = (uint32_t)(g_now_us / 1000U);
    robot_app_motion_step(now_ms);
    CHECK(!g_b_inject_stop_unlock);
    CHECK(apphost_stopped());
    g_b_start = false;
    apphost_run(20U);
    CHECK(VEHICLE_FAULT == apphost_state());
}

/** @brief Both-black debounce must HALT then resume without a sticky fault. */
static void apphost_junction (void)
{
    uint32_t previous_zero = 0U;

    apphost_start();
    previous_zero = g_zero_commits;
    g_raw_ir[0] = ROBOT_IR_LEFT_BLACK;
    g_raw_ir[1] = ROBOT_IR_RIGHT_BLACK;
    apphost_run(15U);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(g_zero_commits > previous_zero);
    apphost_run(30U);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    g_raw_ir[0] = 2000U;
    g_raw_ir[1] = 2000U;
    apphost_run(30U);
    CHECK(VEHICLE_FOLLOW == apphost_state());
}

/** @brief Either individual wheel stall must be detected despite other
 * pulses.
 */
static void apphost_stall (bool b_left)
{
    apphost_start();
    g_b_block_left = b_left;
    g_b_block_right = !b_left;
    apphost_run(APPHOST_STALL_LIMIT_MS);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
    CHECK(g_last_status.b_hardfault);
}

/** @brief A lost ultrasonic echo never becomes a new clear-space observation.
 */
static void apphost_unknown_range (void)
{
    const uint32_t supervisor = UINT32_C(1) << ROBOT_APP_BIT_SUPERVISOR;
    const uint32_t echo = UINT32_C(1) << ROBOT_APP_BIT_NO_ECHO;
    robot_app_diagnostic_t diagnostic;

    apphost_start();
    g_b_no_echo = true;
    apphost_run(200U);
    CHECK(VEHICLE_RANGE_WAIT == apphost_state());
    CHECK(apphost_stopped());
    CHECK(!g_last_status.b_rangevalid);
    CHECK(echo == apphost_mask());
    apphost_run(1000U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
    /* The supervisor's own timeout is its fault bit; NO_ECHO says why. */
    memset(&diagnostic, 0, sizeof(diagnostic));
    robot_app_diagnostic(&diagnostic);
    CHECK((ROBOT_APP_FAULT_SUPERVISOR == diagnostic.code) &&
          ((supervisor | echo) == diagnostic.mask));
    g_b_no_echo = false;
    apphost_run(300U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
    CHECK(supervisor == apphost_mask());
    apphost_button();
    CHECK(VEHICLE_IDLE == apphost_state());
    CHECK(apphost_stopped());
    CHECK(0U == apphost_mask());
    apphost_button();
    CHECK(VEHICLE_FOLLOW == apphost_state());
}

/** @brief A timeout pauses, rests, then reacquires clear range or scans. */
static void apphost_transient_range (bool b_obstacle)
{
    apphost_start();
    g_b_no_echo = true;
    apphost_run(200U);
    CHECK(VEHICLE_RANGE_WAIT == apphost_state());
    CHECK(apphost_stopped());
    g_b_no_echo = false;
    if (b_obstacle)
    {
        apphost_place_obstacle();
    }
    apphost_run(10U);
    CHECK(VEHICLE_RANGE_WAIT == apphost_state());
    CHECK(apphost_stopped());
    apphost_run(400U);
    CHECK((b_obstacle ? VEHICLE_SCAN : VEHICLE_FOLLOW) == apphost_state());
    CHECK(b_obstacle == apphost_stopped());
    CHECK(1 == apphost_diagnostic_value(APPHOST_RUN));
}

/** @brief STOP during a range pause remains latched after echo recovery. */
static void apphost_range_stop (void)
{
    apphost_start();
    g_b_no_echo = true;
    apphost_run(200U);
    CHECK(VEHICLE_RANGE_WAIT == apphost_state());
    apphost_send("STOP");
    g_b_no_echo = false;
    apphost_run(400U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
    CHECK(ROBOT_APP_FAULT_REMOTE_STOP ==
          apphost_diagnostic_value(APPHOST_FC));
}

/** @brief Absent results and ranges from the wrong pose cannot grant motion. */
static void apphost_sonar_silent (bool b_stale_pose)
{
    apphost_start();
    g_b_sonar_silent = true;
    if (b_stale_pose)
    {
        /* Synthetic rotation exceeds the 0.12 rad beam-pose tolerance. */
        g_left_total -= 30U;
        g_right_total += 30U;
        g_left_delta -= 30;
        g_right_delta += 30;
        apphost_run(20U);
        CHECK(VEHICLE_RANGE_WAIT == apphost_state());
        CHECK(g_last_status.b_rangevalid);
    }
    apphost_run(250U);
    CHECK(VEHICLE_RANGE_WAIT == apphost_state());
    CHECK(apphost_stopped());
    if (b_stale_pose)
    {
        g_b_sonar_silent = false;
        apphost_run(400U);
        CHECK(VEHICLE_FOLLOW == apphost_state());
    }
    else
    {
        apphost_run(1000U);
        CHECK(VEHICLE_FAULT == apphost_state());
        CHECK(apphost_stopped());
    }
}

/** @brief Unknown startup range cannot acquire IDLE or honor local START. */
static void apphost_range_start (void)
{
    g_b_no_echo = true;
    apphost_boot();
    CHECK(VEHICLE_INIT == apphost_state());
    apphost_button();
    CHECK(VEHICLE_INIT == apphost_state());
    CHECK(apphost_stopped());
    g_b_no_echo = false;
    apphost_run(200U);
    CHECK(VEHICLE_IDLE == apphost_state());
    CHECK(apphost_stopped());
}

/** @brief A valid decoder config may exceed application sampling limits. */
static void apphost_barcode_gap (void)
{
    const barcode_config_t * p_config = NULL;

    g_b_large_barcode_gap = true;
    p_config = course_cfg_codebook();
    CHECK(barcode_config_valid(p_config));
    CHECK(50U == p_config->maximum_gap_ms);
    apphost_boot();
    apphost_button();
    CHECK(VEHICLE_INIT == apphost_state());
    CHECK(apphost_stopped());
}

/** @brief Fine-result handoff waits for settled valid frontal sensing.
 * An unknown result (no centre echo after the scan, or no echo from open
 * side sectors) must never authorize a bypass.
 */
static void apphost_scan (bool b_unknown_return)
{
    uint32_t elapsed = 0U;
    int32_t state = VEHICLE_FOLLOW;
    bool b_saw_scan = false;
    bool b_saw_bypass = false;

    apphost_start();
    apphost_place_obstacle();
    g_b_no_return_echo = b_unknown_return && (!g_b_no_side_echo);
    for (elapsed = 0U; elapsed < APPHOST_SCAN_LIMIT_MS; elapsed += 10U)
    {
        apphost_run(10U);
        state = apphost_state();
        b_saw_scan = b_saw_scan || (VEHICLE_SCAN == state);
        if (VEHICLE_BYPASS == state)
        {
            b_saw_bypass = true;
            break;
        }
        if (VEHICLE_FAULT == state)
        {
            break;
        }
    }
    CHECK(b_saw_scan);
    CHECK(g_b_centre_after_scan);
    if (b_unknown_return)
    {
        CHECK(!b_saw_bypass);
        CHECK(apphost_stopped());
    }
    else
    {
        CHECK(b_saw_bypass);
        CHECK(g_centre_result_us > g_last_centre_us);
        CHECK((g_centre_result_us - g_last_centre_us) >=
              ((uint64_t)ROBOT_SERVO_SETTLE_MS * 1000U));
    }
}

/** @brief Remote START/RESET cannot energise an idle robot; STOP is accepted.
 */
static void apphost_command_policy (void)
{
    apphost_boot();
    apphost_send("START");
    apphost_run(10U);
    CHECK(VEHICLE_IDLE == apphost_state());
    CHECK(apphost_stopped());
    apphost_send("RESET");
    apphost_run(10U);
    CHECK(VEHICLE_IDLE == apphost_state());
    CHECK(apphost_stopped());
    apphost_send("STOP");
    apphost_run(20U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
    CHECK(3U == g_command_reads);
}

/** @brief Only the HC-SR04 shares the GP0/GP1 console pins, as TRIG on TX
 * and ECHO on RX; it stays off the GP12-GP15 servo ports, the right IR D0
 * rewired from Grove 6 takes neither GP26 nor a servo pin, and no IR D0
 * shares a pin the firmware drives or reads.
 */
static void apphost_pin_policy (void)
{
    static const uint32_t used[] =
    {
        ROBOT_PIN_ENCODER_LEFT_A, ROBOT_PIN_ENCODER_LEFT_B,
        ROBOT_PIN_ENCODER_RIGHT_A, ROBOT_PIN_ENCODER_RIGHT_B,
        ROBOT_PIN_MOTOR_LEFT_A, ROBOT_PIN_MOTOR_LEFT_B,
        ROBOT_PIN_MOTOR_RIGHT_A, ROBOT_PIN_MOTOR_RIGHT_B, ROBOT_PIN_SERVO,
        ROBOT_PIN_IMU_SDA, ROBOT_PIN_IMU_SCL, ROBOT_PIN_TRIGGER,
        ROBOT_PIN_ECHO, ROBOT_PIN_STOP, ROBOT_PIN_START, ROBOT_PIN_IR_LEFT,
        ROBOT_PIN_IR_RIGHT, ROBOT_PIN_IR_BARCODE
    };
    uint32_t index = 0U;

    for (index = 0U; index < (sizeof(used) / sizeof(used[0])); ++index)
    {
        CHECK((ROBOT_PIN_IR_LEFT_AUX != used[index]) &&
              (ROBOT_PIN_IR_RIGHT_AUX != used[index]) &&
              (ROBOT_PIN_IR_BARCODE_AUX != used[index]));
    }
    CHECK(ROBOT_PIN_ENCODER_LEFT_A > 1U);
    CHECK(ROBOT_PIN_ENCODER_LEFT_B > 1U);
    CHECK(ROBOT_PIN_ENCODER_RIGHT_A > 1U);
    CHECK(ROBOT_PIN_ENCODER_RIGHT_B > 1U);
    CHECK(ROBOT_PIN_IMU_SDA > 1U);
    CHECK(ROBOT_PIN_IMU_SCL > 1U);
    CHECK(ROBOT_PIN_MOTOR_LEFT_A > 1U);
    CHECK(ROBOT_PIN_SERVO > 1U);
    CHECK(ROBOT_PIN_STOP > 1U);
    CHECK(ROBOT_PIN_START > 1U);
    CHECK(ROBOT_PIN_IR_LEFT > 1U);
    CHECK((0U == ROBOT_PIN_CONSOLE_TX) && (1U == ROBOT_PIN_CONSOLE_RX));
    CHECK(ROBOT_PIN_CONSOLE_TX == ROBOT_PIN_TRIGGER);
    CHECK(ROBOT_PIN_CONSOLE_RX == ROBOT_PIN_ECHO);
    CHECK((ROBOT_PIN_TRIGGER < 12U) || (ROBOT_PIN_TRIGGER > 15U));
    CHECK((ROBOT_PIN_ECHO < 12U) || (ROBOT_PIN_ECHO > 15U));
    /* The right IR D0 moved off GP26 (Grove 5's IR) to GP13. */
    CHECK(ROBOT_PIN_IR_RIGHT_AUX != ROBOT_PIN_IR_LEFT);
    CHECK((ROBOT_PIN_IR_RIGHT_AUX != ROBOT_PIN_SERVO) &&
          (ROBOT_PIN_IR_RIGHT_AUX != 14U) && (ROBOT_PIN_IR_RIGHT_AUX != 15U));
}

/** @brief An explicit scenario name selects a finite deterministic fixture. */
static bool apphost_select (const char * p_name)
{
    if (0 == strcmp(p_name, "default"))
    {
        apphost_default();
    }
    else if (0 == strcmp(p_name, "local_control"))
    {
        apphost_local_control();
    }
    else if (0 == strcmp(p_name, "stop_epoch"))
    {
        apphost_epoch();
    }
    else if (0 == strcmp(p_name, "junction_wait"))
    {
        apphost_junction();
    }
    else if (0 == strcmp(p_name, "left_stall"))
    {
        apphost_stall(true);
    }
    else if (0 == strcmp(p_name, "right_stall"))
    {
        apphost_stall(false);
    }
    else if (0 == strcmp(p_name, "line_timeout"))
    {
        apphost_line_timeout();
    }
    else if (0 == strcmp(p_name, "unknown_range"))
    {
        apphost_unknown_range();
    }
    else if (0 == strcmp(p_name, "transient_range"))
    {
        apphost_transient_range(false);
    }
    else if (0 == strcmp(p_name, "range_obstacle"))
    {
        apphost_transient_range(true);
    }
    else if (0 == strcmp(p_name, "range_stop"))
    {
        apphost_range_stop();
    }
    else if (0 == strcmp(p_name, "sonar_silent"))
    {
        apphost_sonar_silent(false);
    }
    else if (0 == strcmp(p_name, "stale_range_pose"))
    {
        apphost_sonar_silent(true);
    }
    else if (0 == strcmp(p_name, "range_start"))
    {
        apphost_range_start();
    }
    else if (0 == strcmp(p_name, "barcode_gap"))
    {
        apphost_barcode_gap();
    }
    else if (0 == strcmp(p_name, "scan_handoff"))
    {
        apphost_scan(false);
    }
    else if (0 == strcmp(p_name, "servo_endpoints"))
    {
        apphost_servo_endpoints();
    }
    else if (0 == strcmp(p_name, "scan_unknown_return"))
    {
        apphost_scan(true);
    }
    else if (0 == strcmp(p_name, "full_bypass"))
    {
        apphost_full_bypass();
    }
    else if (0 == strcmp(p_name, "command_policy"))
    {
        apphost_command_policy();
    }
    else if (0 == strcmp(p_name, "uart_pins"))
    {
        apphost_pin_policy();
    }
    else
    {
        return apphost_select_guards(p_name);
    }
    return true;
}

/** @brief Keep safety/diagnostic scenarios separate from course dispatch. */
static bool apphost_select_guards (const char * p_name)
{
    if (0 == strcmp(p_name, "pitch_fault"))
    {
        apphost_tilt_fault(false);
    }
    else if (0 == strcmp(p_name, "roll_fault"))
    {
        apphost_tilt_fault(true);
    }
    else if (0 == strcmp(p_name, "invalid_tilt_start"))
    {
        apphost_invalid_tilt_start();
    }
    else if (0 == strcmp(p_name, "transient_tilt"))
    {
        apphost_transient_tilt();
    }
    else if (0 == strcmp(p_name, "accel_pending"))
    {
        apphost_accel_pending();
    }
    else if (0 == strcmp(p_name, "accel_stale"))
    {
        apphost_accel_unavailable(false);
    }
    else if (0 == strcmp(p_name, "accel_invalid"))
    {
        apphost_accel_unavailable(true);
    }
    else if (0 == strcmp(p_name, "barcode_noise"))
    {
        apphost_barcode_noise();
    }
    else if (0 == strcmp(p_name, "barcode_junction"))
    {
        apphost_barcode_junction();
    }
    else if (0 == strcmp(p_name, "barcode_crossbar"))
    {
        apphost_barcode_crossbar();
    }
    else if (0 == strcmp(p_name, "hump_history"))
    {
        apphost_hump_history();
    }
    else if (0 == strcmp(p_name, "watchdog_owners"))
    {
        apphost_watchdog_owners();
    }
    else if (0 == strcmp(p_name, "fault_diagnostic"))
    {
        apphost_fault_diagnostic();
    }
    else if (0 == strcmp(p_name, "fault_mask"))
    {
        apphost_fault_mask();
    }
    else if (0 == strcmp(p_name, "fault_stress"))
    {
        apphost_fault_stress();
    }
    else if (0 == strcmp(p_name, "encoder_wrap"))
    {
        apphost_encoder_wrap();
    }
    else if (0 == strcmp(p_name, "telemetry_budget"))
    {
        apphost_telemetry_budget();
    }
    else if (0 == strcmp(p_name, "telemetry_wrap"))
    {
        apphost_telemetry_wrap();
    }
    else
    {
        return apphost_select_timing(p_name);
    }
    return true;
}

/** @brief Sensor-clock, preemption, commissioning and output scenarios. */
static bool apphost_select_timing (const char * p_name)
{
    if (0 == strcmp(p_name, "accel_clock_fast"))
    {
        /* +100 ppm sensor clock, no read jitter. */
        apphost_accel_clock(100010U, 0U);
    }
    else if (0 == strcmp(p_name, "accel_clock_slow"))
    {
        /* -100 ppm sensor clock with up to 0.8 ms of read jitter. */
        apphost_accel_clock(99990U, 800U);
    }
    else if (0 == strcmp(p_name, "preempt_hump"))
    {
        apphost_preempt_hump(false);
    }
    else if (0 == strcmp(p_name, "preempt_hump_always"))
    {
        apphost_preempt_hump(true);
    }
    else if (0 == strcmp(p_name, "preempt_steady"))
    {
        /* One Motion publish during the blocked IMU read. */
        apphost_preempt_steady(1U);
    }
    else if (0 == strcmp(p_name, "preempt_steady_double"))
    {
        /* Two Motion publishes (+1 and +6 ms) during one IMU read. */
        apphost_preempt_steady(6U);
    }
    else if (0 == strcmp(p_name, "preempt_vehicle"))
    {
        apphost_preempt_vehicle();
    }
    else if (0 == strcmp(p_name, "collision_guard"))
    {
        apphost_collision(false);
    }
    else if (0 == strcmp(p_name, "preempt_collision"))
    {
        apphost_collision(true);
    }
    else if (0 == strcmp(p_name, "barcode_range_pause"))
    {
        apphost_barcode_range_pause(false);
    }
    else if (0 == strcmp(p_name, "barcode_quiet_pause"))
    {
        apphost_barcode_range_pause(true);
    }
    else if (0 == strcmp(p_name, "scan_side_silent"))
    {
        /* Open side sectors beyond 20 degrees return no echo at all. */
        g_b_no_side_echo = true;
        apphost_scan(true);
    }
    else if (0 == strcmp(p_name, "wiring_only"))
    {
        apphost_wiring_only();
    }
    else if (0 == strcmp(p_name, "motor_output_fault"))
    {
        apphost_motor_output();
    }
    else
    {
        return apphost_select_review(p_name);
    }
    return true;
}

/** @brief Missing optical-task activations cause a bounded owner watchdog. */
static void apphost_line_timeout (void)
{
    uint32_t previous_feeds = 0U;

    apphost_start();
    previous_feeds = g_watchdog_feeds;
    g_b_skip_line = true;
    apphost_run(40U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
    CHECK(g_last_status.b_hardfault);
    CHECK(g_watchdog_feeds <= (previous_feeds + 10U));
}

/** @brief Put one synthetic frontal object in a fixed world location. */
static void apphost_place_obstacle (void)
{
    g_obstacle_x = g_robot_x + (0.20 * cos(g_robot_yaw));
    g_obstacle_y = g_robot_y + (0.20 * sin(g_robot_yaw));
    g_b_scan_obstacle = true;
}

/** @brief Complete the application's real scan/lease/bypass/recovery chain.
 * Sensors and wheel response remain deterministic test doubles throughout.
 */
static void apphost_full_bypass (void)
{
    uint32_t elapsed = 0U;
    int32_t state = VEHICLE_FOLLOW;
    bool b_saw_bypass = false;
    bool b_returned_follow = false;

    apphost_start();
    apphost_place_obstacle();
    for (elapsed = 0U; elapsed < APPHOST_ROUTE_LIMIT_MS; elapsed += 10U)
    {
        apphost_run(10U);
        state = apphost_state();
        b_saw_bypass = b_saw_bypass || (VEHICLE_BYPASS == state);
        if (b_saw_bypass && (VEHICLE_FOLLOW == state))
        {
            b_returned_follow = true;
            break;
        }
        if (VEHICLE_FAULT == state)
        {
            break;
        }
    }
    CHECK(b_saw_bypass);
    CHECK(b_returned_follow);
    CHECK(g_robot_x > g_obstacle_x);
    CHECK(fabs(g_robot_y) < ROBOT_LINE_UNIQUE_CORRIDOR_M);
}

/** @brief Asymmetric calibrated endpoint spans work with either beam sign. */
static void apphost_servo_endpoints (void)
{
    uint32_t elapsed = 0U;
    robot_app_diagnostic_t diagnostic = {0};

    apphost_start();
    apphost_place_obstacle();
    for (elapsed = 0U; elapsed < APPHOST_SCAN_LIMIT_MS; elapsed += 10U)
    {
        apphost_run(10U);
        robot_app_diagnostic(&diagnostic);
        if ((ROBOT_APP_FAULT_NONE != diagnostic.code) ||
            ((ROBOT_SERVO_MIN_US == g_servo_min_us) &&
             (ROBOT_SERVO_MAX_US == g_servo_max_us) &&
             (ROBOT_SERVO_CENTRE_US == g_servo_us)))
        {
            break;
        }
    }
    CHECK(ROBOT_APP_FAULT_NONE == diagnostic.code);
    CHECK(ROBOT_SERVO_MIN_US == g_servo_min_us);
    CHECK(ROBOT_SERVO_MAX_US == g_servo_max_us);
    CHECK(ROBOT_SERVO_CENTRE_US == g_servo_us);
    CHECK(g_servo_first_us == ((ROBOT_SERVO_ANGLE_SIGN > 0) ?
          ROBOT_SERVO_MIN_US : ROBOT_SERVO_MAX_US));
}

/** @brief Sustained 45-degree pitch or roll cannot leave motors energized. */
static void apphost_tilt_fault (bool b_roll)
{
    robot_app_diagnostic_t diagnostic = {0};

    apphost_start();
    g_raw_accel[b_roll ? 1U : 0U] = 707;
    g_raw_accel[2] = 707;
    apphost_run(300U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
    CHECK(g_last_status.b_hardfault);
    robot_app_diagnostic(&diagnostic);
    CHECK((ROBOT_APP_FAULT_TILT == diagnostic.code) ||
          ((!b_roll) &&
           (ROBOT_APP_FAULT_TILT_UNAVAILABLE == diagnostic.code)));
}

/** @brief Fresh I2C data without usable orientation cannot authorize START. */
static void apphost_invalid_tilt_start (void)
{
    apphost_boot();
    g_raw_accel[2] = -1000;
    apphost_run(200U);
    apphost_button();
    CHECK(VEHICLE_IDLE == apphost_state());
    CHECK(apphost_stopped());
}

/** @brief Invalid orientation brakes but permits deliberate local recovery. */
static void apphost_transient_tilt (void)
{
    robot_app_diagnostic_t diagnostic = {0};

    apphost_start();
    g_raw_accel[2] = 0;
    apphost_run(10U);
    CHECK(apphost_stopped());
    apphost_run(240U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(!g_last_status.b_hardfault);
    robot_app_diagnostic(&diagnostic);
    CHECK(ROBOT_APP_FAULT_TILT_UNAVAILABLE == diagnostic.code);
    apphost_button();
    CHECK(VEHICLE_FAULT == apphost_state());
    g_raw_accel[2] = 1000;
    apphost_run(250U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
    apphost_button();
    CHECK(VEHICLE_IDLE == apphost_state());
    CHECK(apphost_stopped());
    apphost_button();
    CHECK(VEHICLE_FOLLOW == apphost_state());
    apphost_run(50U);
    CHECK(!apphost_stopped());
}

/** @brief A sample phase miss must not be confused with corrupted I2C data. */
static void apphost_accel_pending (void)
{
    robot_app_diagnostic_t diagnostic = {0};

    apphost_start();
    g_b_accel_pending = true;
    apphost_run(10U);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(!apphost_stopped());
    robot_app_diagnostic(&diagnostic);
    CHECK(ROBOT_APP_FAULT_NONE == diagnostic.code);
    g_b_accel_pending = false;
    apphost_run(20U);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(!apphost_stopped());
}

/** @brief Persistent missing samples stop; corrupted samples remain hard. */
static void apphost_accel_unavailable (bool b_invalid)
{
    robot_app_diagnostic_t diagnostic = {0};

    apphost_start();
    g_b_accel_pending = !b_invalid;
    g_b_accel_invalid = b_invalid;
    apphost_run(b_invalid ? 10U : 60U);
    CHECK(apphost_stopped());
    apphost_run(20U);
    CHECK(VEHICLE_FAULT == apphost_state());
    robot_app_diagnostic(&diagnostic);
    CHECK((b_invalid ? ROBOT_APP_FAULT_IMU :
           ROBOT_APP_FAULT_TILT_UNAVAILABLE) == diagnostic.code);
    g_b_accel_pending = false;
    g_b_accel_invalid = false;
    apphost_run(250U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
    apphost_button();
    CHECK((b_invalid ? VEHICLE_FAULT : VEHICLE_IDLE) == apphost_state());
    CHECK(apphost_stopped());
    if (!b_invalid)
    {
        apphost_button();
        CHECK(VEHICLE_FOLLOW == apphost_state());
    }
}

/** @brief Preserve the first cause until an approved stationary reset. */
static void apphost_fault_diagnostic (void)
{
    robot_app_diagnostic_t first = {0};
    robot_app_diagnostic_t later = {0};

    apphost_start();
    apphost_send("STOP");
    apphost_run(10U);
    robot_app_diagnostic(&first);
    CHECK(ROBOT_APP_FAULT_REMOTE_STOP == first.code);
    CHECK(705U == first.first_fault_ms);
    g_b_stop = true;
    apphost_run(20U);
    robot_app_diagnostic(&later);
    CHECK(first.code == later.code);
    CHECK(first.first_fault_ms == later.first_fault_ms);
    g_b_stop = false;
    apphost_run(250U);
    apphost_button();
    CHECK(VEHICLE_IDLE == apphost_state());
    robot_app_diagnostic(&later);
    CHECK(ROBOT_APP_FAULT_NONE == later.code);
    CHECK(0U == later.first_fault_ms);
    g_b_stop = true;
    apphost_run(20U);
    robot_app_diagnostic(&later);
    CHECK(ROBOT_APP_FAULT_LOCAL_STOP == later.code);
    CHECK(later.first_fault_ms > first.first_fault_ms);
    CHECK(apphost_stopped());
}

/** @brief Scheduled status/perception/diagnostics fit the stated TX budget. */
static void apphost_telemetry_budget (void)
{
    apphost_boot();
    g_status_frames = 0U;
    g_perception_frames = 0U;
    g_diagnostic_frames = 0U;
    g_same_time_frames = 0U;
    g_max_same_time_frames = 0U;
    apphost_run(1000U);
    CHECK(5U == g_status_frames);
    CHECK(1U == g_perception_frames);
    CHECK(1U == g_diagnostic_frames);
    CHECK(3U == g_max_same_time_frames);
    CHECK((0U == g_last_diagnostic.fault_code) &&
          (0U == g_last_diagnostic.first_fault_ms));
}

/** @brief 300 status frames cross the 250 wrap once; seq never skips and
 * every 1 Hz frame repeats the seq of the status frame just sent, a
 * multiple of 5, so a missing partner frame stays detectable.
 */
static void apphost_telemetry_wrap (void)
{
    apphost_boot();
    g_status_frames = 0U;
    g_perception_frames = 0U;
    g_diagnostic_frames = 0U;
    g_seq_errors = 0U;
    g_seq_wraps = 0U;
    g_pair_errors = 0U;
    apphost_run(60000U);
    CHECK(300U == g_status_frames);
    CHECK(60U == g_perception_frames);
    CHECK(60U == g_diagnostic_frames);
    CHECK(1U == g_seq_wraps);
    CHECK(0U == g_seq_errors);
    CHECK(0U == g_pair_errors);
}

/** @brief Status seq advances by one modulo 250 from frame to frame. */
static void apphost_sequence (uint8_t seq)
{
    if (0U != g_status_frames)
    {
        if (seq != (uint8_t)((g_last_seq + 1U) % TELEMETRY_SEQUENCE_MODULO))
        {
            ++g_seq_errors;
        }
        if (seq < g_last_seq)
        {
            ++g_seq_wraps;
        }
    }
    g_last_seq = seq;
}

/** @brief A perception or diagnostic frame pairs with the last status. */
static void apphost_partner (uint8_t seq)
{
    if ((seq != g_last_seq) || (0U != (seq % 5U)))
    {
        ++g_pair_errors;
    }
}

/** @brief Physical path supplies barcode run widths in this fixture only. */
static void apphost_advance (double distance_m)
{
    double start_m = g_robot_x;
    uint32_t elapsed = 0U;

    while (((g_robot_x - start_m) < distance_m) && (elapsed < 6000U))
    {
        apphost_run(1U);
        ++elapsed;
    }
    CHECK(elapsed < 6000U);
}

/** @brief Force a full telemetry burst and read one decoded diagnostic
 * field.
 */
static int apphost_diagnostic_value (apphost_key_t key)
{
    uint32_t index = 0U;
    uint32_t frames = g_diagnostic_frames;
    int value = -1;

    for (index = 0U; index < 5U; ++index)
    {
        robot_app_telemetry_step((uint32_t)(g_now_us / 1000U));
    }
    CHECK(g_diagnostic_frames > frames);
    switch (key)
    {
        case APPHOST_RUN:
            value = (int)g_last_diagnostic.run_id;
        break;
        case APPHOST_FC:
            value = (int)g_last_diagnostic.fault_code;
        break;
        case APPHOST_BU:
            value = (int)g_last_diagnostic.barcode_unknown;
        break;
        case APPHOST_BR:
            value = (int)g_last_diagnostic.barcode_rejected;
        break;
        case APPHOST_MP:
            value = (int)g_last_diagnostic.mission_peak_mm;
        break;
        default:
            CHECK(false);
        break;
    }
    return value;
}

/** @brief Live fault mask through the public diagnostic snapshot. */
static uint32_t apphost_mask (void)
{
    robot_app_diagnostic_t diagnostic;

    memset(&diagnostic, 0, sizeof(diagnostic));
    robot_app_diagnostic(&diagnostic);
    return diagnostic.mask;
}

/** @brief Mask bits, stop sources and status bits through a mission:
 * remote STOP is a stop source, not a mask bit; WIFI_LOSS follows the
 * radio session and NO_ECHO the latest front reading; an approved reset
 * clears stopping state but keeps the status bits; telemetry carries them.
 */
static void apphost_fault_mask (void)
{
    robot_app_diagnostic_t diagnostic;
    const uint32_t wifi = UINT32_C(1) << ROBOT_APP_BIT_WIFI_LOSS;
    const uint32_t echo = UINT32_C(1) << ROBOT_APP_BIT_NO_ECHO;

    apphost_start();
    CHECK(0U == apphost_mask());
    g_b_online = false;
    apphost_run(10U);
    CHECK(wifi == apphost_mask());
    CHECK(VEHICLE_FOLLOW == apphost_state());
    g_b_online = true;
    apphost_run(10U);
    CHECK(0U == apphost_mask());
    apphost_send("STOP");
    apphost_run(10U);
    memset(&diagnostic, 0, sizeof(diagnostic));
    robot_app_diagnostic(&diagnostic);
    CHECK((ROBOT_APP_FAULT_REMOTE_STOP == diagnostic.code) &&
          (ROBOT_APP_STOP_REMOTE == diagnostic.stop_source) &&
          (0U == diagnostic.mask));
    g_b_no_echo = true;
    apphost_run(300U);
    CHECK(echo == (apphost_mask() & echo));
    (void)apphost_diagnostic_value(APPHOST_FC);
    CHECK((echo == (g_last_diagnostic.fault_mask & echo)) &&
          (ROBOT_APP_STOP_REMOTE == g_last_diagnostic.stop_source));
    g_b_no_echo = false;
    apphost_run(300U);
    CHECK(0U == (apphost_mask() & echo));
    g_b_online = false;
    apphost_run(10U);
    apphost_button();
    CHECK(VEHICLE_IDLE == apphost_state());
    memset(&diagnostic, 0, sizeof(diagnostic));
    robot_app_diagnostic(&diagnostic);
    CHECK((ROBOT_APP_FAULT_NONE == diagnostic.code) &&
          (0U == diagnostic.stop_source) && (wifi == diagnostic.mask));
}

/** @brief The 16-bit wrapping encoder totals cross 0xFFFF -> 0 during
 * line following: the optical path, barcode coordinate and IMU path use
 * modulo differences, so no ENCODER fault and no stop may follow.
 */
static void apphost_encoder_wrap (void)
{
    int rejected = 0;

    g_left_total = 0xFFF0U;
    g_right_total = 0xFFF0U;
    apphost_start();
    rejected = apphost_diagnostic_value(APPHOST_BR);
    apphost_run(1000U);
    CHECK((g_left_total > 0x10000U) && (g_right_total > 0x10000U));
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(!apphost_stopped());
    CHECK(0U == apphost_mask());
    CHECK(0 == apphost_diagnostic_value(APPHOST_FC));
    CHECK(rejected == apphost_diagnostic_value(APPHOST_BR));
}

/** @brief Line's 16-bit total differences: a 10-count reverse step back
 * across 0x0000 reads as -10 with no fault, while a jump of exactly 0x8000
 * (sign unknowable) latches a hard ENCODER fault as the first cause.
 */
static void apphost_encoder_jumps (void)
{
    const uint32_t encoder = UINT32_C(1) << ROBOT_APP_BIT_ENCODER;
    robot_app_diagnostic_t diagnostic;

    g_left_total = 5U;
    apphost_boot();
    g_left_total -= 10U;
    apphost_run(4U);
    CHECK(0U == apphost_mask());
    g_left_total += 0x8000U;
    apphost_run(4U);
    memset(&diagnostic, 0, sizeof(diagnostic));
    robot_app_diagnostic(&diagnostic);
    CHECK((ROBOT_APP_FAULT_ENCODER == diagnostic.code) &&
          (encoder == (diagnostic.mask & encoder)));
}

/** @brief Edges counted between board_init() and the end of
 * robot_app_init() (40 counts, beyond the 6-bit delta) are discarded, so
 * the first Motion read cannot latch a false hard ENCODER fault.
 */
static void apphost_boot_counts (void)
{
    g_left_delta = 40;
    g_right_delta = -40;
    apphost_start();
    CHECK(0U == apphost_mask());
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(!apphost_stopped());
}

/** @brief GP20 pressed after Safety's poll and before Vehicle's read in one
 * tick: Vehicle reaches FAULT first, yet the stop is recorded as LOCAL STOP
 * with its source, never as a SUPERVISOR fault.
 */
static void apphost_stop_race (void)
{
    robot_app_diagnostic_t diagnostic;

    apphost_start();
    while (9U != ((uint32_t)(g_now_us / 1000U) % 10U))
    {
        apphost_run(1U);
    }
    g_b_press_after_safety = true;
    apphost_run(10U);
    memset(&diagnostic, 0, sizeof(diagnostic));
    robot_app_diagnostic(&diagnostic);
    CHECK((ROBOT_APP_FAULT_LOCAL_STOP == diagnostic.code) &&
          (ROBOT_APP_STOP_LOCAL == diagnostic.stop_source) &&
          (0U == diagnostic.mask));
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
}

/** @brief Vehicle half of the reset handshake: a STOP raised just after the
 * snapshot of the Vehicle step that publishes the reset (its third unlock:
 * copy, button latch, snapshot) leaves that reset unclean, so Motion
 * refuses it and the car stays stopped.
 */
static void apphost_stop_snapshot (void)
{
    apphost_start();
    apphost_send("STOP");
    apphost_run(300U);
    CHECK(VEHICLE_FAULT == apphost_state());
    while (9U != ((uint32_t)(g_now_us / 1000U) % 10U))
    {
        apphost_run(1U);
    }
    /* The next Vehicle step records the press; the one 30 ms later
     * latches it and publishes the reset.
     */
    g_b_start = true;
    apphost_run(30U);
    g_inject_vehicle_unlock = 3U;
    apphost_run(1U);
    CHECK(0U == g_inject_vehicle_unlock);
    g_b_start = false;
    apphost_run(20U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
}

/** @brief Scenarios for the reset handshake, the boot window, the stop
 * race and the encoder total rules.
 */
static bool apphost_select_review (const char * p_name)
{
    if (0 == strcmp(p_name, "stop_race"))
    {
        apphost_stop_race();
    }
    else if (0 == strcmp(p_name, "stop_snapshot"))
    {
        apphost_stop_snapshot();
    }
    else if (0 == strcmp(p_name, "boot_counts"))
    {
        apphost_boot_counts();
    }
    else if (0 == strcmp(p_name, "encoder_jumps"))
    {
        apphost_encoder_jumps();
    }
    else
    {
        return false;
    }
    return true;
}

/** @brief Tribunal stress test: toggle the two status bits from their
 * owners (Comm every step, Scan at each ping) 10,000 times while a hard
 * TILT is latched (a braked car cannot also stall); TILT must never be
 * lost and WIFI_LOSS must follow the session flag in every round.
 */
static void apphost_fault_stress (void)
{
    const uint32_t tilt = UINT32_C(1) << ROBOT_APP_BIT_TILT;
    const uint32_t wifi = UINT32_C(1) << ROBOT_APP_BIT_WIFI_LOSS;
    uint32_t round = 0U;
    uint32_t mask = 0U;
    bool b_kept = true;
    bool b_tracked = true;

    apphost_start();
    g_raw_accel[0] = 707;
    g_raw_accel[2] = 707;
    apphost_run(300U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(tilt == (apphost_mask() & tilt));
    g_raw_accel[0] = 0;
    g_raw_accel[2] = 1000;
    for (round = 0U; round < 10000U; ++round)
    {
        g_b_online = 0U != (round & 1U);
        g_b_no_echo = 0U != (round & 2U);
        apphost_run(5U);
        mask = apphost_mask();
        b_kept = b_kept && (tilt == (mask & tilt));
        b_tracked = b_tracked &&
                    ((0U != (mask & wifi)) == (!g_b_online));
    }
    CHECK(b_kept);
    CHECK(b_tracked);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
}

/** @brief Unmatched/too-short frames count; an impossible ADC still faults. */
static void apphost_barcode_noise (void)
{
    apphost_start();
    apphost_advance(0.03);
    g_raw_ir[2] = ROBOT_IR_BARCODE_BLACK;
    apphost_advance(0.018);
    g_raw_ir[2] = ROBOT_IR_BARCODE_WHITE;
    apphost_advance(0.03);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(1 == apphost_diagnostic_value(APPHOST_BU));
    g_raw_ir[2] = ROBOT_IR_BARCODE_BLACK;
    apphost_advance(0.005);
    g_raw_ir[2] = ROBOT_IR_BARCODE_WHITE;
    apphost_advance(0.03);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(1 == apphost_diagnostic_value(APPHOST_BR));
    g_raw_ir[0] = UINT16_MAX;
    apphost_run(20U);
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
    CHECK(g_last_status.b_hardfault);
}

/** @brief Barcode-end does not execute action; a later latched edge does. */
static void apphost_barcode_junction (void)
{
    apphost_start();
    apphost_advance(0.03);
    g_raw_ir[2] = ROBOT_IR_BARCODE_BLACK;
    apphost_advance(0.012);
    g_raw_ir[2] = ROBOT_IR_BARCODE_WHITE;
    apphost_advance(0.15);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    g_raw_ir[0] = ROBOT_IR_LEFT_BLACK;
    g_raw_ir[1] = ROBOT_IR_RIGHT_BLACK;
    apphost_run(45U);
    CHECK(VEHICLE_NAVIGATE == apphost_state());
}

/** @brief Completed hump survives approved stop/reset/start in diagnostics. */
static void apphost_hump_history (void)
{
    int previous_peak = 0;

    apphost_start();
    apphost_run(500U);
    g_raw_accel[0] = 199;
    g_raw_accel[2] = 980;
    apphost_run(1600U);
    g_raw_accel[0] = -199;
    apphost_run(1800U);
    g_raw_accel[0] = 0;
    g_raw_accel[2] = 1000;
    apphost_run(600U);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    previous_peak = apphost_diagnostic_value(APPHOST_MP);
    CHECK(previous_peak > 0);
    CHECK(g_last_diagnostic.b_mission_valid);
    apphost_send("STOP");
    apphost_run(350U);
    apphost_button();
    CHECK(VEHICLE_IDLE == apphost_state());
    apphost_button();
    apphost_run(100U);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(previous_peak == apphost_diagnostic_value(APPHOST_MP));
    CHECK(0U == g_last_status.peak_mm);
    CHECK(g_last_diagnostic.b_mission_valid);
}

/** @brief Control owners and the application Comm owner govern watchdog
 * feeding; Telemetry does not. The radio owner that receives MQTT STOP
 * (cyw43_utk_task, P9) is not gated: Comm liveness does not prove reception.
 */
static void apphost_watchdog_owners (void)
{
    uint32_t feeds = 0U;

    apphost_start();
    feeds = g_watchdog_feeds;
    g_b_skip_telemetry = true;
    apphost_run(1500U);
    CHECK(g_watchdog_feeds > (feeds + 700U));
    CHECK(VEHICLE_FOLLOW == apphost_state());
    g_b_skip_comm = true;
    apphost_run(150U);
    feeds = g_watchdog_feeds;
    apphost_run(100U);
    CHECK(feeds == g_watchdog_feeds);
}

/** @brief Model the next 1 ms tick while a lower-priority step is blocked in
 * a board call: only Safety, Motion and Line (priorities 1..3) may run.
 */
static void apphost_preempt (void)
{
    uint32_t now_ms = 0U;

    g_now_us += 1000U;
    apphost_plant();
    /* Scenario millisecond times fit uint32_t; conversion is exact. */
    now_ms = (uint32_t)(g_now_us / 1000U);
    g_preemptions += 1U;
    if (0U == (now_ms % 2U))
    {
        robot_app_safety_step(now_ms);
    }
    if (g_motion_phase == (now_ms % 5U))
    {
        g_preempt_motion_steps += 1U;
        robot_app_motion_step(now_ms);
    }
    if ((g_line_phase == (now_ms % 2U)) && (!g_b_skip_line))
    {
        robot_app_line_step(now_ms);
    }
}

/** @brief LSM303DLHC STATUS_REG_A on the sensor's own clock: samples finish
 * every 1/ODR after the phase; a read sets ZYXDA for one unread sample and
 * ZYXOR as well for two or more. The production classifier decides, and
 * any flagged status drains the BDU latch, as in board.c.
 */
static board_accel_status_t apphost_accel_clocked (int16_t milligravity[3])
{
    uint64_t read_us = g_now_us;
    uint64_t produced = 0U;
    uint8_t status = 0U;
    board_accel_status_t result = BOARD_ACCEL_PENDING;

    g_accel_lcg = (g_accel_lcg * 1103515245U) + 12345U;
    read_us += (g_accel_lcg >> 8U) % (g_accel_jitter_us + 1U);
    if (read_us > g_accel_phase_us)
    {
        produced = ((read_us - g_accel_phase_us) * g_accel_odr_mhz) /
                   (UINT64_C(1000000) * APPHOST_MHZ_PER_HZ);
    }
    if (produced > g_accel_consumed)
    {
        status = (uint8_t)BOARD_ACCEL_ZYXDA;
        if ((produced - g_accel_consumed) > 1U)
        {
            status = (uint8_t)(BOARD_ACCEL_ZYXDA | BOARD_ACCEL_ZYXOR);
            g_accel_overruns += 1U;
        }
    }
    result = board_accel_classify(status);
    if (BOARD_ACCEL_PENDING != result)
    {
        g_accel_consumed = produced;
        memcpy(milligravity, g_raw_accel, sizeof(g_raw_accel));
    }
    return result;
}

/** @brief A sensor clock offset from the 10 ms poll must not end a run.
 * The first sample completes 10 us after an IMU read instant, so the
 * offset moves a completion across a read (overrun) early in the run.
 */
static void apphost_accel_clock (uint32_t odr_mhz, uint32_t jitter_us)
{
    robot_app_diagnostic_t diagnostic = {0};

    apphost_start();
    /* Test horizons below 2^32 us make the narrowing exact. */
    g_accel_phase_us = (uint32_t)g_now_us + 10U;
    g_accel_consumed = 0U;
    g_accel_jitter_us = jitter_us;
    g_accel_odr_mhz = odr_mhz;
    apphost_run(APPHOST_CLOCK_RUN_MS);
    robot_app_diagnostic(&diagnostic);
    if (0 > printf("accel clock %u mHz, jitter %u us: %u overrun reads, "
                   "fc=%d\n", odr_mhz, jitter_us, g_accel_overruns,
                   (int)diagnostic.code))
    {
        g_failures += 1U;
    }
    CHECK(g_accel_overruns > 0U);
    CHECK(ROBOT_APP_FAULT_NONE == diagnostic.code);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(!g_last_status.b_hardfault);
    CHECK(!apphost_stopped());
}

/** @brief MotionTask (released 1 ms after the IMU) preempts one IMU I2C read
 * mid-climb, or every IMU read across the hump. The IMU step differences
 * the path at the last publish at or before its release, so the hump is
 * still measured and the crest does not latch COLLISION. The plant
 * quantises counts (0.567 mm), which makes this phase-dependent (RTOS-03,
 * T-53): the single preemption passes at Motion phases 0-4 with IMU phase
 * 0; the every-read case latches COLLISION at every Motion phase with IMU
 * phase 0 and, with Motion one phase after the IMU (a publish inside every
 * read), passes only at IMU phases 5-8, so it runs at IMU 5, Motion 1.
 * The optional phase arguments of main() sweep other phases.
 */
static void apphost_preempt_hump (bool b_always)
{
    g_motion_phase = 1U;
    g_imu_phase = b_always ? 5U : 0U;
    apphost_start();
    apphost_run(500U);
    g_b_preempt_accel_always = b_always;
    g_raw_accel[0] = 199;
    g_raw_accel[2] = 980;
    apphost_run(800U);
    g_b_preempt_accel = true;
    apphost_run(800U);
    CHECK(!g_b_preempt_accel);
    CHECK(b_always ? (g_preemptions > 100U) : (1U == g_preemptions));
    g_raw_accel[0] = -199;
    apphost_run(1800U);
    g_raw_accel[0] = 0;
    g_raw_accel[2] = 1000;
    apphost_run(600U);
    g_b_preempt_accel_always = false;
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(0 == apphost_diagnostic_value(APPHOST_FC));
    CHECK(apphost_diagnostic_value(APPHOST_MP) > 0);
    CHECK(g_last_diagnostic.b_mission_valid);
}

/** @brief Pinned wheel speed of exactly one encoder count per 5 ms Motion
 * period (0.113 m/s). One part per million extra keeps floating-point
 * accumulation from delaying a count within any scenario horizon.
 */
static double apphost_steady_speed (void)
{
    return (0.2 * 1.000001) * 2.0 * CORE_PI * ROBOT_WHEEL_RADIUS_M /
           (0.001 * (double)ROBOT_ENCODER_COUNTS_PER_REV);
}

/** @brief NI-RTOS-2: on a steady plant, MotionTask (1 ms later phase)
 * publishes once, or twice, while one IMU I2C read is blocked. Then the
 * accelerometer shows a -3.9 m/s^2 longitudinal transient (a knock or floor
 * seam) for two IMU samples. The wheels are pinned at one count per 5 ms,
 * so the encoder path has no count quantisation (RTOS-03, T-53): only a
 * path/time skew could show an encoder deceleration. Differencing the
 * newest publish over the release-based dt latches COLLISION here; the IMU
 * step must use the publish at or before its release, so there is no
 * collision candidate and no fault. When doubled, that step's encoder
 * interval is invalid and the next one, still valid, under-estimates its
 * path by 5 ms (a T-53 residual); neither latches COLLISION.
 */
static void apphost_preempt_steady (uint32_t preempt_ms)
{
    uint32_t elapsed = 0U;

    g_motion_phase = 1U;
    apphost_start();
    apphost_run(300U);
    g_left_fraction = 0.0;
    g_right_fraction = 0.0;
    g_steady_mps = apphost_steady_speed();
    apphost_run(1000U);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(0 == apphost_diagnostic_value(APPHOST_FC));
    g_preempt_ms = preempt_ms;
    g_b_preempt_accel = true;
    for (elapsed = 0U; g_b_preempt_accel && (elapsed < 20U); ++elapsed)
    {
        apphost_run(1U);
    }
    CHECK(!g_b_preempt_accel);
    /* -3.92 m/s^2 longitudinal with |a| = 0.9996 g, as collision_guard. */
    g_raw_accel[0] = -400;
    g_raw_accel[2] = 916;
    apphost_run(20U);
    g_raw_accel[0] = 0;
    g_raw_accel[2] = 1000;
    apphost_run(100U);
    CHECK(preempt_ms == g_preemptions);
    /* Motion (phase 1) published at +1 ms, and at +6 ms when doubled. */
    CHECK(((preempt_ms + 4U) / 5U) == g_preempt_motion_steps);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(0 == apphost_diagnostic_value(APPHOST_FC));
    CHECK(!apphost_stopped());
}

/** @brief NI-MOT-2 pins the current rule (Buddy 3 decides, T-55/T-06): a
 * decoded action is pending, and the junction crossbar reaches the side
 * sensor 5 mm before both line sensors. Its dark edge opens a barcode
 * frame; NAVIGATE entry ends FOLLOW and discards the frame, which counts
 * as rejected (br), never as unknown or as a second decode.
 */
static void apphost_barcode_crossbar (void)
{
    int unknown = 0;
    int rejected = 0;

    apphost_start();
    apphost_advance(0.03);
    g_raw_ir[2] = ROBOT_IR_BARCODE_BLACK;
    apphost_advance(0.012);
    g_raw_ir[2] = ROBOT_IR_BARCODE_WHITE;
    apphost_advance(0.15);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    unknown = apphost_diagnostic_value(APPHOST_BU);
    rejected = apphost_diagnostic_value(APPHOST_BR);
    g_raw_ir[2] = ROBOT_IR_BARCODE_BLACK;
    apphost_advance(0.005);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    g_raw_ir[0] = ROBOT_IR_LEFT_BLACK;
    g_raw_ir[1] = ROBOT_IR_RIGHT_BLACK;
    apphost_run(45U);
    CHECK(VEHICLE_NAVIGATE == apphost_state());
    CHECK(unknown == apphost_diagnostic_value(APPHOST_BU));
    CHECK((rejected + 1) == apphost_diagnostic_value(APPHOST_BR));
}

/** @brief Parse single-digit Motion (0..4) and IMU (0..9) release phases. */
static bool apphost_phases (const char * p_motion, const char * p_imu)
{
    if ((NULL == p_motion) || (NULL == p_imu) ||
        (p_motion[0] < '0') || (p_motion[0] > '4') ||
        ('\0' != p_motion[1]) || (p_imu[0] < '0') ||
        (p_imu[0] > '9') || ('\0' != p_imu[1]))
    {
        return false;
    }
    /* Checked single ASCII digits convert exactly to 0..9. */
    g_override_motion_phase = (uint32_t)(p_motion[0] - '0');
    g_override_imu_phase = (uint32_t)(p_imu[0] - '0');
    g_b_phase_override = true;
    return true;
}

/** @brief Motion and Line publish newer stamps between VehicleTask's now_ms
 * capture and its snapshot; FOLLOW must continue without a fault.
 */
static void apphost_preempt_vehicle (void)
{
    g_motion_phase = 1U;
    g_line_phase = 1U;
    apphost_start();
    apphost_run(300U);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    g_b_preempt_button = true;
    apphost_run(40U);
    CHECK(!g_b_preempt_button);
    CHECK(1U == g_preemptions);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(0 == apphost_diagnostic_value(APPHOST_FC));
    CHECK(!apphost_stopped());
}

/** @brief A frontal impact while following latches COLLISION. Optionally,
 * MotionTask preempts every IMU read, which must keep the guard armed.
 */
static void apphost_collision (bool b_preempt)
{
    g_motion_phase = 1U;
    apphost_start();
    g_b_preempt_accel_always = b_preempt;
    apphost_run(1000U);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    g_b_block_left = true;
    g_b_block_right = true;
    g_left_speed = 0.0;
    g_right_speed = 0.0;
    /* -3.92 m/s^2 longitudinal with |a| = 0.9996 g: inside the gravity band. */
    g_raw_accel[0] = -400;
    g_raw_accel[2] = 916;
    apphost_run(40U);
    g_raw_accel[0] = 0;
    g_raw_accel[2] = 1000;
    apphost_run(60U);
    g_b_preempt_accel_always = false;
    CHECK(b_preempt == (g_preemptions > 90U));
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(ROBOT_APP_FAULT_COLLISION == apphost_diagnostic_value(APPHOST_FC));
    CHECK(apphost_stopped());
}

/** @brief Wiring-only commissioning stage: tasks run with calibration still
 * unconfirmed. Motors stay braked without a latched output fault.
 */
static void apphost_wiring_only (void)
{
    robot_app_diagnostic_t diagnostic = {0};

    CHECK(0 != ROBOT_WIRING_CONFIRMED);
    CHECK(0 == ROBOT_CALIBRATION_CONFIRMED);
    apphost_boot();
    apphost_run(600U);
    robot_app_diagnostic(&diagnostic);
    CHECK(ROBOT_APP_FAULT_NONE == diagnostic.code);
    CHECK(VEHICLE_INIT == apphost_state());
    CHECK(g_last_status.b_stop && (!g_last_status.b_hardfault));
    CHECK(apphost_stopped());
    CHECK(0U != g_zero_commits);
    apphost_button();
    CHECK(VEHICLE_INIT == apphost_state());
    CHECK(apphost_stopped());
    CHECK(0 == apphost_diagnostic_value(APPHOST_FC));
}

/** @brief With calibration confirmed, a rejected PWM preparation is a real
 * output failure: it brakes and latches the hard MOTOR_OUTPUT fault, which
 * a local reset cannot clear.
 */
static void apphost_motor_output (void)
{
    CHECK(0 != ROBOT_CALIBRATION_CONFIRMED);
    apphost_start();
    g_b_prepare_reject = true;
    apphost_run(10U);
    CHECK(apphost_stopped());
    g_b_prepare_reject = false;
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(g_last_status.b_hardfault);
    CHECK(ROBOT_APP_FAULT_MOTOR_OUTPUT ==
          apphost_diagnostic_value(APPHOST_FC));
    apphost_run(250U);
    apphost_button();
    CHECK(VEHICLE_FAULT == apphost_state());
    CHECK(apphost_stopped());
}

/** @brief A range pause while a barcode frame is open discards the partial
 * code; the loss is counted as rejected (br), never as a decode. The pause
 * lands mid-bar, or after a valid bar inside the trailing quiet zone, where
 * the frame stays open until quiet_zone_m of white closes it.
 */
static void apphost_barcode_range_pause (bool b_trailing)
{
    const barcode_config_t * p_config = course_cfg_codebook();
    double white_start_m = 0.0;
    double white_m = 0.0;
    uint32_t elapsed = 0U;
    int unknown = 0;
    int rejected = 0;

    apphost_start();
    apphost_advance(0.03);
    unknown = apphost_diagnostic_value(APPHOST_BU);
    rejected = apphost_diagnostic_value(APPHOST_BR);
    g_raw_ir[2] = ROBOT_IR_BARCODE_BLACK;
    /* Mid-bar: 6 mm into a bar. Trailing: after one valid 12 mm bar. */
    apphost_advance(b_trailing ? 0.012 : 0.006);
    if (b_trailing)
    {
        /* Commit the white edge (a quarter minimum run), then lose range. */
        g_raw_ir[2] = ROBOT_IR_BARCODE_WHITE;
        white_start_m = g_robot_x;
        apphost_advance(0.4 * p_config->minimum_run_m);
    }
    g_b_no_echo = true;
    while ((!apphost_stopped()) && (elapsed < 200U))
    {
        apphost_run(1U);
        ++elapsed;
    }
    apphost_run(200U - elapsed);
    if (b_trailing)
    {
        /* Includes braking travel, so it over-states the FOLLOW span. */
        white_m = g_robot_x - white_start_m;
        if (0 > printf("trailing white at pause %.4f m (quiet %.4f m)\n",
                       white_m, p_config->quiet_zone_m))
        {
            g_failures += 1U;
        }
        CHECK(white_m > (0.5 * p_config->minimum_run_m));
        CHECK(white_m < p_config->quiet_zone_m);
    }
    CHECK(VEHICLE_RANGE_WAIT == apphost_state());
    CHECK(apphost_stopped());
    g_b_no_echo = false;
    apphost_run(400U);
    CHECK(VEHICLE_FOLLOW == apphost_state());
    CHECK(unknown == apphost_diagnostic_value(APPHOST_BU));
    CHECK((rejected + 1) == apphost_diagnostic_value(APPHOST_BR));
}

/* End of file. */


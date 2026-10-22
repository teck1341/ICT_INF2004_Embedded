/** @file app_cfg.c
 * @brief Explicit SI-unit mapping; numerical defaults are not measurements.
 */
#include "app_cfg.h"
#include "robot_config.h"

/** @brief Differential drive and conservative initial output bounds. */
void app_cfg_motion (motion_config_t * p_config)
{
    p_config->odometry.left_radius_m = ROBOT_WHEEL_RADIUS_M;
    p_config->odometry.right_radius_m = ROBOT_WHEEL_RADIUS_M;
    p_config->odometry.track_width_m = ROBOT_TRACK_WIDTH_M;
    p_config->odometry.left_counts_per_revolution =
        ROBOT_ENCODER_COUNTS_PER_REV;
    p_config->odometry.right_counts_per_revolution =
        ROBOT_ENCODER_COUNTS_PER_REV;
    p_config->wheel_pid.proportional_gain = ROBOT_PID_PROPORTIONAL;
    p_config->wheel_pid.integral_gain = ROBOT_PID_INTEGRAL;
    p_config->wheel_pid.derivative_gain = ROBOT_PID_DERIVATIVE;
    p_config->wheel_pid.output_min = -ROBOT_MAX_DUTY;
    p_config->wheel_pid.output_max = ROBOT_MAX_DUTY;
    p_config->wheel_pid.max_dt_s = 0.025;
    p_config->max_wheel_mps = ROBOT_MAX_SPEED_MPS;
    p_config->max_yaw_rps = 1.0;
    p_config->distance_tolerance_m = 0.005;
    p_config->turn_tolerance_rad = 0.03;
    p_config->stationary_mps = 0.012;
    p_config->primitive_timeout_ms = 15000U;
    p_config->b_calibration_confirmed =
        (0 != ROBOT_CALIBRATION_CONFIRMED) &&
        (0 != ROBOT_WIRING_CONFIRMED);
}

/** @brief Course bounds; no START until all course inputs exist. */
void app_cfg_vehicle (vehicle_config_t * p_config, bool b_codebook_ready)
{
    p_config->forward_mps = ROBOT_LINE_SPEED_MPS;
    p_config->turn_rps = ROBOT_TURN_RATE_RPS;
    /* Approach from the junction edge to the pivot, driven before turning.
     * The constant must hold the measured detection-to-crossbar-centre
     * travel (about the sensor-to-axle offset + half the line width - the
     * 20 ms debounce travel), not the raw offset; unmeasured (T-06).
     */
    p_config->junction_distance_m = ROBOT_SENSOR_AXLE_OFFSET_M;
    p_config->reacquire_distance_m = ROBOT_REACQUIRE_DISTANCE_M;
    p_config->max_bypass_offset_m = 0.80;
    p_config->max_bypass_length_m = 1.20;
    p_config->scan_timeout_ms = 20000U;
    p_config->reacquire_timeout_ms = 10000U;
    p_config->range_max_age_ms = 150U;
    /* Proposed stopped retry bound; bench qualification is still required. */
    p_config->range_wait_timeout_ms = 1000U;
    /* Proposed bound: confirm barcode/junction placement on track. */
    p_config->pending_nav_timeout_ms = 10000U;
    p_config->b_calibration_confirmed =
        (0 != ROBOT_CALIBRATION_CONFIRMED) &&
        (0 != ROBOT_SERVO_CONFIRMED) &&
        (0 != ROBOT_WIRING_CONFIRMED) &&
        (0 != ROBOT_IMU_MOUNT_CONFIRMED);
    p_config->b_course_confirmed =
        (0 != ROBOT_COURSE_CONFIRMED) && b_codebook_ready;
}

/** @brief Per-channel calibration is independent of sensor output polarity. */
void app_cfg_line (line_ctrl_config_t * p_config)
{
    line_ctrl_defaults(p_config);
    p_config->white_adc[0] = ROBOT_IR_LEFT_WHITE;
    p_config->black_adc[0] = ROBOT_IR_LEFT_BLACK;
    p_config->white_adc[1] = ROBOT_IR_RIGHT_WHITE;
    p_config->black_adc[1] = ROBOT_IR_RIGHT_BLACK;
    p_config->white_adc[2] = ROBOT_IR_BARCODE_WHITE;
    p_config->black_adc[2] = ROBOT_IR_BARCODE_BLACK;
    p_config->b_calibration_confirmed =
        0 != ROBOT_CALIBRATION_CONFIRMED;
}

/** @brief Terrain thresholds require a separately measured flat reference. */
void app_cfg_terrain (terrain_config_t * p_config)
{
    terrain_default_config(p_config);
    p_config->b_calibrated = (0 != ROBOT_CALIBRATION_CONFIRMED) &&
                             (0 != ROBOT_IMU_MOUNT_CONFIRMED);
}

/** @brief Servo settling uses the slower of design and fitted-servo needs. */
void app_cfg_scanner (scanner_config_t * p_config)
{
    scanner_default_config(p_config);
    p_config->settle_ms = ROBOT_SERVO_SETTLE_MS;
    p_config->robot_half_width_m = ROBOT_ROBOT_WIDTH_M / 2.0;
    p_config->uncertainty_margin_m = ROBOT_OBSTACLE_MARGIN_M;
    p_config->obstacle_distance_m = 0.60;
    p_config->required_clearance_m = 0.50;
    p_config->b_calibrated = (0 != ROBOT_CALIBRATION_CONFIRMED) &&
                             (0 != ROBOT_SERVO_CONFIRMED);
}

/* End of file. */


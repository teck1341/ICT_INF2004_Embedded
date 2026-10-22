/**
 * @file robot_config.h
 * @brief Explicit, unconfirmed physical configuration for the supplied kit.
 * @details Values below are design hypotheses, not measured calibration.
 * Record evidence in the validation log before changing confirmation gates.
 */
#ifndef ROBOT_CONFIG_H
#define ROBOT_CONFIG_H

/* Independent gates prevent guessed calibration from energising actuators. */
#define ROBOT_CALIBRATION_CONFIRMED 0
#define ROBOT_CALIBRATION_VERSION 0U
#define ROBOT_SERVO_CONFIRMED 0
#define ROBOT_BARCODE_CONFIRMED 0
#define ROBOT_WIRING_CONFIRMED 0

/* 2 = x4 quadrature; 1 = both A edges, B ignored. Verify the fitted sensor.
 * A single-channel sensor cannot measure direction. The optional mode uses
 * applied motor direction and must be explicitly accepted after coast and
 * reversal trials. Edges while braked have unknown sign and are rejected.
 */
#define ROBOT_ENCODER_CHANNELS 2
#define ROBOT_SINGLE_CHANNEL_DIRECTION_CONFIRMED 0
#if (ROBOT_ENCODER_CHANNELS != 1) && (ROBOT_ENCODER_CHANNELS != 2)
#error "ROBOT_ENCODER_CHANNELS must be 1 or 2"
#endif

/* Pinned BSP clock; checked against BSP CPU clock configuration at install.
 * ROBOT_KERNEL_TICK_MS is documentation only and is not read by firmware:
 * the installer sets CNF_TIMER_PERIOD to 1 itself. ROBOT_PWM_HZ is read only
 * by the compile-time check in board.c; the PWM uses ROBOT_PWM_WRAP.
 */
#define ROBOT_CLOCK_HZ 125000000U
#define ROBOT_KERNEL_TICK_MS 1U
#define ROBOT_PWM_HZ 20000U
#define ROBOT_PWM_WRAP 6249U
#define ROBOT_SERVO_FRAME_US 20000U
#define ROBOT_SERVO_MIN_US 1000U
#define ROBOT_SERVO_CENTRE_US 1500U
#define ROBOT_SERVO_ANGLE_SIGN 1
#define ROBOT_SERVO_MAX_US 2000U

/* Pico GPIO numbers, not physical header pins. Each device sits on one Robo
 * Pico connector: Grove 1 = HC-SR04 TRIG GP0 / ECHO GP1 (5 V module: the
 * ECHO divider is mandatory), Grove 2 = left wheel encoder A/B, Grove 3 =
 * accelerometer on I2C0 (GP4 SDA, GP5 SCL), Grove 4 = right wheel encoder
 * A/B (swap the two encoder pairs here if the wheels are wired the other
 * way), motor driver M1 = GP8/GP9 (left wheel, PWM slice 4) and M2 =
 * GP10/GP11 (right wheel, PWM slice 5), so the Grove 2 encoder must sit on
 * the M1 wheel, Grove 5/6/7 = IR left/right/barcode (analogue A0 on ADC0..2,
 * digital D0 on GP6/GP13/GP7) and servo port 1 (GP12) = scan servo. Grove 5
 * and Grove 6 both carry GP26, so the right sensor's D0 is rewired from
 * Grove 6 to GP13 (servo port 2). The firmware reads only the analogue
 * outputs and never configures the D0 pins, which stay inputs from reset.
 * GP14 and GP15 are kept for servos. GP0/GP1 are also the UART0
 * console pins: the console owns them until board_init() hands them to the
 * sonar, and a start-up refusal returns them to UART0 to print its reason.
 * Kernel fault messages after start-up reach no UART pin: use SWD.
 * TRIG must sit on the console TX pin and ECHO on RX, so the boot console
 * never drives against the sensor's ECHO output; board.c checks this.
 * Wire Grove contacts by silkscreen GPIO label, not pin number.
 * Both encoder inputs of one wheel must share an IO_BANK0 interrupt bank
 * (GPIO 0-7, 8-15, 16-23); board.c checks this.
 */
#define ROBOT_PIN_ENCODER_LEFT_A 2U
#define ROBOT_PIN_ENCODER_LEFT_B 3U
#define ROBOT_PIN_ENCODER_RIGHT_A 16U
#define ROBOT_PIN_ENCODER_RIGHT_B 17U
#define ROBOT_PIN_MOTOR_LEFT_A 8U
#define ROBOT_PIN_MOTOR_LEFT_B 9U
#define ROBOT_PIN_MOTOR_RIGHT_A 10U
#define ROBOT_PIN_MOTOR_RIGHT_B 11U
#define ROBOT_PIN_SERVO 12U
#define ROBOT_PIN_IMU_SDA 4U
#define ROBOT_PIN_IMU_SCL 5U
#define ROBOT_PIN_TRIGGER 0U
#define ROBOT_PIN_ECHO 1U
#define ROBOT_PIN_CONSOLE_TX 0U
#define ROBOT_PIN_CONSOLE_RX 1U
#define ROBOT_PIN_STOP 20U
#define ROBOT_PIN_START 21U
#define ROBOT_PIN_IR_LEFT 26U
#define ROBOT_PIN_IR_RIGHT 27U
#define ROBOT_PIN_IR_BARCODE 28U
/* IR digital D0 outputs: wired, not read by the firmware. */
#define ROBOT_PIN_IR_LEFT_AUX 6U
#define ROBOT_PIN_IR_RIGHT_AUX 13U
#define ROBOT_PIN_IR_BARCODE_AUX 7U

/* Mechanical hypotheses: replace with measured loaded-wheel parameters.
 * Counts/revolution means decoded edges at the WHEEL shaft: x4 for two
 * channels, BOTH A edges for one channel. No single-channel value is assumed.
 */
#define ROBOT_WHEEL_RADIUS_M 0.0325
#define ROBOT_TRACK_WIDTH_M 0.140
#if ROBOT_ENCODER_CHANNELS == 2
#define ROBOT_ENCODER_COUNTS_PER_REV 360U
#else
#define ROBOT_ENCODER_COUNTS_PER_REV 0U
#endif
#define ROBOT_LEFT_ENCODER_SIGN 1
#define ROBOT_RIGHT_ENCODER_SIGN 1
#define ROBOT_LEFT_MOTOR_SIGN 1
#define ROBOT_RIGHT_MOTOR_SIGN 1
#define ROBOT_MAX_SPEED_MPS 0.15
#define ROBOT_MAX_DUTY 0.35
/* No acceleration/setpoint ramp exists: wheel targets step directly. The PID
 * integral is held while saturated and bounded by the +/-ROBOT_MAX_DUTY
 * output limits (core.c); there is no separate integral limit.
 */
#define ROBOT_PID_PROPORTIONAL 1.0
#define ROBOT_PID_INTEGRAL 0.3
#define ROBOT_PID_DERIVATIVE 0.0

/* Optical hypotheses. Confirm polarity and each black/white distribution.
 * ROBOT_LINE_SENSOR_SPACING_M and ROBOT_BARCODE_LATERAL_OFFSET_M are
 * geometry records only: documentation, not read by firmware (a barcode
 * coordinate along the side sensor's track, T-56, would need the offset).
 * ROBOT_SENSOR_AXLE_OFFSET_M is the NAVIGATE approach distance (app_cfg.c
 * junction_distance_m). Enter the MEASURED travel from junction-edge
 * detection to the axle over the crossbar centre (about the sensor-to-axle
 * offset + half the line width - the debounce/detection travel), not the
 * raw geometric offset (T-06). 0.060 is the unmeasured offset hypothesis.
 */
#define ROBOT_IR_LEFT_WHITE 3500U
#define ROBOT_IR_LEFT_BLACK 500U
#define ROBOT_IR_RIGHT_WHITE 3500U
#define ROBOT_IR_RIGHT_BLACK 500U
#define ROBOT_IR_BARCODE_WHITE 3500U
#define ROBOT_IR_BARCODE_BLACK 500U
#define ROBOT_LINE_SENSOR_SPACING_M 0.018
#define ROBOT_BARCODE_LATERAL_OFFSET_M 0.035
#define ROBOT_SENSOR_AXLE_OFFSET_M 0.060

/* Sensor transport and conservative initial thresholds, still unmeasured. */
#define ROBOT_I2C_HZ 100000U
#define ROBOT_I2C_TIMEOUT_US 2000U
#define ROBOT_ECHO_TIMEOUT_US 30000U
#define ROBOT_TRIGGER_INTERVAL_US 65000U
#define ROBOT_TRIGGER_PULSE_US 10U
#define ROBOT_SERVO_SETTLE_MS 180U
/* Integer limits compare with shared fields that are rounded up (ceil mm,
 * magnitude-up mrad), which keeps the original double decisions exact;
 * MM / 1000.0 is the same double as the former 0.25 literal. */
#define ROBOT_FRONT_STOP_DISTANCE_MM 250U
#define ROBOT_FRONT_STOP_DISTANCE_M (ROBOT_FRONT_STOP_DISTANCE_MM / 1000.0)
#define ROBOT_ROBOT_WIDTH_M 0.18
#define ROBOT_OBSTACLE_MARGIN_M 0.06
#define ROBOT_REACQUIRE_DISTANCE_M 0.60
#define ROBOT_ACCEL_BIAS_X_MG 0
#define ROBOT_ACCEL_BIAS_Y_MG 0
#define ROBOT_ACCEL_BIAS_Z_MG 0
#define ROBOT_ACCEL_SCALE_X 1.0
#define ROBOT_ACCEL_SCALE_Y 1.0
#define ROBOT_ACCEL_SCALE_Z 1.0

/* Course bounds require actual track measurements and an approved codebook.
 * Free-corridor confirmation is an environmental assumption: a frontal scan
 * cannot see the rear depth of an obstacle or prove an entire swept path.
 */
#define ROBOT_COURSE_CONFIRMED 0
#define ROBOT_CORRIDOR_CONFIRMED 0
#define ROBOT_MAX_OBSTACLE_DEPTH_M 0.30
#define ROBOT_ROBOT_LENGTH_M 0.22
#define ROBOT_LINE_UNIQUE_CORRIDOR_M 0.03
#define ROBOT_LINE_HEADING_TOL_RAD 0.20
#define ROBOT_LINE_LOCK_MS 100U
#define ROBOT_TURN_RATE_RPS 0.65
#define ROBOT_LINE_GAIN_RPS 0.75
#define ROBOT_LINE_SPEED_MPS 0.08
#define ROBOT_STALL_MS 500U
/* Unmeasured stall hypothesis: a wheel commanded at |duty| >= this value
 * with no encoder count for ROBOT_STALL_MS latches a hard stall fault. The
 * slow PID ramp from rest (integral build-up, not a setpoint ramp) can trip
 * it if breakaway duty exceeds about 0.16; measure breakaway (T-59).
 */
#define ROBOT_STALL_DUTY 0.15
#define ROBOT_TILT_LIMIT_MRAD 600
#define ROBOT_TILT_LIMIT_RAD (ROBOT_TILT_LIMIT_MRAD / 1000.0)
#define ROBOT_IMU_REFERENCE_X_MG 0
#define ROBOT_IMU_REFERENCE_Y_MG 0
#define ROBOT_IMU_REFERENCE_Z_MG 1000
#define ROBOT_IMU_MOUNT_CONFIRMED 0
/* Axis indices/signs map the fitted sensor to x forward, y left, z up. */
#define ROBOT_IMU_FORWARD_AXIS 0U
#define ROBOT_IMU_LEFT_AXIS 1U
#define ROBOT_IMU_UP_AXIS 2U
#define ROBOT_IMU_FORWARD_SIGN 1
#define ROBOT_IMU_LEFT_SIGN 1
#define ROBOT_IMU_UP_SIGN 1

#endif /* ROBOT_CONFIG_H */

/* End of file. */


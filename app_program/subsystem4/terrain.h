/** @file terrain.h
 * @brief Quasi-static accelerometer terrain and encoder motion estimates.
 *
 * Sensor axes: x forward, y left, z up; stationary flat z is positive gravity.
 * Pitch is positive nose-up. No gyroscope signal is used or synthesized.
 * Direct double use follows project numeric deviation D-NUMERIC. Module
 * output and all internal maths stay double; only the IMU -> all record
 * (terrain_shared_t) is packed to minimum widths.
 */
#ifndef TERRAIN_H
#define TERRAIN_H

#include <stdbool.h>
#include <stdint.h>

/** Shared record widths; signed fields use bitpack_signed/bitpack_extend. */
#define TERRAIN_ANGLE_BITS (12U)
#define TERRAIN_RATE_BITS (13U)
#define TERRAIN_HEIGHT_BITS (9U)
#define TERRAIN_HEIGHT_MAX_MM (511U)
/** terrain_angle_mrad of the reserved angle code: beyond every limit. */
#define TERRAIN_ANGLE_INVALID_MRAD (2048U)

typedef enum
{
    TERRAIN_INVALID = 0,
    TERRAIN_STATIONARY,
    TERRAIN_STEADY,
    TERRAIN_ACCELERATING,
    TERRAIN_BRAKING,
    TERRAIN_TURNING,
    TERRAIN_CLIMBING,
    TERRAIN_DESCENDING,
    TERRAIN_COLLISION_CANDIDATE
} terrain_motion_t;

typedef struct
{
    double gravity_mps2;
    double gravity_tolerance_mps2;
    double filter_time_s;
    double maximum_dt_s;
    double slope_enter_rad;
    double slope_exit_rad;
    double flat_confirm_s;
    double minimum_hump_m;
    double maximum_hump_m;
    double closure_tolerance_m;
    double stationary_speed_mps;
    double acceleration_threshold_mps2;
    double collision_deceleration_mps2;
    double turning_threshold_rps;
    bool   b_calibrated;
} terrain_config_t;

typedef struct
{
    double accel_x_mps2;
    double accel_y_mps2;
    double accel_z_mps2;
    double signed_distance_m;
    double encoder_yaw_rate_rps;
    double dt_s;
    bool   b_sensor_valid;
    bool   b_encoder_valid;
    bool   b_quasistatic;
} terrain_input_t;

typedef struct
{
    double           pitch_rad;
    double           roll_rad;
    double           encoder_yaw_rate_rps;
    double           height_estimate_m;
    double           hump_peak_m;
    double           last_hump_peak_m;
    double           run_maximum_m;
    /* Valid humps this run, saturating at UINT8_MAX. */
    uint8_t          completed_humps;
    terrain_motion_t motion                : 4;
    bool             b_tilt_valid          : 1;
    bool             b_height_valid        : 1;
    bool             b_hump_active         : 1;
    bool             b_hump_completed      : 1;
    bool             b_last_hump_valid     : 1;
    bool             b_collision_candidate : 1;
} terrain_output_t;

typedef struct
{
    terrain_config_t config;
    terrain_output_t output;
    double           pitch_reference_rad;
    double           roll_reference_rad;
    double           previous_speed_mps;
    double           filtered_pitch_rad;
    double           filtered_roll_rad;
    double           flat_time_s;
    bool             b_initialized     : 1;
    bool             b_reference_valid : 1;
    bool             b_filter_valid    : 1;
    bool             b_speed_valid     : 1;
    bool             b_active_valid    : 1;
    bool             b_descent_seen    : 1;
} terrain_state_t;

/** IMU -> all record, 64 bits. Pitch/roll: 12-bit signed mrad, magnitude
 * rounded UP (AWAY), saturating at +/-2047, reserved code when tilt is
 * invalid; keeps |angle| > 600 mrad decisions exact. Motion 4 bits, flags.
 * Yaw rate 13-bit signed mrad/s (nearest, reserved = non-finite); run
 * maximum and mission maximum 9-bit mm (FLOOR, 0..511).
 * b_tilt_valid is set only when both angle fields hold measurements, so a
 * reserved code is never compared as an angle.
 */
typedef struct
{
    uint32_t         pitch_mrad            : 12;
    uint32_t         roll_mrad             : 12;
    terrain_motion_t motion                : 4;
    bool             b_tilt_valid          : 1;
    bool             b_collision_candidate : 1;
    bool             b_hump_active         : 1;
    bool             b_last_hump_valid     : 1;
    uint32_t         yaw_mrad_s            : 13;
    uint32_t         run_maximum_mm        : 9;
    uint32_t         mission_maximum_mm    : 9;
    bool             b_mission_valid       : 1;
} terrain_shared_t;

/** Populate proposed thresholds; calibration remains false. Null is ignored. */
void terrain_default_config (terrain_config_t * p_config);

/** Validate configuration and reset all state. False leaves state unchanged. */
bool terrain_init (terrain_state_t * p_state,
                   const terrain_config_t * p_config);

/**
 * Capture mounting reference from a known-flat stationary mean acceleration.
 * Caller must supply a previously averaged, calibrated sample and stationary
 * evidence; one arbitrary live sample is not a calibration procedure.
 * Refuses active humps, non-gravity samples and uncalibrated configuration.
 */
bool terrain_reference (terrain_state_t * p_state,
                        double accel_x_mps2,
                        double accel_y_mps2,
                        double accel_z_mps2,
                        bool b_stationary);

/**
 * Advance one sample. Invalid samples explicitly invalidate current estimates
 * and the active hump, while preserving previously valid completed run maxima.
 * Height integrates signed path * sin(filtered pitch), a chassis-path estimate.
 * A complete hump needs uphill onset, a forward descent outside the flat band,
 * then flat dwell and height closure. Flat crests leave the event open.
 * This does not establish physical ground height. Caller serializes all calls.
 */
bool terrain_step (terrain_state_t * p_state,
                   const terrain_input_t * p_input,
                   terrain_output_t * p_output);

/**
 * Pack one step's output into the shared record. The caller supplies the
 * mission maximum (integer maximum of terrain_height_mm values, saturated
 * at 511) and its flag. Packing uses floating point, so call it before
 * taking the shared-data lock. NULL arguments are ignored.
 */
void terrain_shared_pack (const terrain_output_t * p_output,
                          uint32_t mission_maximum_mm, bool b_mission_valid,
                          terrain_shared_t * p_shared);

/** FLOOR to whole mm, 0..511, for heights (NaN or negative -> 0). */
uint32_t terrain_height_mm (double height_m);

/**
 * |angle| in mrad of a packed pitch or roll field; the reserved code reads
 * as TERRAIN_ANGLE_INVALID_MRAD. For the 600 mrad tilt limit,
 * terrain_angle_mrad(field) > 600U equals fabs(angle_rad) > 0.60 for every
 * double angle (and <= equals <=); re-verify any other limit by test.
 */
uint32_t terrain_angle_mrad (uint32_t field);

#endif /* TERRAIN_H */

/** End of file. */


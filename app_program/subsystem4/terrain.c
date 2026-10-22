/** @file terrain.c
 * @brief Accelerometer tilt, bounded hump integration and event candidates.
 */
#include "terrain.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "bitpack.h"
#include "core.h"

#if DBL_MANT_DIG < 53
#error "Terrain calculations require at least 53 double mantissa bits."
#endif

/* Constant masks let -Wconversion prove each bit-field store fits. */
#define TERRAIN_ANGLE_MASK ((UINT32_C(1) << TERRAIN_ANGLE_BITS) - 1U)
#define TERRAIN_RATE_MASK ((UINT32_C(1) << TERRAIN_RATE_BITS) - 1U)
#define TERRAIN_HEIGHT_MASK ((UINT32_C(1) << TERRAIN_HEIGHT_BITS) - 1U)
#define TERRAIN_MILLI_PER_UNIT (1000.0)

static bool terrain_config_valid (const terrain_config_t * p_config);
static bool terrain_input_valid (const terrain_input_t * p_input);
static bool terrain_angles (const terrain_config_t * p_config,
                            double accel_x, double accel_y, double accel_z,
                            double * p_pitch, double * p_roll);
static void terrain_invalidate (terrain_state_t * p_state);
static void terrain_filter (terrain_state_t * p_state,
                            const terrain_input_t * p_input,
                            double pitch, double roll);
static void terrain_motion (terrain_state_t * p_state,
                            const terrain_input_t * p_input);
static void terrain_hump (terrain_state_t * p_state,
                          const terrain_input_t * p_input);
static void terrain_finish_hump (terrain_state_t * p_state);

/** Populate physically plausible starting thresholds requiring bench tuning. */
void terrain_default_config (terrain_config_t * p_config)
{
    if (NULL != p_config)
    {
        p_config->gravity_mps2                  = 9.80665;
        p_config->gravity_tolerance_mps2        = 0.8;
        p_config->filter_time_s                 = 0.08;
        p_config->maximum_dt_s                  = 0.05;
        p_config->slope_enter_rad               = 0.08;
        p_config->slope_exit_rad                = 0.035;
        p_config->flat_confirm_s                = 0.20;
        p_config->minimum_hump_m                = 0.005;
        p_config->maximum_hump_m                = 0.30;
        p_config->closure_tolerance_m           = 0.03;
        p_config->stationary_speed_mps          = 0.005;
        p_config->acceleration_threshold_mps2   = 0.30;
        p_config->collision_deceleration_mps2   = 3.0;
        p_config->turning_threshold_rps         = 0.25;
        p_config->b_calibrated                  = false;
    }
}

/** Reject invalid parameters before modifying caller-owned state. */
bool terrain_init (terrain_state_t * p_state,
                   const terrain_config_t * p_config)
{
    terrain_config_t config_copy;

    if ((NULL == p_state) || (!terrain_config_valid(p_config)))
    {
        return false;
    }
    config_copy = *p_config;
    memset(p_state, 0, sizeof(*p_state));
    p_state->config        = config_copy;
    p_state->b_initialized = true;
    terrain_invalidate(p_state);
    return true;
}

/** Capture a caller-verified known-flat reference without erasing run data. */
bool terrain_reference (terrain_state_t * p_state,
                        double accel_x_mps2,
                        double accel_y_mps2,
                        double accel_z_mps2,
                        bool b_stationary)
{
    double pitch = 0.0;
    double roll  = 0.0;

    if ((NULL == p_state) || (!p_state->b_initialized)
        || (!p_state->config.b_calibrated) || (!b_stationary)
        || (p_state->output.b_hump_active))
    {
        return false;
    }
    if (!terrain_angles(&p_state->config, accel_x_mps2, accel_y_mps2,
                        accel_z_mps2, &pitch, &roll))
    {
        return false;
    }
    p_state->pitch_reference_rad = pitch;
    p_state->roll_reference_rad  = roll;
    p_state->b_reference_valid   = true;
    p_state->b_filter_valid      = false;
    p_state->b_speed_valid       = false;
    return true;
}

/** Process an observation; unavailable estimates carry explicit flags. */
bool terrain_step (terrain_state_t * p_state,
                   const terrain_input_t * p_input,
                   terrain_output_t * p_output)
{
    double pitch   = 0.0;
    double roll    = 0.0;
    bool   b_tilt  = false;

    if ((NULL == p_state) || (NULL == p_output)
        || (!p_state->b_initialized))
    {
        return false;
    }
    p_state->output.encoder_yaw_rate_rps   = NAN;
    p_state->output.b_hump_completed      = false;
    p_state->output.b_collision_candidate = false;
    if ((!terrain_input_valid(p_input))
        || (p_input->dt_s > p_state->config.maximum_dt_s)
        || (!p_state->b_reference_valid))
    {
        terrain_invalidate(p_state);
        p_state->b_speed_valid = false;
        *p_output = p_state->output;
        return false;
    }
    b_tilt = p_input->b_sensor_valid && p_input->b_quasistatic
             && (terrain_angles(&p_state->config, p_input->accel_x_mps2,
                                p_input->accel_y_mps2,
                                p_input->accel_z_mps2, &pitch, &roll));
    if (b_tilt)
    {
        terrain_filter(p_state, p_input, pitch, roll);
    }
    else
    {
        terrain_invalidate(p_state);
    }
    terrain_motion(p_state, p_input);
    if (b_tilt && p_input->b_encoder_valid)
    {
        terrain_hump(p_state, p_input);
    }
    else
    {
        p_state->b_active_valid        = false;
        p_state->output.b_height_valid = false;
    }
    *p_output = p_state->output;
    return true;
}

/** Quantise angles (AWAY), yaw (NEAREST) and heights (FLOOR) to fields. */
void terrain_shared_pack (const terrain_output_t * p_output,
                          uint32_t mission_maximum_mm, bool b_mission_valid,
                          terrain_shared_t * p_shared)
{
    const int32_t angle_limit = bitpack_signed_max(TERRAIN_ANGLE_BITS);
    const int32_t rate_limit  = bitpack_signed_max(TERRAIN_RATE_BITS);
    int32_t pitch_mrad = 0;
    int32_t roll_mrad  = 0;
    int32_t rate_mrad_s = 0;
    bool b_tilt = false;

    if ((NULL == p_output) || (NULL == p_shared))
    {
        return;
    }
    /* A non-finite angle (never published with a valid flag) clears the
     * flag, so the reserved code is never read as a measurement. */
    b_tilt = p_output->b_tilt_valid
             && bitpack_quantise(p_output->pitch_rad, TERRAIN_MILLI_PER_UNIT,
                                 BITPACK_AWAY, -angle_limit, angle_limit,
                                 &pitch_mrad)
             && bitpack_quantise(p_output->roll_rad, TERRAIN_MILLI_PER_UNIT,
                                 BITPACK_AWAY, -angle_limit, angle_limit,
                                 &roll_mrad);
    if (b_tilt)
    {
        p_shared->pitch_mrad = bitpack_signed(pitch_mrad, TERRAIN_ANGLE_BITS)
                               & TERRAIN_ANGLE_MASK;
        p_shared->roll_mrad  = bitpack_signed(roll_mrad, TERRAIN_ANGLE_BITS)
                               & TERRAIN_ANGLE_MASK;
    }
    else
    {
        p_shared->pitch_mrad = bitpack_invalid(TERRAIN_ANGLE_BITS)
                               & TERRAIN_ANGLE_MASK;
        p_shared->roll_mrad  = bitpack_invalid(TERRAIN_ANGLE_BITS)
                               & TERRAIN_ANGLE_MASK;
    }
    if (bitpack_quantise(p_output->encoder_yaw_rate_rps,
                         TERRAIN_MILLI_PER_UNIT, BITPACK_NEAREST,
                         -rate_limit, rate_limit, &rate_mrad_s))
    {
        p_shared->yaw_mrad_s = bitpack_signed(rate_mrad_s, TERRAIN_RATE_BITS)
                               & TERRAIN_RATE_MASK;
    }
    else
    {
        p_shared->yaw_mrad_s = bitpack_invalid(TERRAIN_RATE_BITS)
                               & TERRAIN_RATE_MASK;
    }
    p_shared->motion                = p_output->motion;
    p_shared->b_tilt_valid          = b_tilt;
    p_shared->b_collision_candidate = p_output->b_collision_candidate;
    p_shared->b_hump_active         = p_output->b_hump_active;
    p_shared->b_last_hump_valid     = p_output->b_last_hump_valid;
    p_shared->run_maximum_mm = terrain_height_mm(p_output->run_maximum_m)
                               & TERRAIN_HEIGHT_MASK;
    p_shared->mission_maximum_mm = bitpack_saturate(mission_maximum_mm,
                                                    TERRAIN_HEIGHT_BITS)
                                   & TERRAIN_HEIGHT_MASK;
    p_shared->b_mission_valid = b_mission_valid;
}

/** Floor equals the legacy truncation for every nonnegative height. */
uint32_t terrain_height_mm (double height_m)
{
    int32_t height_mm = 0;

    /* Negative heights clamp to 0; a non-finite height is no measurement.
     * The 511 mm limit is exact in int32_t. */
    if (!bitpack_quantise(height_m, TERRAIN_MILLI_PER_UNIT, BITPACK_FLOOR,
                          0, (int32_t)TERRAIN_HEIGHT_MAX_MM, &height_mm))
    {
        return 0U;
    }
    /* Clamped to 0..511 above, so the conversion is exact. */
    return (uint32_t)height_mm;
}

/** Sign-extend, then take the magnitude; -2047..2047 plus reserved -2048. */
uint32_t terrain_angle_mrad (uint32_t field)
{
    const int32_t angle_mrad = bitpack_extend(field & TERRAIN_ANGLE_MASK,
                                              TERRAIN_ANGLE_BITS);

    /* The magnitude is at most 2048, so both conversions are exact. */
    return (angle_mrad < 0) ? (uint32_t)(-angle_mrad) : (uint32_t)angle_mrad;
}

/** Validate every floating threshold, including ordering constraints. */
static bool terrain_config_valid (const terrain_config_t * p_config)
{
    if (NULL == p_config)
    {
        return false;
    }
    return (isfinite(p_config->gravity_mps2))
           && (p_config->gravity_mps2 > 0.0)
           && (isfinite(p_config->gravity_tolerance_mps2))
           && (p_config->gravity_tolerance_mps2 > 0.0)
           && (p_config->gravity_tolerance_mps2 < p_config->gravity_mps2)
           && (isfinite(p_config->filter_time_s))
           && (p_config->filter_time_s >= 0.0)
           && (isfinite(p_config->maximum_dt_s))
           && (p_config->maximum_dt_s > 0.0)
           && (isfinite(p_config->slope_enter_rad))
           && (p_config->slope_enter_rad > 0.0)
           && (p_config->slope_enter_rad < (CORE_PI / 2.0))
           && (isfinite(p_config->slope_exit_rad))
           && (p_config->slope_exit_rad >= 0.0)
           && (p_config->slope_exit_rad < p_config->slope_enter_rad)
           && (isfinite(p_config->flat_confirm_s))
           && (p_config->flat_confirm_s > 0.0)
           && (isfinite(p_config->minimum_hump_m))
           && (p_config->minimum_hump_m > 0.0)
           && (isfinite(p_config->maximum_hump_m))
           && (p_config->maximum_hump_m > p_config->minimum_hump_m)
           && (isfinite(p_config->closure_tolerance_m))
           && (p_config->closure_tolerance_m >= 0.0)
           && (isfinite(p_config->stationary_speed_mps))
           && (p_config->stationary_speed_mps >= 0.0)
           && (isfinite(p_config->acceleration_threshold_mps2))
           && (p_config->acceleration_threshold_mps2 > 0.0)
           && (isfinite(p_config->collision_deceleration_mps2))
           && (p_config->collision_deceleration_mps2
               > p_config->acceleration_threshold_mps2)
           && (isfinite(p_config->turning_threshold_rps))
           && (p_config->turning_threshold_rps > 0.0);
}

/** Numeric validity is distinct from sensor/encoder trust flags. */
static bool terrain_input_valid (const terrain_input_t * p_input)
{
    return (NULL != p_input)
           && (isfinite(p_input->accel_x_mps2))
           && (isfinite(p_input->accel_y_mps2))
           && (isfinite(p_input->accel_z_mps2))
           && (isfinite(p_input->signed_distance_m))
           && (isfinite(p_input->encoder_yaw_rate_rps))
           && (isfinite(p_input->dt_s)) && (p_input->dt_s > 0.0);
}

/** Recover gravity orientation only in the calibrated near-gravity envelope. */
static bool terrain_angles (const terrain_config_t * p_config,
                            double accel_x, double accel_y, double accel_z,
                            double * p_pitch, double * p_roll)
{
    const double norm = hypot(accel_x, hypot(accel_y, accel_z));

    if ((!isfinite(norm)) || (accel_z <= 0.0)
        || (fabs(norm - p_config->gravity_mps2)
            > p_config->gravity_tolerance_mps2))
    {
        return false;
    }
    *p_pitch = atan2(accel_x, hypot(accel_y, accel_z));
    *p_roll  = atan2(accel_y, accel_z);
    return (isfinite(*p_pitch)) && (isfinite(*p_roll));
}

/** Preserve completed heights, but invalidate partial integration and tilt. */
static void terrain_invalidate (terrain_state_t * p_state)
{
    p_state->output.pitch_rad          = NAN;
    p_state->output.roll_rad           = NAN;
    p_state->output.encoder_yaw_rate_rps = NAN;
    p_state->output.b_tilt_valid       = false;
    p_state->output.b_height_valid     = false;
    p_state->output.motion             = TERRAIN_INVALID;
    p_state->b_filter_valid            = false;
    p_state->b_active_valid            = false;
    p_state->flat_time_s               = 0.0;
}

/** First-order low pass; a fresh reference initializes without zero ramping. */
static void terrain_filter (terrain_state_t * p_state,
                            const terrain_input_t * p_input,
                            double pitch, double roll)
{
    const double alpha = p_input->dt_s
                         / (p_state->config.filter_time_s + p_input->dt_s);

    pitch -= p_state->pitch_reference_rad;
    roll  -= p_state->roll_reference_rad;
    if (!p_state->b_filter_valid)
    {
        p_state->filtered_pitch_rad = pitch;
        p_state->filtered_roll_rad  = roll;
        p_state->b_filter_valid     = true;
    }
    else
    {
        p_state->filtered_pitch_rad += alpha
                                      * (pitch - p_state->filtered_pitch_rad);
        p_state->filtered_roll_rad  += alpha
                                      * (roll - p_state->filtered_roll_rad);
    }
    p_state->output.pitch_rad    = p_state->filtered_pitch_rad;
    p_state->output.roll_rad     = p_state->filtered_roll_rad;
    p_state->output.b_tilt_valid = true;
}

/** Classify motion; hard braking can also produce an impact candidate. */
static void terrain_motion (terrain_state_t * p_state,
                            const terrain_input_t * p_input)
{
    const terrain_config_t * p_config = &p_state->config;
    const double speed = p_input->signed_distance_m / p_input->dt_s;
    double acceleration = 0.0;
    double longitudinal = p_input->accel_x_mps2;

    if ((!p_input->b_encoder_valid) || (!isfinite(speed)))
    {
        p_state->output.motion = TERRAIN_INVALID;
        p_state->b_speed_valid = false;
        return;
    }
    if (p_state->b_speed_valid)
    {
        acceleration = (speed - p_state->previous_speed_mps) / p_input->dt_s;
    }
    if (!isfinite(acceleration))
    {
        p_state->output.motion = TERRAIN_INVALID;
        p_state->b_speed_valid = false;
        return;
    }
    longitudinal -= p_config->gravity_mps2
                    * sin(p_state->filtered_pitch_rad
                          + p_state->pitch_reference_rad);
    p_state->output.encoder_yaw_rate_rps = p_input->encoder_yaw_rate_rps;
    p_state->output.b_collision_candidate = p_state->b_speed_valid
        && p_input->b_sensor_valid
        && (acceleration < -p_config->collision_deceleration_mps2)
        && (longitudinal < -p_config->collision_deceleration_mps2);
    p_state->previous_speed_mps = speed;
    p_state->b_speed_valid      = true;
    if (p_state->output.b_collision_candidate)
    {
        p_state->output.motion = TERRAIN_COLLISION_CANDIDATE;
    }
    else if (fabs(p_input->encoder_yaw_rate_rps)
             > p_config->turning_threshold_rps)
    {
        p_state->output.motion = TERRAIN_TURNING;
    }
    else if (fabs(speed) <= p_config->stationary_speed_mps)
    {
        p_state->output.motion = TERRAIN_STATIONARY;
    }
    else if (p_state->output.b_tilt_valid
             && (p_state->output.pitch_rad >= p_config->slope_enter_rad))
    {
        p_state->output.motion = TERRAIN_CLIMBING;
    }
    else if (p_state->output.b_tilt_valid
             && (p_state->output.pitch_rad <= -p_config->slope_enter_rad))
    {
        p_state->output.motion = TERRAIN_DESCENDING;
    }
    else if (acceleration > p_config->acceleration_threshold_mps2)
    {
        p_state->output.motion = TERRAIN_ACCELERATING;
    }
    else if (acceleration < -p_config->acceleration_threshold_mps2)
    {
        p_state->output.motion = TERRAIN_BRAKING;
    }
    else
    {
        p_state->output.motion = TERRAIN_STEADY;
    }
}

/** Integrate one forward near-straight observation with explicit closure. */
static void terrain_hump (terrain_state_t * p_state,
                          const terrain_input_t * p_input)
{
    terrain_output_t * p_out = &p_state->output;
    const terrain_config_t * p_cfg = &p_state->config;
    double increment = 0.0;

    if ((!p_out->b_hump_active)
        && (p_out->pitch_rad >= p_cfg->slope_enter_rad)
        && (p_input->signed_distance_m > 0.0))
    {
        p_out->b_hump_active     = true;
        p_out->height_estimate_m = 0.0;
        p_out->hump_peak_m       = 0.0;
        p_state->flat_time_s     = 0.0;
        p_state->b_active_valid  = true;
        p_state->b_descent_seen  = false;
    }
    if (!p_out->b_hump_active)
    {
        p_out->b_height_valid = false;
        return;
    }
    if ((p_input->signed_distance_m < 0.0)
        || (fabs(p_input->encoder_yaw_rate_rps)
            > p_cfg->turning_threshold_rps)
        || (CORE_OK != core_hump_height_increment(p_input->signed_distance_m,
                       p_out->pitch_rad, true, &increment)))
    {
        p_state->b_active_valid = false;
    }
    if (p_state->b_active_valid)
    {
        p_out->height_estimate_m += increment;
        p_out->hump_peak_m = fmax(p_out->hump_peak_m,
                                 p_out->height_estimate_m);
        if ((!isfinite(p_out->height_estimate_m))
            || (p_out->hump_peak_m > p_cfg->maximum_hump_m))
        {
            p_state->b_active_valid = false;
        }
    }
    p_out->b_height_valid = p_state->b_active_valid;
    if ((p_input->signed_distance_m > 0.0)
        && (p_out->pitch_rad < -p_cfg->slope_exit_rad))
    {
        p_state->b_descent_seen = true;
    }
    if (fabs(p_out->pitch_rad) <= p_cfg->slope_exit_rad)
    {
        p_state->flat_time_s += p_input->dt_s;
    }
    else
    {
        p_state->flat_time_s = 0.0;
    }
    if ((p_state->flat_time_s >= p_cfg->flat_confirm_s)
        && (p_state->b_descent_seen || (!p_state->b_active_valid)))
    {
        terrain_finish_hump(p_state);
    }
}

/** Commit bounded, closed humps and preserve the previous valid maximum. */
static void terrain_finish_hump (terrain_state_t * p_state)
{
    terrain_output_t * p_out = &p_state->output;
    const terrain_config_t * p_cfg = &p_state->config;

    p_out->b_hump_active     = false;
    p_out->b_hump_completed  = true;
    p_out->b_last_hump_valid = p_state->b_active_valid
        && (p_out->hump_peak_m >= p_cfg->minimum_hump_m)
        && (fabs(p_out->height_estimate_m) <= p_cfg->closure_tolerance_m);
    p_out->b_height_valid = p_out->b_last_hump_valid;
    if (p_out->b_last_hump_valid)
    {
        p_out->last_hump_peak_m = p_out->hump_peak_m;
        p_out->run_maximum_m = fmax(p_out->run_maximum_m,
                                   p_out->last_hump_peak_m);
        if (UINT8_MAX != p_out->completed_humps)
        {
            p_out->completed_humps++;
        }
    }
    p_state->flat_time_s = 0.0;
}

/** End of file. */


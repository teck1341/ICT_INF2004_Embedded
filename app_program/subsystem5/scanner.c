/** @file scanner.c
 * @brief Fixed-memory coarse/fine ultrasonic scan scheduling and profile.
 */
#include "scanner.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "bitpack.h"

#if DBL_MANT_DIG < 53
#error "Scanner geometry requires at least 53 double mantissa bits."
#endif

#define SCANNER_MIN_ANGLE_DEG (-60)
#define SCANNER_MAX_ANGLE_DEG (60)
#define SCANNER_COARSE_STEP_DEG (10)
#define SCANNER_FINE_STEP_DEG (2)
#define SCANNER_SIDE_ANGLE_DEG (20)
#define SCANNER_MAX_DELAY_MS (60000U)
#define SCANNER_MIN_INTERVAL_MS (65U)
#define SCANNER_US_PER_S (1000000.0)
#define SCANNER_MM_PER_M (1000.0)
#define SCANNER_CODE_MASK (0x7FFFU)
/* Constant masks let -Wconversion prove each bit-field store fits. */
#define SCANNER_MM_MASK ((UINT32_C(1) << SCANNER_SHARED_MM_BITS) - 1U)
#define SCANNER_COUNT_MASK ((UINT32_C(1) << SCANNER_SHARED_COUNT_BITS) - 1U)

typedef struct
{
    double minimum_y_m;
    double maximum_y_m;
    double left_distance_m;
    double right_distance_m;
    uint32_t left_samples;
    uint32_t right_samples;
    bool b_left_valid;
    bool b_right_valid;
} scanner_profile_t;

static bool scanner_config_valid (const scanner_config_t * p_config);
static bool scanner_saturation_exact (
    const core_ultrasonic_config_t * p_range);
static void scanner_result_reset (scanner_result_t * p_result);
static void scanner_table_clear (scanner_state_t * p_state);
static uint32_t scanner_index (int32_t angle_deg);
static bool scanner_decode (const scanner_state_t * p_state, uint32_t index,
                            scanner_sample_t * p_sample);
static void scanner_capture (scanner_state_t * p_state,
                             const scanner_echo_t * p_echo);
static bool scanner_advance (scanner_state_t * p_state);
static bool scanner_refine (scanner_state_t * p_state);
static void scanner_profile (scanner_state_t * p_state);
static void scanner_accumulate (scanner_state_t * p_state,
                                scanner_profile_t * p_profile,
                                const scanner_sample_t * p_sample);
static void scanner_choose_side (scanner_state_t * p_state,
                                 const scanner_profile_t * p_profile);
static uint32_t scanner_shared_mm (double distance_m, bool b_valid);

/** Default timing exceeds the HC-SR04 recommendation of 60 ms per cycle. */
void scanner_default_config (scanner_config_t * p_config)
{
    if (NULL != p_config)
    {
        p_config->range.sound_speed_m_per_s = 343.0;
        p_config->range.min_distance_m      = 0.02;
        p_config->range.max_distance_m      = 4.0;
        p_config->obstacle_distance_m       = 0.60;
        p_config->robot_half_width_m        = 0.08;
        p_config->uncertainty_margin_m      = 0.05;
        p_config->required_clearance_m      = 0.50;
        p_config->settle_ms                 = 150U;
        p_config->trigger_interval_ms       = SCANNER_MIN_INTERVAL_MS;
        p_config->echo_timeout_ms           = 30U;
        p_config->b_calibrated              = false;
    }
}

/** Configuration validation is independent of later calibration approval. */
bool scanner_init (scanner_state_t * p_state,
                   const scanner_config_t * p_config)
{
    scanner_config_t config_copy;

    if ((NULL == p_state) || (!scanner_config_valid(p_config)))
    {
        return false;
    }
    config_copy = *p_config;
    memset(p_state, 0, sizeof(*p_state));
    p_state->config        = config_copy;
    p_state->b_initialized = true;
    p_state->phase         = SCANNER_IDLE;
    scanner_table_clear(p_state);
    scanner_result_reset(&p_state->result);
    return true;
}

/** Acquire only after calibration and stationary-pose confirmation. */
bool scanner_start (scanner_state_t * p_state, uint32_t now_ms,
                    bool b_stationary)
{
    if ((NULL == p_state) || (!p_state->b_initialized)
        || (!p_state->config.b_calibrated) || (!b_stationary))
    {
        return false;
    }
    if ((SCANNER_POSITION == p_state->phase)
        || (SCANNER_SETTLE == p_state->phase)
        || (SCANNER_WAIT_ECHO == p_state->phase))
    {
        return false;
    }
    p_state->sample_count     = 0U;
    p_state->angle_deg        = SCANNER_MIN_ANGLE_DEG;
    p_state->fine_end_deg     = SCANNER_MAX_ANGLE_DEG;
    p_state->phase_started_ms = now_ms;
    p_state->b_fine           = false;
    p_state->phase            = SCANNER_POSITION;
    scanner_table_clear(p_state);
    scanner_result_reset(&p_state->result);
    return true;
}

/** Service one bounded transition; board pulse capture owns microseconds. */
bool scanner_step (scanner_state_t * p_state, uint32_t now_ms,
                   bool b_stationary, const scanner_echo_t * p_echo,
                   scanner_output_t * p_output)
{
    uint32_t elapsed_ms = 0U;

    if (NULL == p_output)
    {
        return false;
    }
    memset(p_output, 0, sizeof(*p_output));
    if ((NULL == p_state) || (NULL == p_echo)
        || (!p_state->b_initialized))
    {
        return false;
    }
    if ((SCANNER_IDLE == p_state->phase)
        || (SCANNER_COMPLETE == p_state->phase)
        || (SCANNER_ABORTED == p_state->phase))
    {
        return true;
    }
    if (!b_stationary)
    {
        scanner_result_reset(&p_state->result);
        p_state->result.b_aborted = true;
        p_state->phase            = SCANNER_ABORTED;
        p_output->b_set_servo     = true;
        p_output->b_done          = true;
        return true;
    }
    elapsed_ms = now_ms - p_state->phase_started_ms;
    switch (p_state->phase)
    {
        case SCANNER_IDLE:
        case SCANNER_COMPLETE:
        case SCANNER_ABORTED:
            /* Terminal phases returned above before stationary validation. */
        break;
        case SCANNER_POSITION:
            p_output->b_set_servo     = true;
            p_output->angle_deg       = p_state->angle_deg;
            p_state->phase_started_ms = now_ms;
            p_state->phase            = SCANNER_SETTLE;
        break;
        case SCANNER_SETTLE:
            if ((elapsed_ms >= p_state->config.settle_ms)
                && ((!p_state->b_triggered)
                    || ((now_ms - p_state->last_trigger_ms)
                        >= p_state->config.trigger_interval_ms)))
            {
                p_output->b_trigger      = true;
                p_state->last_trigger_ms = now_ms;
                p_state->b_triggered     = true;
                p_state->phase_started_ms = now_ms;
                p_state->phase            = SCANNER_WAIT_ECHO;
            }
        break;
        case SCANNER_WAIT_ECHO:
            if (p_echo->b_ready
                || (elapsed_ms >= p_state->config.echo_timeout_ms))
            {
                scanner_capture(p_state, p_echo);
                if (!scanner_advance(p_state))
                {
                    scanner_profile(p_state);
                    p_state->phase       = SCANNER_COMPLETE;
                    p_output->b_set_servo = true;
                    p_output->b_done      = true;
                }
            }
        break;
        default:
            scanner_result_reset(&p_state->result);
            p_state->phase            = SCANNER_ABORTED;
            p_state->result.b_aborted = true;
            p_output->b_set_servo     = true;
            p_output->b_done          = true;
            return false;
        break;
    }
    return true;
}

/** Odd, out-of-sector and never-sampled angles have no observation; nor
 * has a state that scanner_init never accepted (its table is not filled).
 */
bool scanner_sample (const scanner_state_t * p_state, int32_t angle_deg,
                     scanner_sample_t * p_sample)
{
    if ((NULL == p_state) || (NULL == p_sample)
        || (!p_state->b_initialized)
        || (angle_deg < SCANNER_MIN_ANGLE_DEG)
        || (angle_deg > SCANNER_MAX_ANGLE_DEG)
        || (0 != ((angle_deg - SCANNER_MIN_ANGLE_DEG)
                  % SCANNER_FINE_STEP_DEG)))
    {
        return false;
    }
    return scanner_decode(p_state, scanner_index(angle_deg), p_sample);
}

/** CEIL distances to whole mm and copy counts, side and flags. */
void scanner_shared_pack (const scanner_result_t * p_result,
                          scanner_shared_t * p_shared)
{
    if ((NULL == p_result) || (NULL == p_shared))
    {
        return;
    }
    p_shared->nearest_mm = scanner_shared_mm(p_result->nearest_distance_m,
                                             p_result->valid_samples > 0U)
                           & SCANNER_MM_MASK;
    p_shared->width_mm = scanner_shared_mm(p_result->apparent_width_m,
                                           p_result->b_obstacle_seen)
                         & SCANNER_MM_MASK;
    p_shared->preferred_side  = p_result->preferred_side;
    p_shared->valid_samples   = bitpack_saturate(p_result->valid_samples,
                                                 SCANNER_SHARED_COUNT_BITS)
                                & SCANNER_COUNT_MASK;
    p_shared->unknown_samples = bitpack_saturate(p_result->unknown_samples,
                                                 SCANNER_SHARED_COUNT_BITS)
                                & SCANNER_COUNT_MASK;
    p_shared->b_profile_valid  = p_result->b_profile_valid;
    p_shared->b_obstacle_seen  = p_result->b_obstacle_seen;
    p_shared->b_left_observed  = p_result->b_left_observed;
    p_shared->b_right_observed = p_result->b_right_observed;
    p_shared->b_aborted        = p_result->b_aborted;
}

/** Whole millimetres are exact in double; one division rounds to metres. */
void scanner_shared_unpack (const scanner_shared_t * p_shared,
                            scanner_result_t * p_result)
{
    if ((NULL == p_shared) || (NULL == p_result))
    {
        return;
    }
    scanner_result_reset(p_result);
    if (p_shared->valid_samples > 0U)
    {
        p_result->nearest_distance_m = ((double) p_shared->nearest_mm)
                                       / SCANNER_MM_PER_M;
    }
    if (p_shared->b_obstacle_seen)
    {
        p_result->apparent_width_m = ((double) p_shared->width_mm)
                                     / SCANNER_MM_PER_M;
    }
    p_result->preferred_side   = p_shared->preferred_side;
    p_result->valid_samples    = p_shared->valid_samples;
    p_result->unknown_samples  = p_shared->unknown_samples;
    p_result->b_profile_valid  = p_shared->b_profile_valid;
    p_result->b_obstacle_seen  = p_shared->b_obstacle_seen;
    p_result->b_left_observed  = p_shared->b_left_observed;
    p_result->b_right_observed = p_shared->b_right_observed;
    p_result->b_aborted        = p_shared->b_aborted;
}

/** Bound times below half the clock wrap and reject nonphysical geometry. */
static bool scanner_config_valid (const scanner_config_t * p_config)
{
    if (NULL == p_config)
    {
        return false;
    }
    return (isfinite(p_config->range.sound_speed_m_per_s))
           && (p_config->range.sound_speed_m_per_s > 0.0)
           && (isfinite(p_config->range.min_distance_m))
           && (p_config->range.min_distance_m >= 0.02)
           && (isfinite(p_config->range.max_distance_m))
           && (p_config->range.max_distance_m <= 4.0)
           && (p_config->range.max_distance_m
               > p_config->range.min_distance_m)
           && (isfinite(p_config->obstacle_distance_m))
           && (p_config->obstacle_distance_m
               >= p_config->range.min_distance_m)
           && (p_config->obstacle_distance_m < p_config->range.max_distance_m)
           && (isfinite(p_config->robot_half_width_m))
           && (p_config->robot_half_width_m > 0.0)
           && (isfinite(p_config->uncertainty_margin_m))
           && (p_config->uncertainty_margin_m >= 0.0)
           && (isfinite(p_config->required_clearance_m))
           && (p_config->required_clearance_m > 0.0)
           && (p_config->settle_ms > 0U)
           && (p_config->settle_ms <= SCANNER_MAX_DELAY_MS)
           && (p_config->trigger_interval_ms >= SCANNER_MIN_INTERVAL_MS)
           && (p_config->trigger_interval_ms <= SCANNER_MAX_DELAY_MS)
           && (p_config->echo_timeout_ms > 0U)
           && (p_config->echo_timeout_ms < p_config->trigger_interval_ms)
           && (scanner_saturation_exact(&p_config->range));
}

/**
 * Longer pulses are stored as SCANNER_ENTRY_PULSE_MAX_US. With that code
 * beyond the maximum range, computed exactly as core_ultrasonic_range does,
 * every longer pulse is also beyond it (monotone arithmetic), so the stored
 * code and the original pulse are both non-VALID.
 */
static bool scanner_saturation_exact (
    const core_ultrasonic_config_t * p_range)
{
    const double pulse_s = ((double) SCANNER_ENTRY_PULSE_MAX_US)
                           / SCANNER_US_PER_S;

    return ((pulse_s / 2.0) * p_range->sound_speed_m_per_s)
           > p_range->max_distance_m;
}

/** Unknown distances use NaN; no previous profile survives a new request. */
static void scanner_result_reset (scanner_result_t * p_result)
{
    memset(p_result, 0, sizeof(*p_result));
    p_result->nearest_distance_m = NAN;
    p_result->apparent_width_m   = NAN;
    p_result->left_clearance_m   = NAN;
    p_result->right_clearance_m  = NAN;
}

/** No observation from an earlier scan may survive into a new one. */
static void scanner_table_clear (scanner_state_t * p_state)
{
    uint32_t index = 0U;

    for (index = 0U; index < SCANNER_TABLE_SIZE; index++)
    {
        /* 0x7FFF fits the 16-bit entry exactly. */
        p_state->table[index] = (scanner_entry_t) SCANNER_ENTRY_NOT_SAMPLED;
    }
}

/** Table index (angle + 60) / 2 of an even beam angle in -60..60. */
static uint32_t scanner_index (int32_t angle_deg)
{
    /* -60..60 gives the exact offset 0..120; an angle outside the sector
     * converts modulo 2^32 (C99 6.3.1.3) to an index >= 61 (or 60 for 61).
     * The step constant 2 is exact as uint32_t. */
    return ((uint32_t) (angle_deg - SCANNER_MIN_ANGLE_DEG))
           / (uint32_t) SCANNER_FINE_STEP_DEG;
}

/** Rebuild the list sample; the pulse code converts exactly as it did. */
static bool scanner_decode (const scanner_state_t * p_state, uint32_t index,
                            scanner_sample_t * p_sample)
{
    const uint32_t entry = p_state->table[index];
    const uint32_t code  = entry & SCANNER_CODE_MASK;
    core_status_t status = CORE_UNAVAILABLE;

    if (SCANNER_ENTRY_NOT_SAMPLED == code)
    {
        return false;
    }
    /* index is 0..60, so the beam angle -60..60 is exact. */
    p_sample->angle_deg = (((int32_t) index) * SCANNER_FINE_STEP_DEG)
                          + SCANNER_MIN_ANGLE_DEG;
    p_sample->b_fine = (0U != (entry & SCANNER_ENTRY_FINE));
    /* Code 0 is no echo; a zero-width echo also failed as UNKNOWN. */
    status = core_ultrasonic_range(&p_state->config.range,
                                   SCANNER_ENTRY_NO_ECHO != code,
                                   ((double) code) / SCANNER_US_PER_S,
                                   &p_sample->range);
    if ((CORE_OK != status) && (CORE_OUT_OF_RANGE != status))
    {
        p_sample->range.state      = CORE_RANGE_UNKNOWN;
        p_sample->range.distance_m = NAN;
    }
    return true;
}

/** Store the current pulse or timeout; the newest observation wins. */
static void scanner_capture (scanner_state_t * p_state,
                             const scanner_echo_t * p_echo)
{
    const uint32_t index = scanner_index(p_state->angle_deg);
    uint32_t code = SCANNER_ENTRY_NO_ECHO;

    /* The beam angle is always in -60..60; the index bound is defensive. */
    if ((p_state->sample_count >= SCANNER_MAX_SAMPLES)
        || (index >= SCANNER_TABLE_SIZE))
    {
        return;
    }
    if (p_echo->b_ready)
    {
        /* scanner_init proved the saturated code is beyond max range. */
        code = (p_echo->pulse_us > SCANNER_ENTRY_PULSE_MAX_US)
               ? SCANNER_ENTRY_PULSE_MAX_US : p_echo->pulse_us;
    }
    if (p_state->b_fine)
    {
        code |= SCANNER_ENTRY_FINE;
    }
    /* A 15-bit code plus the fine flag always fits the 16-bit entry. */
    p_state->table[index] = (scanner_entry_t) code;
    p_state->sample_count++;
}

/** Continue bounded sweeps; at most 13 coarse plus 61 fine samples fit. */
static bool scanner_advance (scanner_state_t * p_state)
{
    const int32_t step_deg = p_state->b_fine ? SCANNER_FINE_STEP_DEG
                                            : SCANNER_COARSE_STEP_DEG;
    const int32_t end_deg  = p_state->b_fine ? p_state->fine_end_deg
                                            : SCANNER_MAX_ANGLE_DEG;

    if ((p_state->angle_deg < end_deg)
        && (p_state->sample_count < SCANNER_MAX_SAMPLES))
    {
        /* Below an end of at most 60 by a whole step: the sum is <= 60. */
        p_state->angle_deg = (int8_t) (p_state->angle_deg + step_deg);
        p_state->phase = SCANNER_POSITION;
        return true;
    }
    if ((!p_state->b_fine) && (scanner_refine(p_state)))
    {
        p_state->phase = SCANNER_POSITION;
        return true;
    }
    return false;
}

/** Refine all uncertain/near coarse beams and their immediate neighbours. */
static bool scanner_refine (scanner_state_t * p_state)
{
    scanner_sample_t sample;
    int32_t first_deg = SCANNER_MAX_ANGLE_DEG;
    int32_t last_deg  = SCANNER_MIN_ANGLE_DEG;
    uint32_t index   = 0U;
    bool b_found     = false;

    /* Only this scan's coarse beams are sampled here; ascending angle order
     * is their capture order. */
    for (index = 0U; index < SCANNER_TABLE_SIZE; index++)
    {
        if (scanner_decode(p_state, index, &sample)
            && ((CORE_RANGE_VALID != sample.range.state)
                || (sample.range.distance_m
                    <= p_state->config.obstacle_distance_m)))
        {
            if (!b_found)
            {
                first_deg = sample.angle_deg;
            }
            last_deg = sample.angle_deg;
            b_found  = true;
        }
    }
    if (b_found)
    {
        first_deg -= SCANNER_COARSE_STEP_DEG;
        last_deg  += SCANNER_COARSE_STEP_DEG;
        /* Both are clamped into -60..60, which int8_t holds exactly. */
        p_state->angle_deg = (int8_t) ((first_deg < SCANNER_MIN_ANGLE_DEG)
                             ? SCANNER_MIN_ANGLE_DEG : first_deg);
        p_state->fine_end_deg = (int8_t) ((last_deg > SCANNER_MAX_ANGLE_DEG)
                                ? SCANNER_MAX_ANGLE_DEG : last_deg);
        p_state->b_fine = true;
    }
    return b_found;
}

/** Assemble visible endpoint span and explicitly qualified sector margins. */
static void scanner_profile (scanner_state_t * p_state)
{
    scanner_profile_t profile;
    scanner_sample_t sample;
    uint32_t index = 0U;

    memset(&profile, 0, sizeof(profile));
    profile.minimum_y_m      = DBL_MAX;
    profile.maximum_y_m      = -DBL_MAX;
    profile.left_distance_m  = DBL_MAX;
    profile.right_distance_m = DBL_MAX;
    profile.b_left_valid     = true;
    profile.b_right_valid    = true;
    scanner_result_reset(&p_state->result);
    /* Each sampled entry is the newest observation at its angle. */
    for (index = 0U; index < SCANNER_TABLE_SIZE; index++)
    {
        if (scanner_decode(p_state, index, &sample))
        {
            scanner_accumulate(p_state, &profile, &sample);
        }
    }
    p_state->result.b_profile_valid = (p_state->result.valid_samples > 0U)
        && (0U == p_state->result.unknown_samples);
    if (p_state->result.b_obstacle_seen)
    {
        p_state->result.apparent_width_m = profile.maximum_y_m
                                          - profile.minimum_y_m;
    }
    scanner_choose_side(p_state, &profile);
}

/** Count all unknowns, retaining no-clear semantics even for maximum range. */
static void scanner_accumulate (scanner_state_t * p_state,
                                scanner_profile_t * p_profile,
                                const scanner_sample_t * p_sample)
{
    scanner_result_t * p_result = &p_state->result;
    const bool b_valid = (CORE_RANGE_VALID == p_sample->range.state);
    const bool b_left  = (p_sample->angle_deg >= SCANNER_SIDE_ANGLE_DEG);
    const bool b_right = (p_sample->angle_deg <= -SCANNER_SIDE_ANGLE_DEG);
    double lateral_m = 0.0;
    double angle_rad = 0.0;

    if (b_left)
    {
        p_profile->left_samples++;
        p_profile->b_left_valid = p_profile->b_left_valid && b_valid;
    }
    if (b_right)
    {
        p_profile->right_samples++;
        p_profile->b_right_valid = p_profile->b_right_valid && b_valid;
    }
    if (!b_valid)
    {
        p_result->unknown_samples++;
        return;
    }
    p_result->valid_samples++;
    if ((!isfinite(p_result->nearest_distance_m))
        || (p_sample->range.distance_m < p_result->nearest_distance_m))
    {
        p_result->nearest_distance_m = p_sample->range.distance_m;
    }
    if (b_left)
    {
        p_profile->left_distance_m = fmin(p_profile->left_distance_m,
                                         p_sample->range.distance_m);
    }
    if (b_right)
    {
        p_profile->right_distance_m = fmin(p_profile->right_distance_m,
                                          p_sample->range.distance_m);
    }
    if (p_sample->range.distance_m <= p_state->config.obstacle_distance_m)
    {
        /* The bounded integral degree angle is represented exactly. */
        angle_rad = ((double) p_sample->angle_deg) * CORE_PI / 180.0;
        lateral_m = p_sample->range.distance_m * sin(angle_rad);
        p_profile->minimum_y_m = fmin(p_profile->minimum_y_m, lateral_m);
        p_profile->maximum_y_m = fmax(p_profile->maximum_y_m, lateral_m);
        p_result->b_obstacle_seen = true;
    }
}

/** Prefer a side only with complete observations and sufficient margin. */
static void scanner_choose_side (scanner_state_t * p_state,
                                 const scanner_profile_t * p_profile)
{
    scanner_result_t * p_result = &p_state->result;
    const double allowance_m = p_state->config.robot_half_width_m
                                + p_state->config.uncertainty_margin_m;
    bool b_left_clear  = false;
    bool b_right_clear = false;

    p_result->b_left_observed = p_profile->b_left_valid
                               && (p_profile->left_samples > 0U);
    p_result->b_right_observed = p_profile->b_right_valid
                                && (p_profile->right_samples > 0U);
    if (p_result->b_left_observed)
    {
        p_result->left_clearance_m = fmax(0.0,
                                     p_profile->left_distance_m - allowance_m);
        b_left_clear = p_result->left_clearance_m
                       >= p_state->config.required_clearance_m;
    }
    if (p_result->b_right_observed)
    {
        p_result->right_clearance_m = fmax(0.0,
                                    p_profile->right_distance_m - allowance_m);
        b_right_clear = p_result->right_clearance_m
                        >= p_state->config.required_clearance_m;
    }
    if (!p_result->b_profile_valid)
    {
        p_result->preferred_side = SCANNER_SIDE_NONE;
    }
    else if (b_left_clear && ((!b_right_clear)
             || (p_result->left_clearance_m >= p_result->right_clearance_m)))
    {
        p_result->preferred_side = SCANNER_SIDE_LEFT;
    }
    else if (b_right_clear)
    {
        p_result->preferred_side = SCANNER_SIDE_RIGHT;
    }
    else
    {
        p_result->preferred_side = SCANNER_SIDE_NONE;
    }
}

/** CEIL to whole mm in 0..4095; 0 when not valid. A non-finite distance
 * flagged valid stores the largest code, beyond every consumer gate. */
static uint32_t scanner_shared_mm (double distance_m, bool b_valid)
{
    int32_t distance_mm = 0;

    if (!b_valid)
    {
        return 0U;
    }
    /* The 4095 mm limit is exact in int32_t. */
    if (!bitpack_quantise(distance_m, SCANNER_MM_PER_M, BITPACK_CEIL, 0,
                          (int32_t) SCANNER_MM_MASK, &distance_mm))
    {
        return SCANNER_MM_MASK;
    }
    /* Clamped to 0..4095 above, so the conversion is exact. */
    return (uint32_t) distance_mm;
}

/** End of file. */


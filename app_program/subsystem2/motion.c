/** @file motion.c
 * @brief Leased differential-drive motion with sticky fault inhibition.
 * Early validation returns use BARR-C 6.2.c's readability allowance.
 */
#include "motion.h"

#include "bitpack.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Millisecond time limit (primitive timeout), not a command ID bound. */
#define MOTION_SERIAL_HALF (UINT32_C(2147483648))
#define MOTION_ID_MASK ((UINT32_C(1) << MOTION_ID_BITS) - 1U)
#define MOTION_ID_HALF (UINT32_C(1) << (MOTION_ID_BITS - 1U))
#define MOTION_COMPARE_EPS (1.0e-12)
#define MOTION_SPEED_BUCKET_S (0.005)
#define MOTION_SPEED_WINDOW_S (0.050)
#define MOTION_TURN_LIMIT_RAD (2.0 * CORE_PI)
/* Largest whole milliradian not above 2*pi (6283.185...). */
#define MOTION_TURN_MAX_MRAD (6283)
#define MOTION_MILLI (1000.0)
#define MOTION_FORWARD_MASK ((UINT32_C(1) << MOTION_FORWARD_BITS) - 1U)
#define MOTION_YAW_MASK ((UINT32_C(1) << MOTION_YAW_BITS) - 1U)
#define MOTION_TARGET_MASK ((UINT32_C(1) << MOTION_TARGET_BITS) - 1U)
#define MOTION_SPEED_MASK ((UINT32_C(1) << MOTION_SPEED_BITS) - 1U)

static bool motion_config_valid (const motion_config_t * p_config);
static bool motion_request_valid (const motion_t * p_motion,
                                  const motion_request_t * p_request);
static bool motion_same_primitive (const motion_request_t * p_first,
                                   const motion_request_t * p_second);
static void motion_zero (motion_t * p_motion);
static bool motion_direction_changed (const motion_t * p_motion,
                                      const motion_request_t * p_request);
static bool motion_speed_update (motion_t * p_motion, core_real_t dt_s,
                                 core_real_t left_speed,
                                 core_real_t right_speed);
static void motion_finish (motion_t * p_motion);
static motion_status_t motion_feedback (motion_t * p_motion,
                                        core_real_t dt_s,
                                        int32_t left_delta,
                                        int32_t right_delta);
static motion_status_t motion_drive (motion_t * p_motion, core_real_t dt_s);
static core_real_t motion_sign (core_real_t value);
static int32_t motion_quantise (core_real_t value, bitpack_round_t rule,
                                uint32_t width);
static uint32_t motion_encode (core_real_t value, bitpack_round_t rule,
                               uint32_t width);
static core_real_t motion_decode (uint32_t field, uint32_t width);
static uint32_t motion_target_field (const motion_request_t * p_request);
static bool motion_kind_defined (motion_kind_t kind);

motion_status_t motion_init (motion_t * p_motion,
                             const motion_config_t * p_config)
{
    if (NULL == p_motion)
    {
        return MOTION_INVALID;
    }
    memset(p_motion, 0, sizeof(*p_motion));
    p_motion->output.state = MOTION_INHIBITED;
    p_motion->b_stop_latched = true;
    if (!motion_config_valid(p_config))
    {
        p_motion->output.reason = MOTION_INVALID;
        return MOTION_INVALID;
    }
    p_motion->config = *p_config;
    p_motion->b_initialized = true;
    p_motion->output.b_stationary = false;
    if (!p_config->b_calibration_confirmed)
    {
        p_motion->output.reason = MOTION_REJECTED;
        return MOTION_REJECTED;
    }
    p_motion->b_stop_latched = false;
    p_motion->output.state = MOTION_IDLE;
    return MOTION_OK;
}

motion_status_t motion_submit (motion_t * p_motion,
                               const motion_request_t * p_request,
                               uint32_t now_ms)
{
    uint32_t serial_delta = 0U;

    if ((NULL == p_motion) || (NULL == p_request) ||
        (!p_motion->b_initialized))
    {
        return MOTION_INVALID;
    }
    if (MOTION_HALT == p_request->kind)
    {
        motion_zero(p_motion);
        if (!p_motion->b_stop_latched)
        {
            p_motion->output.state = MOTION_IDLE;
            p_motion->output.reason = MOTION_OK;
        }
        return MOTION_OK;
    }
    if ((p_motion->b_stop_latched) ||
        (!p_motion->config.b_calibration_confirmed))
    {
        return MOTION_REJECTED;
    }
    if ((MOTION_RUNNING == p_motion->output.state) &&
        ((now_ms - p_motion->renewed_ms) >= MOTION_LEASE_MS))
    {
        motion_stop(p_motion, MOTION_EXPIRED);
        return MOTION_EXPIRED;
    }
    if (!motion_request_valid(p_motion, p_request))
    {
        return MOTION_INVALID;
    }
    /* uint8_t IDs promote to int: subtract as uint32_t and keep the low
     * MOTION_ID_BITS bits, the serial distance modulo 2^8.
     */
    serial_delta = ((uint32_t) p_request->command_id -
                    (uint32_t) p_motion->request.command_id) & MOTION_ID_MASK;
    if (p_motion->b_has_command && (0U == serial_delta))
    {
        if (!motion_same_primitive(&p_motion->request, p_request))
        {
            return MOTION_REJECTED;
        }
        if (MOTION_COMPLETE == p_motion->output.state)
        {
            /* The supervisor may publish a renewal from a RUNNING snapshot
             * after this owner completes. Acknowledge that race without
             * restarting the primitive or changing its completion record.
             */
            return MOTION_OK;
        }
        if (MOTION_RUNNING != p_motion->output.state)
        {
            return MOTION_REJECTED;
        }
        if ((MOTION_VELOCITY == p_request->kind) &&
            motion_direction_changed(p_motion, p_request))
        {
            motion_zero(p_motion);
        }
        p_motion->request = *p_request;
        p_motion->renewed_ms = now_ms;
        return MOTION_OK;
    }
    if (p_motion->b_has_command && (serial_delta >= MOTION_ID_HALF))
    {
        return MOTION_REJECTED;
    }
    motion_zero(p_motion);
    p_motion->request = *p_request;
    p_motion->b_has_command = true;
    p_motion->progress = 0.0;
    p_motion->b_start_pending = true;
    p_motion->started_ms = now_ms;
    p_motion->renewed_ms = now_ms;
    p_motion->output.command_id = p_request->command_id;
    p_motion->output.state = MOTION_RUNNING;
    p_motion->output.reason = MOTION_OK;
    return MOTION_OK;
}

void motion_stop (motion_t * p_motion, motion_status_t reason)
{
    if (NULL != p_motion)
    {
        motion_zero(p_motion);
        p_motion->b_stop_latched = true;
        p_motion->output.state = MOTION_INHIBITED;
        p_motion->output.reason = reason;
    }
}

motion_status_t motion_reset (motion_t * p_motion, bool b_stationary,
                              bool b_fault_clear)
{
    if ((NULL == p_motion) || (!p_motion->b_initialized))
    {
        return MOTION_INVALID;
    }
    if ((!b_stationary) || (!p_motion->output.b_stationary) ||
        (!b_fault_clear) || (!p_motion->config.b_calibration_confirmed))
    {
        return MOTION_REJECTED;
    }
    motion_zero(p_motion);
    p_motion->b_stop_latched = false;
    p_motion->output.state = MOTION_IDLE;
    p_motion->output.reason = MOTION_OK;
    return MOTION_OK;
}

motion_status_t motion_step (motion_t * p_motion, uint32_t now_ms,
                             core_real_t dt_s, int32_t left_delta,
                             int32_t right_delta, motion_output_t * p_output)
{
    motion_status_t status = MOTION_OK;

    if ((NULL == p_motion) || (NULL == p_output))
    {
        return MOTION_INVALID;
    }
    p_motion->output.signed_distance_m = 0.0;
    if (!p_motion->b_initialized)
    {
        motion_stop(p_motion, MOTION_INVALID);
        *p_output = p_motion->output;
        return MOTION_INVALID;
    }
    status = motion_feedback(p_motion, dt_s, left_delta, right_delta);
    if (MOTION_OK != status)
    {
        motion_stop(p_motion, status);
    }
    if ((!p_motion->b_stop_latched) &&
        (MOTION_RUNNING == p_motion->output.state))
    {
        if ((now_ms - p_motion->renewed_ms) >= MOTION_LEASE_MS)
        {
            motion_stop(p_motion, MOTION_EXPIRED);
        }
        else if ((MOTION_VELOCITY != p_motion->request.kind) &&
                 ((now_ms - p_motion->started_ms) >=
                  p_motion->config.primitive_timeout_ms))
        {
            motion_stop(p_motion, MOTION_TIMED_OUT);
        }
        else
        {
            status = motion_drive(p_motion, dt_s);
            if (MOTION_OK != status)
            {
                motion_stop(p_motion, status);
            }
        }
    }
    *p_output = p_motion->output;
    return p_motion->output.reason;
}

/** @brief Magnitudes round up, so a packed request is never accepted where
 * the original would be rejected (see motion_request_packed_t).
 */
void motion_request_pack (const motion_request_t * p_request,
                          motion_request_packed_t * p_packed)
{
    if ((NULL == p_request) || (NULL == p_packed))
    {
        return;
    }
    p_packed->forward_mm_s = motion_encode(p_request->forward_mps,
        BITPACK_AWAY, MOTION_FORWARD_BITS) & MOTION_FORWARD_MASK;
    p_packed->yaw_mrad_s = motion_encode(p_request->yaw_rps, BITPACK_AWAY,
                                         MOTION_YAW_BITS) & MOTION_YAW_MASK;
    p_packed->command_id = p_request->command_id;
    if (motion_kind_defined(p_request->kind))
    {
        p_packed->kind = p_request->kind;
        p_packed->target =
            motion_target_field(p_request) & MOTION_TARGET_MASK;
    }
    else
    {
        /* Two bits would alias an undefined kind onto a real one. A
         * VELOCITY with the reserved target is INVALID, as the original.
         */
        p_packed->kind = MOTION_VELOCITY;
        p_packed->target =
            bitpack_invalid(MOTION_TARGET_BITS) & MOTION_TARGET_MASK;
    }
}

/** @brief Field codes back to SI units; the same codes give equal values,
 * so a renewal unpacks to an identical primitive.
 */
void motion_request_unpack (const motion_request_packed_t * p_packed,
                            motion_request_t * p_request)
{
    if ((NULL == p_packed) || (NULL == p_request))
    {
        return;
    }
    p_request->command_id = p_packed->command_id;
    p_request->kind = p_packed->kind;
    p_request->forward_mps = motion_decode(p_packed->forward_mm_s,
                                           MOTION_FORWARD_BITS);
    p_request->yaw_rps = motion_decode(p_packed->yaw_mrad_s,
                                       MOTION_YAW_BITS);
    p_request->target = motion_decode(p_packed->target, MOTION_TARGET_BITS);
}

/** @brief Wheel speeds round to the nearest mm/s; enums and flags copy. */
void motion_snapshot_pack (const motion_output_t * p_output,
                           motion_snapshot_t * p_snapshot)
{
    if ((NULL == p_output) || (NULL == p_snapshot))
    {
        return;
    }
    core_pose_pack(&p_output->pose, &p_snapshot->pose);
    p_snapshot->left_mm_s = motion_encode(p_output->left_speed_mps,
        BITPACK_NEAREST, MOTION_SPEED_BITS) & MOTION_SPEED_MASK;
    p_snapshot->right_mm_s = motion_encode(p_output->right_speed_mps,
        BITPACK_NEAREST, MOTION_SPEED_BITS) & MOTION_SPEED_MASK;
    p_snapshot->command_id = p_output->command_id;
    p_snapshot->state = p_output->state;
    p_snapshot->reason = p_output->reason;
    p_snapshot->b_stationary = p_output->b_stationary;
}

/** @brief Shared fields are restored; motion-task fields read as zero. */
void motion_snapshot_unpack (const motion_snapshot_t * p_snapshot,
                             motion_output_t * p_output)
{
    if ((NULL == p_snapshot) || (NULL == p_output))
    {
        return;
    }
    core_pose_unpack(&p_snapshot->pose, &p_output->pose);
    p_output->state = p_snapshot->state;
    p_output->reason = p_snapshot->reason;
    p_output->command_id = p_snapshot->command_id;
    p_output->left_duty = 0.0;
    p_output->right_duty = 0.0;
    p_output->left_speed_mps = motion_decode(p_snapshot->left_mm_s,
                                             MOTION_SPEED_BITS);
    p_output->right_speed_mps = motion_decode(p_snapshot->right_mm_s,
                                              MOTION_SPEED_BITS);
    p_output->signed_distance_m = 0.0;
    p_output->b_stationary = p_snapshot->b_stationary;
}

/** @brief Check finite, bounded configuration before it can affect outputs. */
static bool motion_config_valid (const motion_config_t * p_config)
{
    core_pose_t pose = {0.0, 0.0, 0.0, 0.0};
    core_odometry_result_t result;
    core_pid_state_t pid;

    if (NULL == p_config)
    {
        return false;
    }
    if ((CORE_OK != core_odometry_step(&p_config->odometry, &pose,
                                      0, 0, &result)) ||
        (CORE_OK != core_pid_reset(&p_config->wheel_pid, &pid)))
    {
        return false;
    }
    return (isfinite(p_config->max_wheel_mps)) &&
           (p_config->max_wheel_mps > 0.0) &&
           (isfinite(p_config->max_yaw_rps)) &&
           (p_config->max_yaw_rps > 0.0) &&
           (isfinite(p_config->distance_tolerance_m)) &&
           (p_config->distance_tolerance_m > 0.0) &&
           (isfinite(p_config->turn_tolerance_rad)) &&
           (p_config->turn_tolerance_rad > 0.0) &&
           (p_config->turn_tolerance_rad < CORE_PI) &&
           (isfinite(p_config->stationary_mps)) &&
           (p_config->stationary_mps > 0.0) &&
           (p_config->wheel_pid.output_min >= -1.0) &&
           (p_config->wheel_pid.output_max <= 1.0) &&
           (p_config->primitive_timeout_ms > MOTION_LEASE_MS) &&
           (p_config->primitive_timeout_ms < MOTION_SERIAL_HALF);
}

/** @brief Validate command shape and the resulting wheel speed envelope. */
static bool motion_request_valid (const motion_t * p_motion,
                                  const motion_request_t * p_request)
{
    core_real_t wheel_limit = 0.0;
    const motion_config_t * p_config = &p_motion->config;

    if ((0U == p_request->command_id) ||
        (!isfinite(p_request->forward_mps)) ||
        (!isfinite(p_request->yaw_rps)) || (!isfinite(p_request->target)))
    {
        return false;
    }
    wheel_limit = fabs(p_request->forward_mps) +
                  (0.5 * p_config->odometry.track_width_m *
                   fabs(p_request->yaw_rps));
    if ((!isfinite(wheel_limit)) ||
        (wheel_limit > p_config->max_wheel_mps) ||
        (fabs(p_request->yaw_rps) > p_config->max_yaw_rps))
    {
        return false;
    }
    switch (p_request->kind)
    {
        case MOTION_VELOCITY:
            return true;
        case MOTION_DISTANCE:
            return (p_request->forward_mps > 0.0) &&
                   (fabs(p_request->yaw_rps) < MOTION_COMPARE_EPS) &&
                   (fabs(p_request->target) >
                    p_config->distance_tolerance_m);
        case MOTION_TURN:
            return (p_request->yaw_rps > 0.0) &&
                   (fabs(p_request->forward_mps) < MOTION_COMPARE_EPS) &&
                   (fabs(p_request->target) >
                    p_config->turn_tolerance_rad) &&
                   (fabs(p_request->target) <= MOTION_TURN_LIMIT_RAD);
        case MOTION_HALT:
            return false;
        default:
            return false;
    }
}

/** @brief A renewal cannot change the geometry or speed of a primitive. */
static bool motion_same_primitive (const motion_request_t * p_first,
                                   const motion_request_t * p_second)
{
    if (p_first->kind != p_second->kind)
    {
        return false;
    }
    if (MOTION_VELOCITY == p_first->kind)
    {
        return true;
    }
    return (fabs(p_first->target - p_second->target) < MOTION_COMPARE_EPS) &&
           (fabs(p_first->forward_mps - p_second->forward_mps) <
            MOTION_COMPARE_EPS) &&
           (fabs(p_first->yaw_rps - p_second->yaw_rps) < MOTION_COMPARE_EPS);
}

/** @brief Clear all drive/PID memory without clearing ID replay protection. */
static void motion_zero (motion_t * p_motion)
{
    p_motion->output.left_duty = 0.0;
    p_motion->output.right_duty = 0.0;
    memset(&p_motion->left_pid, 0, sizeof(p_motion->left_pid));
    memset(&p_motion->right_pid, 0, sizeof(p_motion->right_pid));
    memset(&p_motion->speed_window, 0, sizeof(p_motion->speed_window));
    /* Clearing history is not evidence of rest: preserve b_stationary. */
    p_motion->output.left_speed_mps = 0.0;
    p_motion->output.right_speed_mps = 0.0;
}

/** @brief A same-ID velocity reversal (including zero) starts fresh loops. */
static bool motion_direction_changed (const motion_t * p_motion,
                                      const motion_request_t * p_request)
{
    core_real_t half_track = 0.5 * p_motion->config.odometry.track_width_m;
    core_real_t old_left = p_motion->request.forward_mps -
                           (half_track * p_motion->request.yaw_rps);
    core_real_t old_right = p_motion->request.forward_mps +
                            (half_track * p_motion->request.yaw_rps);
    core_real_t new_left = p_request->forward_mps -
                           (half_track * p_request->yaw_rps);
    core_real_t new_right = p_request->forward_mps +
                            (half_track * p_request->yaw_rps);

    return ((old_left < 0.0) != (new_left < 0.0)) ||
           ((old_left > 0.0) != (new_left > 0.0)) ||
           ((old_right < 0.0) != (new_right < 0.0)) ||
           ((old_right > 0.0) != (new_right > 0.0));
}

/** @brief Average fixed time buckets, with bounded work even for tiny dt.
 * Eleven slots retain ten complete 5 ms bins plus a partial current bin.
 * This avoids a sample-count ring shrinking its window under short intervals.
 * A long accepted interval replaces the window at that interval's mean rate.
 */
static bool motion_speed_update (motion_t * p_motion, core_real_t dt_s,
                                 core_real_t left_speed,
                                 core_real_t right_speed)
{
    motion_speed_window_t * p_window = &p_motion->speed_window;
    motion_speed_bucket_t * p_bucket = NULL;
    core_real_t remaining = dt_s;
    core_real_t part = 0.0;
    core_real_t elapsed = 0.0;
    core_real_t left_distance = 0.0;
    core_real_t right_distance = 0.0;
    uint32_t bucket = 0U;

    if (remaining >= MOTION_SPEED_WINDOW_S)
    {
        memset(p_window, 0, sizeof(*p_window));
        remaining = MOTION_SPEED_WINDOW_S;
    }
    for (bucket = 0U; (bucket < MOTION_SPEED_BUCKETS) &&
                      (remaining > 0.0); bucket += 1U)
    {
        p_bucket = &p_window->buckets[p_window->current];
        part = fmin(remaining, MOTION_SPEED_BUCKET_S - p_bucket->elapsed_s);
        p_bucket->left_distance_m += left_speed * part;
        p_bucket->right_distance_m += right_speed * part;
        p_bucket->elapsed_s += part;
        remaining -= part;
        if (p_bucket->elapsed_s >=
            (MOTION_SPEED_BUCKET_S - MOTION_COMPARE_EPS))
        {
            p_window->current = (p_window->current + 1U) %
                                MOTION_SPEED_BUCKETS;
            memset(&p_window->buckets[p_window->current], 0,
                   sizeof(p_window->buckets[p_window->current]));
        }
    }
    for (bucket = 0U; bucket < MOTION_SPEED_BUCKETS; bucket += 1U)
    {
        elapsed += p_window->buckets[bucket].elapsed_s;
        left_distance += p_window->buckets[bucket].left_distance_m;
        right_distance += p_window->buckets[bucket].right_distance_m;
    }
    if ((!isfinite(elapsed)) || (elapsed <= 0.0) ||
        (remaining > MOTION_COMPARE_EPS))
    {
        return false;
    }
    p_motion->output.left_speed_mps = left_distance / elapsed;
    p_motion->output.right_speed_mps = right_distance / elapsed;
    return isfinite(p_motion->output.left_speed_mps) &&
           isfinite(p_motion->output.right_speed_mps);
}

/** @brief Emit persistent completion until the next accepted fresh ID. */
static void motion_finish (motion_t * p_motion)
{
    motion_zero(p_motion);
    p_motion->output.state = MOTION_COMPLETE;
    p_motion->output.reason = MOTION_OK;
}

/** @brief Consume signed encoder deltas even while drive is inhibited. */
static motion_status_t motion_feedback (motion_t * p_motion,
                                        core_real_t dt_s,
                                        int32_t left_delta,
                                        int32_t right_delta)
{
    core_odometry_result_t result;
    core_real_t left_speed = 0.0;
    core_real_t right_speed = 0.0;

    if ((!isfinite(dt_s)) || (dt_s <= 0.0) ||
        (dt_s > p_motion->config.wheel_pid.max_dt_s))
    {
        p_motion->output.b_stationary = false;
        return MOTION_SENSOR_FAULT;
    }
    if (CORE_OK != core_odometry_step(&p_motion->config.odometry,
                                      &p_motion->output.pose, left_delta,
                                      right_delta, &result))
    {
        p_motion->output.b_stationary = false;
        return MOTION_SENSOR_FAULT;
    }
    left_speed = result.left_distance_m / dt_s;
    right_speed = result.right_distance_m / dt_s;
    if ((!isfinite(left_speed)) || (!isfinite(right_speed)))
    {
        p_motion->output.b_stationary = false;
        return MOTION_SENSOR_FAULT;
    }
    if (!motion_speed_update(p_motion, dt_s, left_speed, right_speed))
    {
        p_motion->output.b_stationary = false;
        return MOTION_SENSOR_FAULT;
    }
    p_motion->output.pose = result.pose;
    p_motion->output.signed_distance_m = result.signed_centre_distance_m;
    p_motion->output.b_stationary =
        (fabs(left_speed) < p_motion->config.stationary_mps) &&
        (fabs(right_speed) < p_motion->config.stationary_mps);
    if (MOTION_RUNNING == p_motion->output.state)
    {
        if (p_motion->b_start_pending)
        {
            p_motion->b_start_pending = false;
        }
        else if (MOTION_DISTANCE == p_motion->request.kind)
        {
            p_motion->progress += result.signed_centre_distance_m;
        }
        else if (MOTION_TURN == p_motion->request.kind)
        {
            p_motion->progress += result.delta_yaw_rad;
        }
        else
        {
            /* Velocity commands have no completion position. */
        }
    }
    if (!isfinite(p_motion->progress))
    {
        return MOTION_SENSOR_FAULT;
    }
    return MOTION_OK;
}

/** @brief Compute bounded wheel duties, completing position primitives first.
 */
static motion_status_t motion_drive (motion_t * p_motion, core_real_t dt_s)
{
    core_real_t forward = p_motion->request.forward_mps;
    core_real_t angular = p_motion->request.yaw_rps;
    core_real_t tolerance = 0.0;
    core_real_t left_target = 0.0;
    core_real_t right_target = 0.0;
    core_real_t left_duty = 0.0;
    core_real_t right_duty = 0.0;
    core_real_t direction = motion_sign(p_motion->request.target);

    if (MOTION_VELOCITY != p_motion->request.kind)
    {
        if (MOTION_DISTANCE == p_motion->request.kind)
        {
            tolerance = p_motion->config.distance_tolerance_m;
            forward *= direction;
        }
        else
        {
            tolerance = p_motion->config.turn_tolerance_rad;
            angular *= direction;
        }
        if ((direction * p_motion->progress) >=
            (fabs(p_motion->request.target) - tolerance))
        {
            motion_finish(p_motion);
            return MOTION_OK;
        }
    }
    left_target = forward -
                  (0.5 * p_motion->config.odometry.track_width_m * angular);
    right_target = forward +
                   (0.5 * p_motion->config.odometry.track_width_m * angular);
    if ((CORE_OK != core_pid_step(&p_motion->config.wheel_pid,
                                  &p_motion->left_pid, left_target,
                                  p_motion->output.left_speed_mps, dt_s,
                                  &left_duty)) ||
        (CORE_OK != core_pid_step(&p_motion->config.wheel_pid,
                                  &p_motion->right_pid, right_target,
                                  p_motion->output.right_speed_mps, dt_s,
                                  &right_duty)))
    {
        return MOTION_SENSOR_FAULT;
    }
    p_motion->output.left_duty = left_duty;
    p_motion->output.right_duty = right_duty;
    return MOTION_OK;
}

/** @brief Return direction without floating equality comparisons. */
static core_real_t motion_sign (core_real_t value)
{
    return (value < 0.0) ? -1.0 : 1.0;
}

/** @brief Round a finite value in thousandths by rule, saturating
 * symmetrically at the field's +/-(2^(width-1)-1).
 */
static int32_t motion_quantise (core_real_t value, bitpack_round_t rule,
                                uint32_t width)
{
    int32_t limit = bitpack_signed_max(width);
    int32_t code = 0;

    if (!bitpack_quantise(value, MOTION_MILLI, rule, -limit, limit, &code))
    {
        /* Only a product beyond DBL_MAX fails here: saturate by sign. */
        code = (value < 0.0) ? -limit : limit;
    }
    return code;
}

/** @brief Signed field in thousandths; non-finite gives the reserved code. */
static uint32_t motion_encode (core_real_t value, bitpack_round_t rule,
                               uint32_t width)
{
    if (!isfinite(value))
    {
        return bitpack_invalid(width);
    }
    return bitpack_signed(motion_quantise(value, rule, width), width);
}

/** @brief Thousandths back to SI units; the reserved code gives NAN. */
static core_real_t motion_decode (uint32_t field, uint32_t width)
{
    if (bitpack_invalid(width) == field)
    {
        return NAN;
    }
    return (core_real_t) bitpack_extend(field, width) / MOTION_MILLI;
}

/** @brief DISTANCE mm or TURN mrad, magnitude rounded up (AWAY), so every
 * |target| > tolerance decision stays exact. A TURN target with
 * |target| <= 2*pi is capped at 6283 mrad, keeping that check exact too.
 * HALT stores 0; VELOCITY stores 0 unless its target is non-finite.
 */
static uint32_t motion_target_field (const motion_request_t * p_request)
{
    uint32_t field = 0U;
    int32_t  code = 0;

    if (MOTION_HALT == p_request->kind)
    {
        field = 0U;
    }
    else if (!isfinite(p_request->target))
    {
        /* Reserved for every other kind, so validation still rejects it. */
        field = bitpack_invalid(MOTION_TARGET_BITS);
    }
    else if (MOTION_VELOCITY == p_request->kind)
    {
        field = 0U;
    }
    else
    {
        code = motion_quantise(p_request->target, BITPACK_AWAY,
                               MOTION_TARGET_BITS);
        if ((MOTION_TURN == p_request->kind) &&
            (fabs(p_request->target) <= MOTION_TURN_LIMIT_RAD))
        {
            code = (code > MOTION_TURN_MAX_MRAD) ?
                   MOTION_TURN_MAX_MRAD : code;
            code = (code < -MOTION_TURN_MAX_MRAD) ?
                   -MOTION_TURN_MAX_MRAD : code;
        }
        field = bitpack_signed(code, MOTION_TARGET_BITS);
    }
    return field;
}

/** @brief Only the four enumerators are primitives; anything else is not. */
static bool motion_kind_defined (motion_kind_t kind)
{
    bool b_defined = false;

    switch (kind)
    {
        case MOTION_VELOCITY:
        case MOTION_DISTANCE:
        case MOTION_TURN:
        case MOTION_HALT:
            b_defined = true;
        break;
        default:
            b_defined = false;
        break;
    }
    return b_defined;
}

/** End of file. */


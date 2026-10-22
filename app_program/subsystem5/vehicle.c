/** @file vehicle.c
 * @brief Bounded line, junction, scan, bypass and recovery coordination.
 * All clearance inputs are evidence gates supplied by calibrated sensing.
 * Early validation returns follow BARR-C 6.2.c's readability allowance.
 */
#include "vehicle.h"

#include "bitpack.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Millisecond time limit for configured timeouts, not a command ID bound. */
#define VEHICLE_SERIAL_HALF (UINT32_C(2147483648))
#define VEHICLE_BYPASS_PHASES (7U)
/* Highest command ID; (id mod 255) + 1 runs 1..255 and skips 0. */
#define VEHICLE_ID_LAST ((UINT32_C(1) << MOTION_ID_BITS) - 1U)
#define VEHICLE_PHASE_MASK ((UINT32_C(1) << VEHICLE_PHASE_BITS) - 1U)
#define VEHICLE_SIDE_MASK ((UINT32_C(1) << VEHICLE_SIDE_BITS) - 1U)

/* The phase reaches VEHICLE_BYPASS_PHASES before REACQUIRE resets it. */
#if (VEHICLE_BYPASS_PHASES > VEHICLE_PHASE_MASK)
#error "vehicle_t.phase is too narrow for VEHICLE_BYPASS_PHASES"
#endif

static bool vehicle_config_valid (const vehicle_config_t * p_config);
static void vehicle_fault (vehicle_t * p_vehicle,
                            vehicle_output_t * p_output);
static void vehicle_halt (vehicle_t * p_vehicle,
                           vehicle_output_t * p_output);
static void vehicle_enter (vehicle_t * p_vehicle, vehicle_state_t state,
                            uint32_t now_ms);
static void vehicle_request (vehicle_t * p_vehicle, motion_kind_t kind,
                              core_real_t target,
                              vehicle_output_t * p_output);
static void vehicle_follow (vehicle_t * p_vehicle,
                             const vehicle_input_t * p_input,
                             vehicle_output_t * p_output);
static void vehicle_range_wait (vehicle_t * p_vehicle, uint32_t now_ms,
                                 const vehicle_input_t * p_input,
                                 vehicle_output_t * p_output);
static void vehicle_navigate (vehicle_t * p_vehicle, uint32_t now_ms,
                               const vehicle_input_t * p_input,
                               vehicle_output_t * p_output);
static void vehicle_scan_step (vehicle_t * p_vehicle, uint32_t now_ms,
                                const vehicle_input_t * p_input,
                                vehicle_output_t * p_output);
static void vehicle_bypass (vehicle_t * p_vehicle, uint32_t now_ms,
                             const vehicle_input_t * p_input,
                             vehicle_output_t * p_output);
static void vehicle_reacquire (vehicle_t * p_vehicle, uint32_t now_ms,
                                const vehicle_input_t * p_input,
                                vehicle_output_t * p_output);
static bool vehicle_completed (const vehicle_t * p_vehicle,
                                const vehicle_input_t * p_input);
static bool vehicle_safety (vehicle_t * p_vehicle, uint32_t now_ms,
                             const vehicle_input_t * p_input,
                             vehicle_output_t * p_output);
static bool vehicle_range_safety (vehicle_t * p_vehicle, uint32_t now_ms,
                                   const vehicle_input_t * p_input,
                                   vehicle_output_t * p_output);
static void vehicle_dispatch (vehicle_t * p_vehicle, uint32_t now_ms,
                               const vehicle_input_t * p_input,
                               vehicle_output_t * p_output);
static core_real_t vehicle_nav_angle (vehicle_nav_t navigation);
static bool vehicle_scan_valid (const vehicle_t * p_vehicle,
                                 const vehicle_input_t * p_input);

bool vehicle_init (vehicle_t * p_vehicle,
                   const vehicle_config_t * p_config)
{
    if (NULL == p_vehicle)
    {
        return false;
    }
    memset(p_vehicle, 0, sizeof(*p_vehicle));
    if (!vehicle_config_valid(p_config))
    {
        p_vehicle->state = VEHICLE_FAULT;
        return false;
    }
    p_vehicle->config = *p_config;
    p_vehicle->state = VEHICLE_INIT;
    p_vehicle->b_initialized = true;
    return true;
}

bool vehicle_step (vehicle_t * p_vehicle, uint32_t now_ms,
                   const vehicle_input_t * p_input,
                   vehicle_output_t * p_output)
{
    if (NULL == p_output)
    {
        return false;
    }
    memset(p_output, 0, sizeof(*p_output));
    if ((NULL == p_vehicle) || (NULL == p_input) ||
        (!p_vehicle->b_initialized))
    {
        p_output->state = VEHICLE_FAULT;
        p_output->b_stop = true;
        return false;
    }
    if (vehicle_safety(p_vehicle, now_ms, p_input, p_output))
    {
        vehicle_dispatch(p_vehicle, now_ms, p_input, p_output);
    }
    p_output->state = p_vehicle->state;
    return true;
}

/** @brief Validate numerical bounds independently from confirmation flags. */
static bool vehicle_config_valid (const vehicle_config_t * p_config)
{
    if (NULL == p_config)
    {
        return false;
    }
    return (isfinite(p_config->forward_mps)) &&
           (p_config->forward_mps > 0.0) &&
           (isfinite(p_config->turn_rps)) && (p_config->turn_rps > 0.0) &&
           (isfinite(p_config->junction_distance_m)) &&
           (p_config->junction_distance_m > 0.0) &&
           (isfinite(p_config->reacquire_distance_m)) &&
           (p_config->reacquire_distance_m > 0.0) &&
           (isfinite(p_config->max_bypass_offset_m)) &&
           (p_config->max_bypass_offset_m > 0.0) &&
           (isfinite(p_config->max_bypass_length_m)) &&
           (p_config->max_bypass_length_m > 0.0) &&
           (p_config->scan_timeout_ms > 0U) &&
           (p_config->scan_timeout_ms < VEHICLE_SERIAL_HALF) &&
           (p_config->reacquire_timeout_ms > 0U) &&
           (p_config->reacquire_timeout_ms < VEHICLE_SERIAL_HALF) &&
           (p_config->range_max_age_ms > 0U) &&
           (p_config->range_max_age_ms < VEHICLE_SERIAL_HALF) &&
           (p_config->range_wait_timeout_ms > 0U) &&
           (p_config->range_wait_timeout_ms < VEHICLE_SERIAL_HALF) &&
           (p_config->pending_nav_timeout_ms > 0U) &&
           (p_config->pending_nav_timeout_ms < VEHICLE_SERIAL_HALF);
}

/** @brief A supervisor fault has a sticky motor-stop output on every tick. */
static void vehicle_fault (vehicle_t * p_vehicle,
                            vehicle_output_t * p_output)
{
    p_vehicle->state = VEHICLE_FAULT;
    p_vehicle->b_active = false;
    p_output->b_issue_motion = false;
    p_output->scan = VEHICLE_SCAN_NONE;
    p_output->b_stop = true;
}

/** @brief Scanning and successful transitions use a non-latching halt. */
static void vehicle_halt (vehicle_t * p_vehicle,
                           vehicle_output_t * p_output)
{
    p_vehicle->b_active = false;
    memset(&p_output->request, 0, sizeof(p_output->request));
    p_output->request.kind = MOTION_HALT;
    p_output->b_issue_motion = true;
}

/** @brief Enter a fresh bounded phase and cancel the supervisor lease. */
static void vehicle_enter (vehicle_t * p_vehicle, vehicle_state_t state,
                            uint32_t now_ms)
{
    p_vehicle->state = state;
    p_vehicle->entered_ms = now_ms;
    p_vehicle->phase = 0U;
    p_vehicle->b_active = false;
}

/** @brief Create one primitive or renew its exact ID and unchanged target. */
static void vehicle_request (vehicle_t * p_vehicle, motion_kind_t kind,
                              core_real_t target,
                              vehicle_output_t * p_output)
{
    if (!p_vehicle->b_active)
    {
        /* 1..255 then 1 again: never the reserved 0, and fits uint8_t. */
        p_vehicle->next_id =
            (uint8_t) ((p_vehicle->next_id % VEHICLE_ID_LAST) + 1U);
        memset(&p_vehicle->active, 0, sizeof(p_vehicle->active));
        p_vehicle->active.command_id = p_vehicle->next_id;
        p_vehicle->active.kind = kind;
        p_vehicle->active.target = target;
        if (MOTION_TURN == kind)
        {
            p_vehicle->active.yaw_rps = p_vehicle->config.turn_rps;
        }
        else
        {
            p_vehicle->active.forward_mps = p_vehicle->config.forward_mps;
        }
        p_vehicle->b_active = true;
    }
    p_output->request = p_vehicle->active;
    p_output->b_issue_motion = true;
}

/** @brief Replace the running velocity snapshot using the current lease ID. */
static void vehicle_follow (vehicle_t * p_vehicle,
                             const vehicle_input_t * p_input,
                             vehicle_output_t * p_output)
{
    if (p_input->b_line_wait)
    {
        vehicle_halt(p_vehicle, p_output);
        return;
    }
    if ((!p_input->b_line_valid) ||
        (!isfinite(p_input->line_forward_mps)) ||
        (!isfinite(p_input->line_yaw_rps)) ||
        (p_input->line_forward_mps < 0.0) ||
        (p_input->line_forward_mps > p_vehicle->config.forward_mps) ||
        (fabs(p_input->line_yaw_rps) > p_vehicle->config.turn_rps))
    {
        vehicle_fault(p_vehicle, p_output);
        return;
    }
    vehicle_request(p_vehicle, MOTION_VELOCITY, 0.0, p_output);
    p_vehicle->active.forward_mps = p_input->line_forward_mps;
    p_vehicle->active.yaw_rps = p_input->line_yaw_rps;
    p_output->request = p_vehicle->active;
}

/** @brief Reobserve after actual rest without extending the pause deadline. */
static void vehicle_range_wait (vehicle_t * p_vehicle, uint32_t now_ms,
                                 const vehicle_input_t * p_input,
                                 vehicle_output_t * p_output)
{
    vehicle_halt(p_vehicle, p_output);
    if ((now_ms - p_vehicle->entered_ms) >=
        p_vehicle->config.range_wait_timeout_ms)
    {
        vehicle_fault(p_vehicle, p_output);
        return;
    }
    if ((!p_input->motion.b_stationary) ||
        (MOTION_IDLE != p_input->motion.state))
    {
        p_vehicle->phase = 0U;
        return;
    }
    if (0U == p_vehicle->phase)
    {
        p_vehicle->range_stationary_ms = now_ms;
        p_vehicle->phase = 1U;
        return;
    }
    if ((!p_input->b_range_valid) ||
        (p_input->range_age_ms >= p_vehicle->config.range_max_age_ms) ||
        (p_input->range_age_ms >=
         (now_ms - p_vehicle->range_stationary_ms)))
    {
        return;
    }
    if (p_input->b_obstacle)
    {
        vehicle_enter(p_vehicle, VEHICLE_SCAN, now_ms);
    }
    else if (!p_input->b_front_clear)
    {
        vehicle_fault(p_vehicle, p_output);
    }
    else if (p_input->b_line_valid && (!p_input->b_line_wait))
    {
        /* The next FOLLOW tick revalidates all inputs before a new lease. */
        vehicle_enter(p_vehicle, VEHICLE_FOLLOW, now_ms);
    }
    else
    {
        /* Optical searching may continue only within the same deadline. */
    }
}

/** @brief Drive the junction approach, then pivot; STRAIGHT drives it alone.
 * The junction edge fires when the line sensors reach the crossbar, ahead of
 * the axle. Phase 0 drives junction_distance_m (the sensor-to-axle offset)
 * so the axle, the in-place pivot centre, is over the junction; phase 1
 * turns LEFT, RIGHT or UTURN. Detection-to-pivot travel is unmeasured.
 */
static void vehicle_navigate (vehicle_t * p_vehicle, uint32_t now_ms,
                               const vehicle_input_t * p_input,
                               vehicle_output_t * p_output)
{
    bool b_straight = (VEHICLE_NAV_STRAIGHT == p_vehicle->pending_nav);
    uint32_t final_phase = b_straight ? 1U : 2U;

    if (vehicle_completed(p_vehicle, p_input))
    {
        /* At most 2 here; the mask never wraps a reachable phase. */
        p_vehicle->phase = (p_vehicle->phase + 1U) & VEHICLE_PHASE_MASK;
        p_vehicle->b_active = false;
    }
    if (p_vehicle->phase >= final_phase)
    {
        p_vehicle->pending_nav = VEHICLE_NAV_NONE;
        vehicle_enter(p_vehicle, VEHICLE_REACQUIRE, now_ms);
        vehicle_halt(p_vehicle, p_output);
    }
    else if ((!b_straight) && (1U == p_vehicle->phase))
    {
        vehicle_request(p_vehicle, MOTION_TURN,
                        vehicle_nav_angle(p_vehicle->pending_nav), p_output);
    }
    else
    {
        vehicle_request(p_vehicle, MOTION_DISTANCE,
                        p_vehicle->config.junction_distance_m, p_output);
    }
}

/** @brief Wait for rest, then request coarse scan and its fine refinement. */
static void vehicle_scan_step (vehicle_t * p_vehicle, uint32_t now_ms,
                                const vehicle_input_t * p_input,
                                vehicle_output_t * p_output)
{
    vehicle_halt(p_vehicle, p_output);
    if ((now_ms - p_vehicle->entered_ms) >=
        p_vehicle->config.scan_timeout_ms)
    {
        vehicle_fault(p_vehicle, p_output);
        return;
    }
    if ((0U == p_vehicle->phase) && p_input->motion.b_stationary)
    {
        p_vehicle->phase = 1U;
        p_output->scan = VEHICLE_SCAN_COARSE;
    }
    else if ((1U == p_vehicle->phase) && p_input->b_coarse_ready)
    {
        p_vehicle->phase = 2U;
        p_output->scan = VEHICLE_SCAN_FINE;
    }
    else if ((2U == p_vehicle->phase) && p_input->b_fine_ready)
    {
        if (!vehicle_scan_valid(p_vehicle, p_input))
        {
            vehicle_fault(p_vehicle, p_output);
            return;
        }
        p_vehicle->bypass_offset_m = p_input->bypass_offset_m;
        p_vehicle->bypass_length_m = p_input->bypass_length_m;
        /* Validated as -1 or +1 above: a 2-bit two's complement code. */
        p_vehicle->bypass_side =
            bitpack_signed(p_input->bypass_side, VEHICLE_SIDE_BITS) &
            VEHICLE_SIDE_MASK;
        vehicle_enter(p_vehicle, VEHICLE_BYPASS, now_ms);
    }
    else
    {
        /* Wait only within the overall scan timeout. */
    }
}

/** @brief Follow the assessed rectangular detour and restore entry heading. */
static void vehicle_bypass (vehicle_t * p_vehicle, uint32_t now_ms,
                             const vehicle_input_t * p_input,
                             vehicle_output_t * p_output)
{
    core_real_t side =
        (bitpack_extend(p_vehicle->bypass_side, VEHICLE_SIDE_BITS) > 0) ?
        1.0 : -1.0;
    core_real_t quarter_turn = side * (0.5 * CORE_PI);

    if (vehicle_completed(p_vehicle, p_input))
    {
        /* At most VEHICLE_BYPASS_PHASES, which the #if above proves fits. */
        p_vehicle->phase = (p_vehicle->phase + 1U) & VEHICLE_PHASE_MASK;
        p_vehicle->b_active = false;
    }
    if (p_vehicle->phase >= VEHICLE_BYPASS_PHASES)
    {
        vehicle_enter(p_vehicle, VEHICLE_REACQUIRE, now_ms);
        vehicle_halt(p_vehicle, p_output);
        return;
    }
    if ((!p_input->b_range_valid) ||
        (p_input->range_age_ms >= p_vehicle->config.range_max_age_ms))
    {
        vehicle_fault(p_vehicle, p_output);
        return;
    }
    if ((1U == (p_vehicle->phase % 2U)) &&
        ((!p_input->b_front_clear) || (p_input->b_obstacle)))
    {
        vehicle_fault(p_vehicle, p_output);
        return;
    }
    switch (p_vehicle->phase)
    {
        case 0U:
        case 6U:
            vehicle_request(p_vehicle, MOTION_TURN, quarter_turn, p_output);
        break;
        case 1U:
        case 5U:
            vehicle_request(p_vehicle, MOTION_DISTANCE,
                            p_vehicle->bypass_offset_m, p_output);
        break;
        case 2U:
        case 4U:
            vehicle_request(p_vehicle, MOTION_TURN, -quarter_turn, p_output);
        break;
        case 3U:
            vehicle_request(p_vehicle, MOTION_DISTANCE,
                            p_vehicle->bypass_length_m, p_output);
        break;
        default:
            vehicle_fault(p_vehicle, p_output);
        break;
    }
}

/** @brief Require the constrained original line or stop after bounded search.
 */
static void vehicle_reacquire (vehicle_t * p_vehicle, uint32_t now_ms,
                                const vehicle_input_t * p_input,
                                vehicle_output_t * p_output)
{
    if ((now_ms - p_vehicle->entered_ms) >=
        p_vehicle->config.reacquire_timeout_ms)
    {
        vehicle_fault(p_vehicle, p_output);
    }
    else if (p_input->b_original_line_verified && p_input->b_line_valid)
    {
        vehicle_enter(p_vehicle, VEHICLE_FOLLOW, now_ms);
        vehicle_halt(p_vehicle, p_output);
    }
    else if (vehicle_completed(p_vehicle, p_input))
    {
        vehicle_fault(p_vehicle, p_output);
    }
    else
    {
        vehicle_request(p_vehicle, MOTION_DISTANCE,
                        p_vehicle->config.reacquire_distance_m, p_output);
    }
}

/** @brief Consume completion only when it belongs to the current primitive. */
static bool vehicle_completed (const vehicle_t * p_vehicle,
                                const vehicle_input_t * p_input)
{
    return (p_vehicle->b_active) &&
           (MOTION_COMPLETE == p_input->motion.state) &&
           (p_vehicle->active.command_id == p_input->motion.command_id);
}

/** @brief Evaluate stop/reset and critical sensing before mode dispatch. */
static bool vehicle_safety (vehicle_t * p_vehicle, uint32_t now_ms,
                             const vehicle_input_t * p_input,
                             vehicle_output_t * p_output)
{
    if (p_input->b_stop)
    {
        vehicle_fault(p_vehicle, p_output);
        return false;
    }
    if (VEHICLE_FAULT == p_vehicle->state)
    {
        if ((p_input->b_reset) && (p_input->b_fault_clear) &&
            (p_input->motion.b_stationary) && (p_input->b_sensors_ready) &&
            (p_vehicle->config.b_calibration_confirmed) &&
            (p_vehicle->config.b_course_confirmed))
        {
            p_vehicle->pending_nav = VEHICLE_NAV_NONE;
            vehicle_enter(p_vehicle, VEHICLE_IDLE, now_ms);
            p_output->b_reset_motion = true;
        }
        else
        {
            p_output->b_stop = true;
        }
        return false;
    }
    if ((VEHICLE_INIT == p_vehicle->state) ||
        (VEHICLE_IDLE == p_vehicle->state))
    {
        return true;
    }
    if ((!p_input->b_sensors_ready) ||
        (MOTION_INHIBITED == p_input->motion.state) ||
        (p_input->navigation < VEHICLE_NAV_NONE) ||
        (p_input->navigation > VEHICLE_NAV_UTURN))
    {
        vehicle_fault(p_vehicle, p_output);
        return false;
    }
    if (((VEHICLE_FOLLOW == p_vehicle->state) ||
         (VEHICLE_RANGE_WAIT == p_vehicle->state)) &&
        (!p_input->b_line_valid) && (!p_input->b_line_wait))
    {
        /* Barcode/obstacle events cannot restore a terminally lost line. */
        vehicle_fault(p_vehicle, p_output);
        return false;
    }
    if ((VEHICLE_NAV_NONE != p_vehicle->pending_nav) &&
        (VEHICLE_NAVIGATE != p_vehicle->state) &&
        ((now_ms - p_vehicle->pending_nav_ms) >=
         p_vehicle->config.pending_nav_timeout_ms))
    {
        vehicle_fault(p_vehicle, p_output);
        return false;
    }
    if ((VEHICLE_FOLLOW == p_vehicle->state) &&
        (VEHICLE_NAV_NONE != p_input->navigation))
    {
        if (VEHICLE_NAV_NONE != p_vehicle->pending_nav)
        {
            vehicle_fault(p_vehicle, p_output);
            return false;
        }
        p_vehicle->pending_nav = p_input->navigation;
        p_vehicle->pending_nav_ms = now_ms;
    }
    return vehicle_range_safety(p_vehicle, now_ms, p_input, p_output);
}

/** @brief Retry lost range in FOLLOW only; interrupted primitives fault. */
static bool vehicle_range_safety (vehicle_t * p_vehicle, uint32_t now_ms,
                                   const vehicle_input_t * p_input,
                                   vehicle_output_t * p_output)
{
    if ((VEHICLE_SCAN == p_vehicle->state) ||
        (VEHICLE_BYPASS == p_vehicle->state))
    {
        return true;
    }
    if (VEHICLE_RANGE_WAIT == p_vehicle->state)
    {
        if ((VEHICLE_NAV_NONE != p_input->navigation) ||
            ((VEHICLE_NAV_NONE != p_vehicle->pending_nav) &&
             p_input->b_junction_edge))
        {
            vehicle_fault(p_vehicle, p_output);
            return false;
        }
        return true;
    }
    if ((!p_input->b_range_valid) ||
        (p_input->range_age_ms >= p_vehicle->config.range_max_age_ms))
    {
        if ((VEHICLE_FOLLOW == p_vehicle->state) &&
            ((!p_input->b_junction_edge) ||
             (VEHICLE_NAV_NONE == p_vehicle->pending_nav)))
        {
            vehicle_enter(p_vehicle, VEHICLE_RANGE_WAIT, now_ms);
            vehicle_halt(p_vehicle, p_output);
        }
        else
        {
            vehicle_fault(p_vehicle, p_output);
        }
        return false;
    }
    if (p_input->b_obstacle)
    {
        if (VEHICLE_FOLLOW == p_vehicle->state)
        {
            vehicle_enter(p_vehicle, VEHICLE_SCAN, now_ms);
            vehicle_halt(p_vehicle, p_output);
        }
        else
        {
            /* An interrupted junction action lacks a confirmed map anchor.
             * Replaying its complete angle could turn an extra 90 degrees.
             * Require explicit stop/reset instead of guessing a new route.
             */
            vehicle_fault(p_vehicle, p_output);
        }
        return false;
    }
    if (!p_input->b_front_clear)
    {
        vehicle_fault(p_vehicle, p_output);
        return false;
    }
    return true;
}

/** @brief Mode-specific actions; unknown state always inhibits motion. */
static void vehicle_dispatch (vehicle_t * p_vehicle, uint32_t now_ms,
                               const vehicle_input_t * p_input,
                               vehicle_output_t * p_output)
{
    switch (p_vehicle->state)
    {
        case VEHICLE_INIT:
            vehicle_halt(p_vehicle, p_output);
            if ((p_vehicle->config.b_calibration_confirmed) &&
                (p_vehicle->config.b_course_confirmed) &&
                (p_input->b_sensors_ready) && (p_input->b_range_valid) &&
                (p_input->range_age_ms < p_vehicle->config.range_max_age_ms))
            {
                vehicle_enter(p_vehicle, VEHICLE_IDLE, now_ms);
            }
        break;
        case VEHICLE_IDLE:
            vehicle_halt(p_vehicle, p_output);
            if ((p_input->b_start) && (p_input->b_sensors_ready) &&
                (p_input->b_line_valid) && (p_input->b_range_valid) &&
                (p_input->b_front_clear) && (!p_input->b_obstacle) &&
                (p_input->range_age_ms < p_vehicle->config.range_max_age_ms) &&
                (MOTION_INHIBITED != p_input->motion.state))
            {
                vehicle_enter(p_vehicle, VEHICLE_FOLLOW, now_ms);
            }
        break;
        case VEHICLE_FOLLOW:
            if ((VEHICLE_NAV_NONE != p_vehicle->pending_nav) &&
                p_input->b_junction_edge &&
                (0U != (now_ms - p_vehicle->pending_nav_ms)))
            {
                vehicle_enter(p_vehicle, VEHICLE_NAVIGATE, now_ms);
                vehicle_navigate(p_vehicle, now_ms, p_input, p_output);
            }
            else
            {
                vehicle_follow(p_vehicle, p_input, p_output);
            }
        break;
        case VEHICLE_NAVIGATE:
            vehicle_navigate(p_vehicle, now_ms, p_input, p_output);
        break;
        case VEHICLE_SCAN:
            vehicle_scan_step(p_vehicle, now_ms, p_input, p_output);
        break;
        case VEHICLE_BYPASS:
            vehicle_bypass(p_vehicle, now_ms, p_input, p_output);
        break;
        case VEHICLE_REACQUIRE:
            vehicle_reacquire(p_vehicle, now_ms, p_input, p_output);
        break;
        case VEHICLE_RANGE_WAIT:
            vehicle_range_wait(p_vehicle, now_ms, p_input, p_output);
        break;
        case VEHICLE_FAULT:
            vehicle_fault(p_vehicle, p_output);
        break;
        default:
            vehicle_fault(p_vehicle, p_output);
        break;
    }
}

/** @brief Positive yaw is left; the U-turn convention is counterclockwise. */
static core_real_t vehicle_nav_angle (vehicle_nav_t navigation)
{
    core_real_t angle = 0.0;

    switch (navigation)
    {
        case VEHICLE_NAV_LEFT:
            angle = 0.5 * CORE_PI;
        break;
        case VEHICLE_NAV_RIGHT:
            angle = -0.5 * CORE_PI;
        break;
        case VEHICLE_NAV_UTURN:
            angle = CORE_PI;
        break;
        case VEHICLE_NAV_NONE:
        case VEHICLE_NAV_STRAIGHT:
            angle = 0.0;
        break;
        default:
            angle = 0.0;
        break;
    }
    return angle;
}

/** @brief Validated finite route dimensions and an assessed swept corridor. */
static bool vehicle_scan_valid (const vehicle_t * p_vehicle,
                                 const vehicle_input_t * p_input)
{
    return (p_input->b_bypass_clearance) &&
           ((1 == p_input->bypass_side) || (-1 == p_input->bypass_side)) &&
           (isfinite(p_input->bypass_offset_m)) &&
           (p_input->bypass_offset_m > 0.0) &&
           (p_input->bypass_offset_m <=
            p_vehicle->config.max_bypass_offset_m) &&
           (isfinite(p_input->bypass_length_m)) &&
           (p_input->bypass_length_m > 0.0) &&
           (p_input->bypass_length_m <=
            p_vehicle->config.max_bypass_length_m);
}

/** End of file. */


/** @file vehicle.h
 * @brief Bounded autonomous mission supervisor; pure snapshot/event interface.
 * A single task owns this state and issues all normal motion/scan requests.
 */
#ifndef VEHICLE_H
#define VEHICLE_H

#include "motion.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    VEHICLE_INIT = 0,
    VEHICLE_IDLE,
    VEHICLE_FOLLOW,
    VEHICLE_NAVIGATE,
    VEHICLE_SCAN,
    VEHICLE_BYPASS,
    VEHICLE_REACQUIRE,
    VEHICLE_FAULT,
    VEHICLE_RANGE_WAIT
} vehicle_state_t;

typedef enum
{
    VEHICLE_NAV_NONE = 0,
    VEHICLE_NAV_LEFT,
    VEHICLE_NAV_RIGHT,
    VEHICLE_NAV_STRAIGHT,
    VEHICLE_NAV_UTURN
} vehicle_nav_t;

typedef enum
{
    VEHICLE_SCAN_NONE = 0,
    VEHICLE_SCAN_COARSE,
    VEHICLE_SCAN_FINE
} vehicle_scan_t;

/** junction_distance_m is the approach driven after the junction edge and
 * before any NAVIGATE pivot: the measured travel from edge detection to the
 * axle over the crossbar centre (about the line-sensor-to-axle offset + half
 * the line width - detection travel), not the raw geometric offset. */
typedef struct
{
    core_real_t forward_mps;
    core_real_t turn_rps;
    core_real_t junction_distance_m;
    core_real_t reacquire_distance_m;
    core_real_t max_bypass_offset_m;
    core_real_t max_bypass_length_m;
    uint32_t    scan_timeout_ms;
    uint32_t    reacquire_timeout_ms;
    uint32_t    range_max_age_ms;
    uint32_t    range_wait_timeout_ms;
    uint32_t    pending_nav_timeout_ms;
    bool        b_calibration_confirmed;
    bool        b_course_confirmed;
} vehicle_config_t;

typedef struct
{
    motion_output_t motion;
    vehicle_nav_t   navigation;
    core_real_t     line_forward_mps;
    core_real_t     line_yaw_rps;
    core_real_t     bypass_offset_m;
    core_real_t     bypass_length_m;
    int32_t         bypass_side;
    uint32_t        range_age_ms;
    bool            b_start;
    bool            b_stop;
    bool            b_reset;
    bool            b_fault_clear;
    bool            b_sensors_ready;
    bool            b_line_valid;
    bool            b_junction_edge;
    bool            b_line_wait;
    bool            b_obstacle;
    bool            b_range_valid;
    bool            b_front_clear;
    bool            b_coarse_ready;
    bool            b_fine_ready;
    bool            b_bypass_clearance;
    bool            b_original_line_verified;
} vehicle_input_t;

typedef struct
{
    vehicle_state_t  state;
    motion_request_t request;
    vehicle_scan_t   scan;
    bool             b_issue_motion;
    bool             b_stop;
    bool             b_reset_motion;
} vehicle_output_t;

/* Stored-state field widths: phase holds 0..VEHICLE_BYPASS_PHASES (7) and
 * bypass_side is a two's complement -1/0/+1 (bitpack_signed/extend).
 */
#define VEHICLE_PHASE_BITS (3U)
#define VEHICLE_SIDE_BITS  (2U)

typedef struct
{
    vehicle_config_t config;
    motion_request_t active;
    uint32_t         entered_ms;
    uint32_t         pending_nav_ms;
    uint32_t         range_stationary_ms;
    core_real_t      bypass_offset_m;
    core_real_t      bypass_length_m;
    /* Primitive IDs run 1..255, then wrap to 1; 0 is never issued. */
    uint8_t          next_id;
    vehicle_state_t  state         : 4;
    vehicle_nav_t    pending_nav   : 3;
    uint32_t         phase         : 3;
    uint32_t         bypass_side   : 2;
    bool             b_active      : 1;
    bool             b_initialized : 1;
} vehicle_t;

/** Initialize with reviewed positive bounds; unconfirmed configs stay INIT. */
bool vehicle_init (vehicle_t * p_vehicle,
                   const vehicle_config_t * p_config);

/** Evaluate one snapshot. No allocation, waiting, I/O, or retained event queue.
 * Navigation/junction/coarse/fine inputs are one-shot events.
 * Clear them after this call.
 * b_line_wait is a bounded SEARCHING/debounce state: HALT without latching.
 * The sensor owner must eventually supply valid line or LOST/FAULT.
 * In FOLLOW, terminal line loss precedes barcode and obstacle transitions.
 * Unknown/stale front range never grants forward movement. Scan clearances
 * require an external footprint/swept-path assessment, not merely no echo.
 * Interrupted FOLLOW halts in bounded RANGE_WAIT. Recovery needs an IDLE,
 * independently stationary motion snapshot and a qualified range triggered
 * strictly after that rest observation. range_age_ms is measured from the
 * trigger, not result consumption. No echo cannot qualify as clear range.
 * Range-wait expiry faults until local reset; pending action age is retained.
 * A pending junction edge while range is unavailable faults rather than
 * losing route evidence. Interrupted primitives are never replayed.
 * Original-line verification requires the course/heading/position constraint
 * and stable line lock. A lone black IR sample is not sufficient evidence.
 * A decoded action stays pending in FOLLOW until a later junction edge.
 * A decode and a junction edge in the same snapshot are not ordered: that
 * edge is consumed without action, and the action waits for the next edge
 * or expires. NAVIGATE drives junction_distance_m first, then pivots for
 * LEFT, RIGHT or UTURN; STRAIGHT drives the approach only.
 * Pending actions expire into a stopped FAULT, including during a bypass.
 * Reacquiring a line does not count as evidence of reaching a junction.
 * An obstacle interrupting NAVIGATE faults: no map anchor is available to
 * reconstruct a partly executed junction action without replaying its turn.
 */
bool vehicle_step (vehicle_t * p_vehicle, uint32_t now_ms,
                   const vehicle_input_t * p_input,
                   vehicle_output_t * p_output);

#endif /* VEHICLE_H */

/** End of file. */


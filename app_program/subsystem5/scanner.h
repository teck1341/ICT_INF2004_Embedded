/** @file scanner.h
 * @brief Bounded nonblocking servo/range acquisition and apparent profile.
 *
 * Zero degrees is forward; positive angles point left. Angles describe the
 * calibrated sensor beam, not raw servo commands. Observations are stored as
 * 16-bit pulse codes (scanner_entry_t) and the Scan -> Vehicle/telemetry
 * result is packed (scanner_shared_t). Profile geometry and the working
 * result stay double under the project numeric deviation D-NUMERIC. State
 * and output belong to one task.
 */
#ifndef SCANNER_H
#define SCANNER_H

#include <stdbool.h>
#include <stdint.h>
#include "core.h"

#define SCANNER_MAX_SAMPLES (80U)
#define SCANNER_TABLE_SIZE (61U)   /* -60..+60 degrees in 2-degree steps */
/* scanner_entry_t pulse codes (bits 0-14) and the fine-pass flag (bit 15). */
#define SCANNER_ENTRY_NO_ECHO (0U)
#define SCANNER_ENTRY_PULSE_MAX_US (32766U)
#define SCANNER_ENTRY_NOT_SAMPLED (0x7FFFU)
#define SCANNER_ENTRY_FINE (0x8000U)
/* scanner_shared_t field widths. */
#define SCANNER_SHARED_MM_BITS (12U)
#define SCANNER_SHARED_COUNT_BITS (6U)

typedef enum
{
    SCANNER_IDLE = 0,
    SCANNER_POSITION,
    SCANNER_SETTLE,
    SCANNER_WAIT_ECHO,
    SCANNER_COMPLETE,
    SCANNER_ABORTED
} scanner_phase_t;

typedef enum
{
    SCANNER_SIDE_NONE = 0,
    SCANNER_SIDE_LEFT,
    SCANNER_SIDE_RIGHT
} scanner_side_t;

typedef struct
{
    core_ultrasonic_config_t range;
    double obstacle_distance_m;
    double robot_half_width_m;
    double uncertainty_margin_m;
    double required_clearance_m;
    uint32_t settle_ms;
    uint32_t trigger_interval_ms;
    uint32_t echo_timeout_ms;
    bool b_calibrated;
} scanner_config_t;

typedef struct
{
    bool     b_ready;
    uint32_t pulse_us;
} scanner_echo_t;

/** Unpacked view of one table entry (see scanner_sample). */
typedef struct
{
    int32_t             angle_deg;
    core_range_sample_t range;
    bool                b_fine;
} scanner_sample_t;

/** One entry per even beam angle, newest observation wins. Bits 0-14: 0 =
 * no echo or failed capture, 1..32766 = echo pulse in microseconds
 * (saturated), 32767 = never sampled; bit 15 = taken in the fine pass.
 * State and distance are recomputed exactly with core_ultrasonic_range, so
 * every profile and side decision equals the old list's.
 */
typedef uint16_t scanner_entry_t;

/** Scan-task working result and the unpacked view of scanner_shared_t. */
typedef struct
{
    double nearest_distance_m;
    double apparent_width_m;
    double left_clearance_m;
    double right_clearance_m;
    scanner_side_t preferred_side;
    uint32_t       valid_samples;
    uint32_t       unknown_samples;
    bool           b_profile_valid;
    bool           b_obstacle_seen;
    bool           b_left_observed;
    bool           b_right_observed;
    bool           b_aborted;
} scanner_result_t;

/** Scan -> Vehicle/telemetry, 43 bits in two 32-bit units: nearest and
 * apparent width in mm rounded UP (12 bits, valid only with
 * valid_samples > 0 / b_obstacle_seen), side 2 bits, valid/unknown counts
 * 6 bits (newest per angle, <= 61), five flags. Clearances are not shared
 * (no reader outside the scanner).
 */
typedef struct
{
    uint32_t       nearest_mm       : 12;
    uint32_t       width_mm         : 12;
    scanner_side_t preferred_side   : 2;
    uint32_t       valid_samples    : 6;
    uint32_t       unknown_samples  : 6;
    bool           b_profile_valid  : 1;
    bool           b_obstacle_seen  : 1;
    bool           b_left_observed  : 1;
    bool           b_right_observed : 1;
    bool           b_aborted        : 1;
} scanner_shared_t;

typedef struct
{
    bool    b_set_servo;
    int32_t angle_deg;
    bool    b_trigger;
    bool    b_done;
} scanner_output_t;

/** sample_count is the number of acquisitions this scan (at most 74, bounded
 * by SCANNER_MAX_SAMPLES). Beam angles are even and within -60..60.
 */
typedef struct
{
    scanner_config_t config;
    scanner_result_t result;
    scanner_entry_t  table[SCANNER_TABLE_SIZE];
    int8_t           angle_deg;
    int8_t           fine_end_deg;
    uint32_t         sample_count  : 7;
    scanner_phase_t  phase         : 3;
    bool             b_initialized : 1;
    bool             b_triggered   : 1;
    bool             b_fine        : 1;
    uint32_t         phase_started_ms;
    uint32_t         last_trigger_ms;
} scanner_state_t;

/** Set proposed 150 ms settle, 65 ms interval and 30 ms timeout defaults. */
void scanner_default_config (scanner_config_t * p_config);

/**
 * Validate configuration and reset state. False leaves state unchanged.
 * Besides the physical bounds, the sound speed must place the saturated
 * pulse code (32766 us) beyond max_distance_m (about 244 m/s at 4 m), so a
 * saturated entry classifies exactly like the longer pulse it replaced.
 */
bool scanner_init (scanner_state_t * p_state,
                   const scanner_config_t * p_config);

/** Begin a -60..+60 degree coarse scan; calibrated and stationary required.
 * Every table entry restarts as never sampled.
 */
bool scanner_start (scanner_state_t * p_state, uint32_t now_ms,
                    bool b_stationary);

/**
 * Advance acquisition without blocking. Caller supplies a completed echo only
 * for the current trigger; the board must reject noise/duplicate/stale edges.
 * A trigger output requests a board-generated >=10 us pulse. Polling need not
 * itself have microsecond resolution. Millisecond timestamps wrap safely when
 * consecutive calls are less than 2^31 ms apart. Moving aborts the entire scan.
 * The fine pass uses 2 degree spacing around coarse obstacle/unknown regions.
 * Completion recenters the servo; the board must still wait for it to settle.
 * Missing and out-of-range observations never certify clear space.
 * Clearances are sampled radial free-distance margins, not swept-path proof;
 * preferred_side is a planning hint that requires monitored motion and recheck.
 */
bool scanner_step (scanner_state_t * p_state, uint32_t now_ms,
                   bool b_stationary, const scanner_echo_t * p_echo,
                   scanner_output_t * p_output);

/**
 * Newest observation at angle_deg this scan, with state and distance
 * recomputed by core_ultrasonic_range. False, leaving *p_sample unchanged,
 * for NULL arguments, a state scanner_init never accepted, an odd angle, an
 * angle outside -60..60 or an angle not sampled since scanner_start.
 */
bool scanner_sample (const scanner_state_t * p_state, int32_t angle_deg,
                     scanner_sample_t * p_sample);

/**
 * Pack a result. Distances round UP to whole mm, which keeps d <= T and
 * d > T exact for whole-mm T, and saturate at 4095 mm; they are 0 when not
 * valid. A non-finite distance flagged valid (never produced) stores 4095,
 * beyond every gate. Counts saturate at 63. NULL arguments are ignored.
 */
void scanner_shared_pack (const scanner_result_t * p_result,
                          scanner_shared_t * p_shared);

/** Distances unpack to metres (NAN when not valid); clearances unpack NAN.
 * NULL arguments are ignored.
 */
void scanner_shared_unpack (const scanner_shared_t * p_shared,
                            scanner_result_t * p_result);

#endif /* SCANNER_H */

/** End of file. */


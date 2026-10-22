/**
 * @file barcode.h
 * @brief Configurable spatial run decoder; no course symbology is assumed.
 * @details Frames start/end with black and require white quiet zones.
 * Encoder distance increases in the intended scan direction. The authored
 * configuration is in metres (proposed project BARR-C 5.4 deviation for its
 * doubles); barcode_derive() converts it once to encoder half-counts (left
 * plus right counts), and every per-sample decision is an integer compare.
 * This explicit framing model itself needs course-owner confirmation.
 */
#ifndef BARCODE_H
#define BARCODE_H

#include <stdbool.h>
#include <stdint.h>

#include "line_ctrl.h"

#define BARCODE_MAX_SYMBOLS (16U)
#define BARCODE_MAX_RUNS    (32U)
#define BARCODE_MIN_SCALE   (0.5)
#define BARCODE_MAX_SCALE   (2.0)
/** Stored run widths are uint8_t half-counts; derive rejects wider runs. */
#define BARCODE_MAX_WIDTH_HC (255U)

/** Designated initializers of the illustrative barcode_defaults() limits
 * (unconfirmed, empty codebook) for constant authored tables in flash.
 */
#define BARCODE_DEFAULT_LIMITS                                          \
    .black_on = 0.75, .black_off = 0.55, .minimum_run_m = 0.001,        \
    .maximum_run_m = 0.060, .quiet_zone_m = 0.015,                      \
    .maximum_frame_m = 0.250, .maximum_step_m = 0.0002,                 \
    .reverse_tolerance_m = 0.0005, .absolute_tolerance_m = 0.0003,      \
    .relative_tolerance = 0.15, .minimum_scale = 1.0,                   \
    .maximum_scale = 1.0, .duplicate_distance_m = 0.200,                \
    .frame_timeout_ms = 10000U, .maximum_gap_ms = 50U

typedef enum
{
    BARCODE_ACTION_UNKNOWN = 0,
    BARCODE_ACTION_LEFT,
    BARCODE_ACTION_RIGHT,
    BARCODE_ACTION_STRAIGHT,
    BARCODE_ACTION_UTURN
} barcode_action_t;

typedef enum
{
    BARCODE_EVENT_NONE = 0,
    BARCODE_EVENT_DECODED,
    BARCODE_EVENT_UNKNOWN,
    BARCODE_EVENT_REJECTED,
    BARCODE_EVENT_INHIBITED
} barcode_event_t;

typedef enum
{
    BARCODE_REASON_NONE = 0,
    BARCODE_REASON_INPUT,
    BARCODE_REASON_REVERSE,
    BARCODE_REASON_DISTANCE,
    BARCODE_REASON_TIMEOUT,
    BARCODE_REASON_OVERFLOW,
    BARCODE_REASON_WIDTH,
    BARCODE_REASON_NO_MATCH,
    BARCODE_REASON_AMBIGUOUS,
    BARCODE_REASON_DUPLICATE
} barcode_reason_t;

/** Authored template in metres; the narrow/wide encoding is deferred until
 * the course symbology is confirmed. Duplicate labels are allowed.
 */
typedef struct
{
    double           width_m[BARCODE_MAX_RUNS];
    uint8_t          run_count;
    char             label;
    barcode_action_t action;
} barcode_symbol_t;

/** Authored configuration: a constant table in flash in production. */
typedef struct
{
    barcode_symbol_t symbols[BARCODE_MAX_SYMBOLS];
    double           black_on;
    double           black_off;
    double           minimum_run_m;
    double           maximum_run_m;
    double           quiet_zone_m;
    double           maximum_frame_m;
    double           maximum_step_m;
    double           reverse_tolerance_m;
    double           absolute_tolerance_m;
    double           relative_tolerance;
    double           minimum_scale;
    double           maximum_scale;
    double           duplicate_distance_m;
    uint32_t         frame_timeout_ms;
    uint32_t         maximum_gap_ms;
    uint8_t          symbol_count;
    bool             b_calibration_confirmed;
    bool             b_codebook_confirmed;
    bool             b_scale_tolerant;
} barcode_config_t;

/** Derived once by barcode_derive(). Each metre limit x becomes an exact
 * half-count threshold for its compare: ceil(x / q) for W < x and W >= x,
 * floor(x / q) for W > x and W <= x, where q is metres per half-count.
 * Hysteresis is exact on the sidebarcode ADC channel for the barcode's own
 * black_on/black_off. At frame end each stored width W is matched as
 * fl(W * unit_m) metres with the original per-run predicate against the
 * authored table p_codebook, which must outlive the runtime.
 */
typedef struct
{
    double                   unit_m;
    const barcode_config_t * p_codebook;
    line_ctrl_hysteresis_t   hysteresis;
    uint32_t                 quiet_zone_hc     : 26; /* ceil, W >= */
    uint32_t                 maximum_step_hc   : 6;  /* floor, A > */
    uint32_t                 maximum_frame_hc  : 26; /* floor, F > */
    uint32_t                 duplicate_hc      : 26; /* ceil, |D| < */
    uint32_t                 minimum_run_hc    : 9;  /* ceil, W < */
    uint32_t                 maximum_run_hc    : 8;  /* floor, W > */
    uint32_t                 reverse_hc        : 8;  /* floor, -A > */
    uint32_t                 persistence_hc    : 7;  /* ceil, D >= */
    uint32_t                 frame_timeout_ms  : 22;
    uint32_t                 maximum_gap_ms    : 16;
    uint32_t                 maximum_adc       : 12;
    bool                     b_enabled         : 1;
    bool                     b_scale_tolerant  : 1;
    bool                     b_line_confirmed  : 1;
    bool                     b_valid           : 1;
} barcode_runtime_t;

/** FOLLOW-only decoder history, 64 B. Positions are wrapping uint32_t
 * half-count coordinates whose differences are taken as int32_t; widths
 * are uint8_t half-counts.
 */
typedef struct
{
    uint8_t  widths_hc[BARCODE_MAX_RUNS];
    uint32_t high_water_hc;
    uint32_t run_start_hc;
    uint32_t candidate_start_hc;
    uint32_t frame_start_hc;
    uint32_t last_decode_hc;
    uint32_t previous_ms;
    uint32_t frame_start_ms;
    uint32_t run_count         : 6;
    uint32_t last_symbol       : 4;
    bool     b_started         : 1;
    bool     b_current_black   : 1;
    bool     b_candidate       : 1;
    bool     b_candidate_black : 1;
    bool     b_frame           : 1;
    bool     b_armed           : 1;
    bool     b_last_decode     : 1;
} barcode_state_t;

/** Per-sample result, 20 bits. symbol is the canonical codebook index of
 * the decoded label (the lowest index with the same label character),
 * meaningful only for BARCODE_EVENT_DECODED.
 */
typedef struct
{
    barcode_event_t  event     : 3;
    barcode_reason_t reason    : 4;
    barcode_action_t action    : 3;
    uint32_t         symbol    : 4;
    uint32_t         run_count : 6;
} barcode_output_t;

/** @brief Set unconfirmed illustrative limits and an empty codebook. */
void barcode_defaults (barcode_config_t * p_config);

/** @brief Check structural validity; confirmation gates are independent. */
bool barcode_config_valid (const barcode_config_t * p_config);

/**
 * @brief Convert a valid authored configuration to integer thresholds.
 * @param unit_m Metres per half-count of centre path (pi * R / counts).
 * @param p_line Line calibration; channel LINE_CTRL_BARCODE_CHANNEL is the
 * sidebarcode sensor. Its confirmation flag and maximum_adc are kept.
 * @return False, with an invalid (zeroed) runtime, for NULL, an invalid
 * configuration or line calibration, a unit that is not finite and
 * positive, a maximum run above 255 half-counts or a distance limit of
 * 2^26 half-counts or more.
 */
bool barcode_derive (const barcode_config_t * p_config, double unit_m,
                     const line_ctrl_config_t * p_line,
                     barcode_runtime_t * p_runtime);

/**
 * @brief Class of one sidebarcode ADC sample for barcode_step().
 * @return INVALID for NULL, an invalid runtime or raw above maximum_adc;
 * LIGHT while the line calibration is unconfirmed (the inhibited line
 * reported darkness 0); otherwise the exact hysteresis class.
 */
line_ctrl_class_t barcode_classify (const barcode_runtime_t * p_runtime,
                                    uint16_t raw);

/** @brief Reset history; NULL is ignored. Call before the first sample. */
void barcode_init (barcode_state_t * p_state);

/**
 * @brief Sample classified darkness and monotonically forward distance.
 * @param darkness LIGHT, BAND (keep the last colour) or DARK, from
 * barcode_classify(); INVALID is rejected.
 * @param position_hc Wrapping centre-path coordinate in half-counts.
 * @param now_ms Monotonic wrapping milliseconds with bounded sample gaps.
 * @return False for invalid pointer/runtime/sample; true otherwise.
 * @details Output is initialized on every call. A decoded event occurs
 * once after a trailing quiet zone, with action UNKNOWN on every other
 * result. Reverse motion, stale input and overflow clear the frame and
 * require a fresh quiet zone. Quiet-zone/repeat distances, thresholds,
 * widths and timing limits must be confirmed for the actual course.
 * maximum_frame_m covers the first black edge through the final black edge;
 * it excludes leading/trailing quiet zones and edge-confirmation travel.
 * The default compares physical widths directly. Optional b_scale_tolerant
 * fits one observed/template total-width ratio in [minimum_scale,
 * maximum_scale], then requires every run to match that scaled template.
 * Scale bounds must lie in [BARCODE_MIN_SCALE, BARCODE_MAX_SCALE]; all
 * scaled template widths must remain within the physical acquisition limits.
 * Absolute/relative tolerances apply to each scaled run, not to scale bounds.
 * This does not recover unobserved runs or relax maximum_step_m, quiet zones,
 * minimum_run_m, framing, direction, or the two confirmation gates.
 * Call from one owner; derive again and reset if the configuration changes.
 */
bool barcode_step (const barcode_runtime_t * p_runtime,
                   barcode_state_t * p_state, line_ctrl_class_t darkness,
                   uint32_t position_hc, uint32_t now_ms,
                   barcode_output_t * p_output);

#endif /* BARCODE_H */

/* End of file. */


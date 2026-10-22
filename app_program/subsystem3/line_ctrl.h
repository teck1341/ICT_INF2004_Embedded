/**
 * @file line_ctrl.h
 * @brief Two line-edge channels plus a calibrated sidebarcode channel.
 * @details ADC units are counts; darkness is 0 white to 1 black. This
 * module assumes the physical optical arrangement has been verified.
 * The authored configuration keeps doubles (proposed project BARR-C 5.4
 * deviation) that line_ctrl_derive() reads once; every per-sample decision
 * is an exact integer compare on 12-bit ADC counts.
 */
#ifndef LINE_CTRL_H
#define LINE_CTRL_H

#include <stdbool.h>
#include <stdint.h>

#define LINE_CTRL_CHANNELS        (3U)
#define LINE_CTRL_BARCODE_CHANNEL (2U)
#define LINE_CTRL_ADC_MAX         (4095U)
/** darkness_q12 of a full-black sample (darkness 1.0). */
#define LINE_CTRL_DARKNESS_ONE    (4095U)
/** steering_q7 of full right steering: steering_q7 / 127 is the steering. */
#define LINE_CTRL_STEERING_ONE    (127)
/** line_ctrl_quotient() results stay below 2^26 (exactness bound). */
#define LINE_CTRL_QUOTIENT_LIMIT  (67108864U)

/** Line evidence; codes 6 and 7 of the 3-bit field are never written. */
typedef enum
{
    LINE_CTRL_INHIBITED = 0,
    LINE_CTRL_TRACKING,
    LINE_CTRL_SEARCHING,
    LINE_CTRL_JUNCTION,
    LINE_CTRL_LOST,
    LINE_CTRL_FAULT
} line_ctrl_status_t;

/** 2-bit class of one ADC sample against an exact hysteresis pair. */
typedef enum
{
    LINE_CTRL_LIGHT = 0,  /* darkness <= black_off */
    LINE_CTRL_BAND,       /* between the thresholds: keep the last class */
    LINE_CTRL_DARK,       /* darkness >= black_on */
    LINE_CTRL_INVALID     /* outside the 12-bit ADC (or configured) range */
} line_ctrl_class_t;

/** Authored calibration, validated and converted once by line_ctrl_derive. */
typedef struct
{
    uint16_t white_adc[LINE_CTRL_CHANNELS];
    uint16_t black_adc[LINE_CTRL_CHANNELS];
    uint16_t minimum_contrast_adc;
    uint16_t maximum_adc;
    double   black_on;
    double   black_off;
    double   minimum_signal;
    uint32_t junction_debounce_ms;
    uint32_t lost_timeout_ms;
    uint32_t maximum_gap_ms;
    bool     b_calibration_confirmed;
} line_ctrl_config_t;

/** Exact ADC hysteresis, 25 bits. With b_dark_high, raw >= on_adc is DARK
 * and raw <= off_adc is LIGHT; otherwise raw <= on_adc is DARK and raw >=
 * off_adc is LIGHT. Equal to the legacy double darkness compares for every
 * raw value 0..4095 (found by binary search over those compares).
 */
typedef struct
{
    uint32_t on_adc      : 12;
    uint32_t off_adc     : 12;
    bool     b_dark_high : 1;
} line_ctrl_hysteresis_t;

/** One derived channel, 49 bits: hysteresis, white level and the span
 * |black - white| that scale darkness and steering.
 */
typedef struct
{
    line_ctrl_hysteresis_t hysteresis;
    uint32_t               white_adc : 12;
    uint32_t               span_adc  : 12;
} line_ctrl_channel_t;

/** Runtime form of a valid configuration, derived once. signal_threshold
 * T = ceil(minimum_signal * S0 * S1) in exact arithmetic (24 bits, since
 * minimum_signal < 1), compared with N = n0 * S1 + n1 * S0, where n is the
 * clamped darkness numerator and S the span of channels 0 and 1. Intervals
 * are 1..60000 ms; timestamps stay 32-bit with wrap-safe subtraction.
 */
typedef struct
{
    line_ctrl_channel_t channel[LINE_CTRL_CHANNELS];
    uint32_t            signal_threshold        : 24;
    bool                b_calibration_confirmed : 1;
    bool                b_valid                 : 1;
    uint32_t            maximum_adc             : 12;
    uint16_t            junction_debounce_ms;
    uint16_t            lost_timeout_ms;
    uint16_t            maximum_gap_ms;
} line_ctrl_runtime_t;

/** LineTask-private history; bit c of black_mask is channel c black. */
typedef struct
{
    uint32_t previous_ms;
    uint32_t lost_since_ms;
    uint32_t junction_since_ms;
    uint32_t black_mask         : 3;
    bool     b_started          : 1;
    bool     b_losing           : 1;
    bool     b_junction_pending : 1;
    bool     b_junction_latched : 1;
} line_ctrl_state_t;

/** Per-sample result. darkness_q12 (0..4095 = 0..1, nearest) is for
 * diagnostics only and is never a compare operand. steering_q7 is
 * -127..127 (half away from zero, 0 below the minimum signal).
 */
typedef struct
{
    uint16_t           darkness_q12[LINE_CTRL_CHANNELS];
    int8_t             steering_q7;
    line_ctrl_status_t status          : 3;
    bool               b_junction_edge : 1;
    bool               b_barcode_black : 1;
} line_ctrl_output_t;

/** @brief Set unconfirmed illustrative thresholds; NULL is ignored. */
void line_ctrl_defaults (line_ctrl_config_t * p_config);

/** @brief Validate structure/ranges; confirmation is a separate gate. */
bool line_ctrl_config_valid (const line_ctrl_config_t * p_config);

/**
 * @brief Exact floor (b_ceil false) or ceiling of numerator / unit.
 * @return False for NULL, a non-finite or negative numerator, a unit that
 * is not finite and positive, or a result of LINE_CTRL_QUOTIENT_LIMIT or
 * more. The result is the exact rational floor/ceiling of the two doubles,
 * so integer decisions W < ceil(x / q) and W > floor(x / q) equal the real
 * decisions W * q < x and W * q > x for every integer W.
 */
bool line_ctrl_quotient (double numerator, double unit, bool b_ceil,
                         uint32_t * p_result);

/**
 * @brief Exact ADC hysteresis of one channel for a black_on/black_off pair.
 * @return False (output zeroed) unless white and black differ and are at
 * most 4095 and 0 <= black_off < black_on <= 1 are finite.
 */
bool line_ctrl_hysteresis (uint16_t white_adc, uint16_t black_adc,
                           double black_on, double black_off,
                           line_ctrl_hysteresis_t * p_hysteresis);

/** @brief Classify raw; INVALID for NULL or raw above 4095. */
line_ctrl_class_t line_ctrl_classify (
    const line_ctrl_hysteresis_t * p_hysteresis, uint16_t raw);

/**
 * @brief Derive integer thresholds once from an authored configuration.
 * @return False, with an invalid (zeroed) runtime, for NULL, a
 * configuration rejected by line_ctrl_config_valid() or maximum_adc above
 * 4095 (the 12-bit ADC).
 */
bool line_ctrl_derive (const line_ctrl_config_t * p_config,
                       line_ctrl_runtime_t * p_runtime);

/** @brief Clear history; NULL is ignored. Call before the first sample. */
void line_ctrl_init (line_ctrl_state_t * p_state);

/**
 * @brief Exact minimum-signal test and Q7 steering of channels 0 and 1.
 * @return True when the signal reaches minimum_signal; *p_steering_q7 is
 * then round(127 * (d1 - d0) / (d0 + d1)), half away from zero, else 0.
 * False also for NULL, an invalid runtime or raw above maximum_adc.
 */
bool line_ctrl_steer (const line_ctrl_runtime_t * p_runtime,
                      const uint16_t * p_raw, int8_t * p_steering_q7);

/**
 * @brief Process a calibrated sample without allocation.
 * @param now_ms Monotonic wrapping milliseconds; gap must stay bounded.
 * @param p_raw Three ADC counts: left, right, sidebarcode.
 * @param p_output Positive steering means line evidence to the right.
 * @return False for NULL, an invalid runtime, ADC or sample timing.
 * @details Valid but unconfirmed calibration returns true and INHIBITED.
 * A fault clears history. SEARCHING is uncertain and is not permission
 * to drive. JUNCTION is ambiguous both-black evidence, not a turn order.
 */
bool line_ctrl_step (const line_ctrl_runtime_t * p_runtime,
                     line_ctrl_state_t * p_state,
                     const uint16_t * p_raw, uint32_t now_ms,
                     line_ctrl_output_t * p_output);

#endif /* LINE_CTRL_H */

/* End of file. */


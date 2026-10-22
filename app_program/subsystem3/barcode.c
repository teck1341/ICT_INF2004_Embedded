/**
 * @file barcode.c
 * @brief Bounded distance-domain run acquisition and explicit codebook match.
 * @details Positions and widths are integer half-counts compared with
 * thresholds that barcode_derive() converted exactly. Frame-end matching
 * reads the constant authored table and applies the removed per-run
 * predicate, in doubles, to each stored width in metres (W * q).
 */
#include "barcode.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#if DBL_MANT_DIG < 53
#error "barcode requires at least 53 double mantissa bits"
#endif

#define BARCODE_RUN_MASK     (0x3FU)
#define BARCODE_SYMBOL_MASK  (0xFU)
#define BARCODE_STEP_MASK    (0x3FU)
#define BARCODE_BYTE_MASK    (0xFFU)
#define BARCODE_PERSIST_MASK (0x7FU)
#define BARCODE_RUN_HC_MASK  (0x1FFU)
#define BARCODE_ADC_MASK     (0xFFFU)
#define BARCODE_GAP_MASK     (0xFFFFU)
#define BARCODE_TIMEOUT_MASK (0x3FFFFFU)
#define BARCODE_LONG_MASK    (0x3FFFFFFU)
#define BARCODE_HALF_RANGE   (0x80000000U)

/* Unconfirmed illustrative limits; production reads course_cfg instead. */
static const barcode_config_t g_barcode_defaults = { BARCODE_DEFAULT_LIMITS };

static bool barcode_limits_valid (const barcode_config_t * p_config);
static bool barcode_derive_distances (const barcode_config_t * p_config,
                                      double unit_m,
                                      barcode_runtime_t * p_runtime);
static int32_t barcode_delta (uint32_t newer, uint32_t older);
static void barcode_reject (barcode_state_t * p_state,
                            barcode_reason_t reason,
                            barcode_output_t * p_output);
static void barcode_transition (const barcode_runtime_t * p_runtime,
                                barcode_state_t * p_state,
                                uint32_t now_ms,
                                barcode_output_t * p_output);
static void barcode_acquire (const barcode_runtime_t * p_runtime,
                             barcode_state_t * p_state,
                             line_ctrl_class_t darkness, uint32_t now_ms,
                             barcode_output_t * p_output);
static void barcode_finish (const barcode_runtime_t * p_runtime,
                            barcode_state_t * p_state,
                            barcode_output_t * p_output);
static uint32_t barcode_canonical (const barcode_config_t * p_config,
                                   uint32_t symbol);
static bool barcode_symbol_matches (const barcode_runtime_t * p_runtime,
                                    const barcode_state_t * p_state,
                                    const barcode_symbol_t * p_symbol);

/** @brief Defaults are engineering hypotheses and do not enable decoding. */
void barcode_defaults (barcode_config_t * p_config)
{
    if (NULL != p_config)
    {
        *p_config = g_barcode_defaults;
    }
}

/** @brief Reject malformed, impossible or non-finite configured patterns. */
bool barcode_config_valid (const barcode_config_t * p_config)
{
    uint32_t symbol;
    uint32_t run;
    double   total;
    double   minimum_scale;
    double   maximum_scale;
    const barcode_symbol_t * p_entry;

    if ((NULL == p_config) || (!barcode_limits_valid(p_config)))
    {
        return false;
    }
    minimum_scale = p_config->b_scale_tolerant ? p_config->minimum_scale : 1.0;
    maximum_scale = p_config->b_scale_tolerant ? p_config->maximum_scale : 1.0;
    for (symbol = 0U; symbol < p_config->symbol_count; ++symbol)
    {
        p_entry = &p_config->symbols[symbol];
        if ((0U == p_entry->run_count) ||
            (p_entry->run_count > BARCODE_MAX_RUNS) ||
            (0U == (p_entry->run_count % 2U)) ||
            ('\0' == p_entry->label) ||
            (p_entry->action < BARCODE_ACTION_LEFT) ||
            (p_entry->action > BARCODE_ACTION_UTURN))
        {
            return false;
        }
        total = 0.0;
        for (run = 0U; run < p_entry->run_count; ++run)
        {
            if ((!isfinite(p_entry->width_m[run])) ||
                (p_entry->width_m[run] < p_config->minimum_run_m) ||
                (p_entry->width_m[run] > p_config->maximum_run_m) ||
                ((p_entry->width_m[run] * minimum_scale) <
                 p_config->minimum_run_m) ||
                ((p_entry->width_m[run] * maximum_scale) >
                 p_config->maximum_run_m) ||
                ((1U == (run % 2U)) &&
                 ((p_entry->width_m[run] * maximum_scale) >=
                  p_config->quiet_zone_m)))
            {
                return false;
            }
            total += p_entry->width_m[run];
        }
        if ((!isfinite(total)) ||
            ((total * maximum_scale) > p_config->maximum_frame_m))
        {
            return false;
        }
    }
    return true;
}

/** @brief Validate once, then keep only exact integer thresholds. */
bool barcode_derive (const barcode_config_t * p_config, double unit_m,
                     const line_ctrl_config_t * p_line,
                     barcode_runtime_t * p_runtime)
{
    if (NULL == p_runtime)
    {
        return false;
    }
    memset(p_runtime, 0, sizeof(*p_runtime));
    if ((!barcode_config_valid(p_config)) ||
        (!line_ctrl_config_valid(p_line)) ||
        (p_line->maximum_adc > LINE_CTRL_ADC_MAX) ||
        (!isfinite(unit_m)) || (unit_m <= 0.0) ||
        (!line_ctrl_hysteresis(p_line->white_adc[LINE_CTRL_BARCODE_CHANNEL],
                               p_line->black_adc[LINE_CTRL_BARCODE_CHANNEL],
                               p_config->black_on, p_config->black_off,
                               &p_runtime->hysteresis)) ||
        (!barcode_derive_distances(p_config, unit_m, p_runtime)))
    {
        memset(p_runtime, 0, sizeof(*p_runtime));
        return false;
    }
    p_runtime->unit_m     = unit_m;
    p_runtime->p_codebook = p_config;
    /* Validated: timeout <= 3,600,000 < 2^22, gap <= 60000, ADC <= 4095. */
    p_runtime->frame_timeout_ms =
        p_config->frame_timeout_ms & BARCODE_TIMEOUT_MASK;
    p_runtime->maximum_gap_ms = p_config->maximum_gap_ms & BARCODE_GAP_MASK;
    p_runtime->maximum_adc = p_line->maximum_adc & BARCODE_ADC_MASK;
    p_runtime->b_enabled = (p_config->b_calibration_confirmed) &&
                           (p_config->b_codebook_confirmed) &&
                           (0U != p_config->symbol_count);
    p_runtime->b_scale_tolerant = p_config->b_scale_tolerant;
    p_runtime->b_line_confirmed = p_line->b_calibration_confirmed;
    p_runtime->b_valid = true;
    return true;
}

/** @brief Exact class of a sidebarcode sample; see the header contract. */
line_ctrl_class_t barcode_classify (const barcode_runtime_t * p_runtime,
                                    uint16_t raw)
{
    uint32_t value = raw;
    uint32_t maximum;

    if ((NULL == p_runtime) || (!p_runtime->b_valid))
    {
        return LINE_CTRL_INVALID;
    }
    maximum = p_runtime->maximum_adc;
    if (value > maximum)
    {
        return LINE_CTRL_INVALID;
    }
    if (!p_runtime->b_line_confirmed)
    {
        /* line_ctrl reported all-zero darkness while INHIBITED. */
        return LINE_CTRL_LIGHT;
    }
    return line_ctrl_classify(&p_runtime->hysteresis, raw);
}

/** @brief Clear fixed-size state; no heap or global ownership is needed. */
void barcode_init (barcode_state_t * p_state)
{
    if (p_state != NULL)
    {
        memset(p_state, 0, sizeof(*p_state));
    }
}

/** @brief Validate and clock one spatial acquisition sample. */
bool barcode_step (const barcode_runtime_t * p_runtime,
                   barcode_state_t * p_state, line_ctrl_class_t darkness,
                   uint32_t position_hc, uint32_t now_ms,
                   barcode_output_t * p_output)
{
    int32_t advance;

    if (NULL == p_output)
    {
        return false;
    }
    memset(p_output, 0, sizeof(*p_output));
    if ((NULL == p_state) || (NULL == p_runtime) || (!p_runtime->b_valid) ||
        ((LINE_CTRL_LIGHT != darkness) && (LINE_CTRL_BAND != darkness) &&
         (LINE_CTRL_DARK != darkness)))
    {
        barcode_init(p_state);
        p_output->event  = BARCODE_EVENT_REJECTED;
        p_output->reason = BARCODE_REASON_INPUT;
        return false;
    }
    if (!p_runtime->b_enabled)
    {
        barcode_init(p_state);
        p_output->event = BARCODE_EVENT_INHIBITED;
        return true;
    }
    if (!p_state->b_started)
    {
        p_state->b_started       = true;
        p_state->high_water_hc   = position_hc;
        p_state->run_start_hc    = position_hc;
        p_state->previous_ms     = now_ms;
        p_state->b_current_black = (LINE_CTRL_DARK == darkness);
    }
    advance = barcode_delta(position_hc, p_state->high_water_hc);
    if ((advance > (int32_t) p_runtime->maximum_step_hc) ||
        (advance < -(int32_t) p_runtime->reverse_hc))
    {
        barcode_reject(p_state,
            (advance < -(int32_t) p_runtime->reverse_hc) ?
            BARCODE_REASON_REVERSE : BARCODE_REASON_DISTANCE, p_output);
        p_state->b_started = false;
        return true;
    }
    if (((now_ms - p_state->previous_ms) > p_runtime->maximum_gap_ms) ||
        ((p_state->b_frame) && ((now_ms - p_state->frame_start_ms) >
                              p_runtime->frame_timeout_ms)))
    {
        barcode_reject(p_state, BARCODE_REASON_TIMEOUT, p_output);
        p_state->b_started = false;
        return true;
    }
    p_state->previous_ms = now_ms;
    if (advance > 0)
    {
        p_state->high_water_hc = position_hc;
    }
    barcode_acquire(p_runtime, p_state, darkness, now_ms, p_output);
    return true;
}

/** @brief Bound every numeric setting to avoid invalid state arithmetic. */
static bool barcode_limits_valid (const barcode_config_t * p_config)
{
    const double limits[] =
    {
        p_config->black_on, p_config->black_off,
        p_config->minimum_run_m, p_config->maximum_run_m,
        p_config->quiet_zone_m, p_config->maximum_frame_m,
        p_config->maximum_step_m, p_config->reverse_tolerance_m,
        p_config->absolute_tolerance_m, p_config->relative_tolerance,
        p_config->duplicate_distance_m
    };
    size_t index;
    const size_t count = sizeof(limits) / sizeof(limits[0]);

    for (index = 0U; index < count; ++index)
    {
        if ((!isfinite(limits[index])) || (limits[index] < 0.0))
        {
            return false;
        }
    }
    if ((p_config->b_scale_tolerant) &&
        ((!isfinite(p_config->minimum_scale)) ||
         (!isfinite(p_config->maximum_scale)) ||
         (p_config->minimum_scale < BARCODE_MIN_SCALE) ||
         (p_config->maximum_scale > BARCODE_MAX_SCALE) ||
         (p_config->minimum_scale > p_config->maximum_scale)))
    {
        return false;
    }
    return (p_config->black_on <= 1.0) &&
           (p_config->black_off < p_config->black_on) &&
           (p_config->minimum_run_m > 0.0) &&
           (p_config->minimum_run_m < p_config->quiet_zone_m) &&
           (p_config->maximum_run_m >= p_config->minimum_run_m) &&
           (p_config->maximum_run_m < p_config->maximum_frame_m) &&
           (p_config->maximum_frame_m <= 100.0) &&
           (p_config->quiet_zone_m < p_config->maximum_frame_m) &&
           (p_config->maximum_step_m > 0.0) &&
           (p_config->maximum_step_m <=
            (0.20 * p_config->minimum_run_m)) &&
           (p_config->reverse_tolerance_m < p_config->minimum_run_m) &&
           (p_config->absolute_tolerance_m < p_config->minimum_run_m) &&
           (p_config->relative_tolerance < 0.5) &&
           (p_config->duplicate_distance_m <= 100.0) &&
           (p_config->frame_timeout_ms > 0U) &&
           (p_config->frame_timeout_ms <= 3600000U) &&
           (p_config->maximum_gap_ms > 0U) &&
           (p_config->maximum_gap_ms <= 60000U) &&
           (p_config->symbol_count <= BARCODE_MAX_SYMBOLS);
}

/**
 * @brief Exact half-count threshold for each metre limit (see barcode.h).
 * @details A valid configuration has maximum run < 256 half-counts once
 * the uint8_t gate passes, so step <= 0.2 * minimum run gives step <= 51,
 * reverse < minimum run <= 256, and persistence = 0.25 * minimum run
 * <= 64; the width checks below only guard the masks.
 */
static bool barcode_derive_distances (const barcode_config_t * p_config,
                                      double unit_m,
                                      barcode_runtime_t * p_runtime)
{
    uint32_t step = 0U;
    uint32_t reverse = 0U;
    uint32_t minimum = 0U;
    uint32_t maximum = 0U;
    uint32_t persistence = 0U;
    uint32_t quiet = 0U;
    uint32_t frame = 0U;
    uint32_t duplicate = 0U;
    bool     b_valid =
        line_ctrl_quotient(p_config->maximum_step_m, unit_m, false, &step) &&
        line_ctrl_quotient(p_config->reverse_tolerance_m, unit_m, false,
                           &reverse) &&
        line_ctrl_quotient(p_config->minimum_run_m, unit_m, true,
                           &minimum) &&
        line_ctrl_quotient(p_config->maximum_run_m, unit_m, false,
                           &maximum) &&
        /* Scaling by 0.25 is exact, as in the legacy compare. */
        line_ctrl_quotient(0.25 * p_config->minimum_run_m, unit_m, true,
                           &persistence) &&
        line_ctrl_quotient(p_config->quiet_zone_m, unit_m, true, &quiet) &&
        line_ctrl_quotient(p_config->maximum_frame_m, unit_m, false,
                           &frame) &&
        line_ctrl_quotient(p_config->duplicate_distance_m, unit_m, true,
                           &duplicate);

    if ((!b_valid) || (maximum > BARCODE_MAX_WIDTH_HC) ||
        (step > BARCODE_STEP_MASK) || (reverse > BARCODE_BYTE_MASK) ||
        (minimum > BARCODE_RUN_HC_MASK) ||
        (persistence > BARCODE_PERSIST_MASK))
    {
        return false;
    }
    /* Quotients are below 2^26, so the 26-bit masks keep every value. */
    p_runtime->maximum_step_hc  = step & BARCODE_STEP_MASK;
    p_runtime->reverse_hc       = reverse & BARCODE_BYTE_MASK;
    p_runtime->minimum_run_hc   = minimum & BARCODE_RUN_HC_MASK;
    p_runtime->maximum_run_hc   = maximum & BARCODE_BYTE_MASK;
    p_runtime->persistence_hc   = persistence & BARCODE_PERSIST_MASK;
    p_runtime->quiet_zone_hc    = quiet & BARCODE_LONG_MASK;
    p_runtime->maximum_frame_hc = frame & BARCODE_LONG_MASK;
    p_runtime->duplicate_hc     = duplicate & BARCODE_LONG_MASK;
    return true;
}

/** @brief Signed modulo-2^32 difference newer - older as int32_t. */
static int32_t barcode_delta (uint32_t newer, uint32_t older)
{
    uint32_t difference = newer - older;

    /* Two's complement reading without an implementation-defined cast:
     * ~difference is below 2^31 in the negative branch.
     */
    return (difference < BARCODE_HALF_RANGE) ? (int32_t) difference
                                             : (-(int32_t) ~difference - 1);
}

/** @brief Discard uncertain frame while retaining duplicate history. */
static void barcode_reject (barcode_state_t * p_state,
                            barcode_reason_t reason,
                            barcode_output_t * p_output)
{
    p_output->event      = BARCODE_EVENT_REJECTED;
    p_output->reason     = reason;
    p_output->run_count  = p_state->run_count;
    p_state->b_frame     = false;
    p_state->b_armed     = false;
    p_state->b_candidate = false;
    p_state->run_count   = 0U;
    p_state->run_start_hc = p_state->high_water_hc;
}

/** @brief Commit an edge only after one-quarter minimum-run persistence. */
static void barcode_transition (const barcode_runtime_t * p_runtime,
                                barcode_state_t * p_state,
                                uint32_t now_ms,
                                barcode_output_t * p_output)
{
    int32_t width = barcode_delta(p_state->candidate_start_hc,
                                  p_state->run_start_hc);

    if (p_state->b_frame)
    {
        if (p_state->run_count >= BARCODE_MAX_RUNS)
        {
            barcode_reject(p_state, BARCODE_REASON_OVERFLOW, p_output);
        }
        else if ((width < (int32_t) p_runtime->minimum_run_hc) ||
                 (width > (int32_t) p_runtime->maximum_run_hc))
        {
            barcode_reject(p_state, BARCODE_REASON_WIDTH, p_output);
        }
        else
        {
            /* minimum <= width <= maximum <= 255: fits uint8_t. */
            p_state->widths_hc[p_state->run_count] = (uint8_t) width;
            p_state->run_count = (p_state->run_count + 1U) &
                                 BARCODE_RUN_MASK;
        }
    }
    else if ((p_state->b_candidate_black) &&
             ((p_state->b_armed) ||
              (width >= (int32_t) p_runtime->quiet_zone_hc)))
    {
        p_state->b_frame        = true;
        p_state->b_armed        = false;
        p_state->run_count      = 0U;
        p_state->frame_start_hc = p_state->candidate_start_hc;
        p_state->frame_start_ms = now_ms;
    }
    else
    {
        /* A black element without a leading quiet zone is ignored. */
    }
    p_state->b_current_black = p_state->b_candidate_black;
    p_state->b_candidate     = false;
    p_state->run_start_hc    = p_state->candidate_start_hc;
}

/** @brief Hysteretic edges reject short spatial noise; quiet white closes. */
static void barcode_acquire (const barcode_runtime_t * p_runtime,
                             barcode_state_t * p_state,
                             line_ctrl_class_t darkness, uint32_t now_ms,
                             barcode_output_t * p_output)
{
    bool     b_black = p_state->b_current_black;
    int32_t  span;
    uint32_t observed_end;

    if (LINE_CTRL_DARK == darkness)
    {
        b_black = true;
    }
    else if (LINE_CTRL_LIGHT == darkness)
    {
        b_black = false;
    }
    else
    {
        /* Keep the stable classification inside the hysteresis band. */
    }
    if (b_black != p_state->b_current_black)
    {
        if (!p_state->b_candidate)
        {
            p_state->b_candidate        = true;
            p_state->b_candidate_black  = b_black;
            p_state->candidate_start_hc = p_state->high_water_hc;
        }
        if (barcode_delta(p_state->high_water_hc,
                          p_state->candidate_start_hc) >=
            (int32_t) p_runtime->persistence_hc)
        {
            barcode_transition(p_runtime, p_state, now_ms, p_output);
        }
    }
    else
    {
        p_state->b_candidate = false;
    }
    /* Edge persistence and trailing quiet are outside the encoded width. */
    observed_end = p_state->b_candidate ? p_state->candidate_start_hc
                                        : p_state->high_water_hc;
    span = barcode_delta(observed_end, p_state->run_start_hc);
    if (!p_state->b_current_black)
    {
        observed_end = p_state->run_start_hc;
    }
    if ((p_state->b_frame) &&
        (barcode_delta(observed_end, p_state->frame_start_hc) >
         (int32_t) p_runtime->maximum_frame_hc))
    {
        barcode_reject(p_state, BARCODE_REASON_OVERFLOW, p_output);
    }
    else if ((!p_state->b_current_black) && (!p_state->b_candidate) &&
             (span >= (int32_t) p_runtime->quiet_zone_hc))
    {
        if (p_state->b_frame)
        {
            barcode_finish(p_runtime, p_state, p_output);
        }
        p_state->b_armed = true;
    }
    else if ((p_state->b_frame) && (p_state->b_current_black) &&
             (span > (int32_t) p_runtime->maximum_run_hc))
    {
        barcode_reject(p_state, BARCODE_REASON_WIDTH, p_output);
    }
    else
    {
        /* Incomplete frames remain pending until bounded termination. */
    }
}

/** @brief Require a unique matching configured pattern and reject repeats. */
static void barcode_finish (const barcode_runtime_t * p_runtime,
                            barcode_state_t * p_state,
                            barcode_output_t * p_output)
{
    const barcode_config_t * p_config = p_runtime->p_codebook;
    uint32_t symbol;
    uint32_t matches = 0U;
    uint32_t match = 0U;
    uint32_t canonical;
    uint32_t last_symbol = p_state->last_symbol;
    int32_t  separation;
    int32_t  duplicate = (int32_t) p_runtime->duplicate_hc;

    p_output->run_count = p_state->run_count;
    p_output->event     = BARCODE_EVENT_UNKNOWN;
    p_output->reason    = BARCODE_REASON_NO_MATCH;
    for (symbol = 0U; symbol < p_config->symbol_count; ++symbol)
    {
        if (barcode_symbol_matches(p_runtime, p_state,
                                   &p_config->symbols[symbol]))
        {
            ++matches;
            match = symbol;
        }
    }
    if (matches > 1U)
    {
        p_output->reason = BARCODE_REASON_AMBIGUOUS;
    }
    else if (1U == matches)
    {
        /* Labels compare by canonical index, so duplicate characters in
         * the codebook stay duplicates exactly as the characters did.
         */
        canonical = barcode_canonical(p_config, match);
        separation = barcode_delta(p_state->run_start_hc,
                                   p_state->last_decode_hc);
        if ((p_state->b_last_decode) && (canonical == last_symbol) &&
            (separation < duplicate) && (separation > -duplicate))
        {
            p_output->event  = BARCODE_EVENT_REJECTED;
            p_output->reason = BARCODE_REASON_DUPLICATE;
        }
        else
        {
            p_output->event        = BARCODE_EVENT_DECODED;
            p_output->reason       = BARCODE_REASON_NONE;
            p_output->action       = p_config->symbols[match].action;
            p_output->symbol       = canonical & BARCODE_SYMBOL_MASK;
            p_state->last_symbol   = canonical & BARCODE_SYMBOL_MASK;
            p_state->last_decode_hc = p_state->run_start_hc;
            p_state->b_last_decode = true;
        }
    }
    else
    {
        /* An unmatched pattern never acquires a motion action. */
    }
    p_state->b_frame   = false;
    p_state->run_count = 0U;
}

/** @brief Lowest codebook index whose label equals that of symbol. */
static uint32_t barcode_canonical (const barcode_config_t * p_config,
                                   uint32_t symbol)
{
    uint32_t index = 0U;

    while ((index < symbol) &&
           (p_config->symbols[index].label != p_config->symbols[symbol].label))
    {
        ++index;
    }
    return index;
}

/**
 * @brief The removed per-run match, with each stored width read in metres.
 * @details Same operations in the same order as the removed double code,
 * the double width replaced by fl(W * q). Strict mode keeps scale 1.0;
 * scale mode first fits one bounded observed/template ratio. Rounding
 * E - T or E + T into an integer window would add a rounding the removed
 * predicate never had.
 */
static bool barcode_symbol_matches (const barcode_runtime_t * p_runtime,
                                    const barcode_state_t * p_state,
                                    const barcode_symbol_t * p_symbol)
{
    const barcode_config_t * p_config = p_runtime->p_codebook;
    uint32_t run;
    double   observed;
    double   expected;
    double   tolerance;
    double   scale = 1.0;
    double   observed_total = 0.0;
    double   template_total = 0.0;

    if (p_symbol->run_count != p_state->run_count)
    {
        return false;
    }
    if (p_runtime->b_scale_tolerant)
    {
        for (run = 0U; run < p_symbol->run_count; ++run)
        {
            /* Widths are at most 255 and exact in double. */
            observed_total += (double) p_state->widths_hc[run] *
                              p_runtime->unit_m;
            template_total += p_symbol->width_m[run];
        }
        scale = observed_total / template_total;
        if ((!isfinite(scale)) || (scale < p_config->minimum_scale) ||
            (scale > p_config->maximum_scale))
        {
            return false;
        }
    }
    for (run = 0U; run < p_symbol->run_count; ++run)
    {
        observed  = (double) p_state->widths_hc[run] * p_runtime->unit_m;
        expected  = scale * p_symbol->width_m[run];
        tolerance = p_config->absolute_tolerance_m +
                    (p_config->relative_tolerance * expected);
        if (fabs(observed - expected) > tolerance)
        {
            return false;
        }
    }
    return true;
}

/* End of file. */


/**
 * @file line_ctrl.c
 * @brief Calibrated optical evidence and bounded event timing.
 * @details line_ctrl_derive() turns the authored doubles into exact integer
 * thresholds once; line_ctrl_step() then uses integer arithmetic only.
 */
#include "line_ctrl.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#if (DBL_MANT_DIG != 53) || (FLT_EVAL_METHOD != 0)
#error "line_ctrl exact thresholds need binary64 without extra precision"
#endif

/* Veltkamp split factor 2^27 + 1: halves of at most 26 significant bits. */
#define LINE_CTRL_SPLIT         (134217729.0)
#define LINE_CTRL_ADC_COUNT     (4096U)
#define LINE_CTRL_ADC_MASK      (0xFFFU)
#define LINE_CTRL_SIGNAL_MASK   (0xFFFFFFU)
#define LINE_CTRL_CHANNEL_MASK  (0x7U)
#define LINE_CTRL_EDGE_MASK     (0x3U)
/* 2 * 127: half-away rounding of 127 * |num| / N as (254|num| + N) / 2N. */
#define LINE_CTRL_STEERING_TWICE (254U)

static int32_t line_ctrl_sign (uint32_t count, double value,
                               double reference);
static uint32_t line_ctrl_ceil_product (double fraction, uint32_t count);
static double line_ctrl_clamp (double value);
static double line_ctrl_darkness (uint32_t raw, uint16_t white_adc,
                                  uint16_t black_adc);
static uint32_t line_ctrl_boundary (uint16_t white_adc, uint16_t black_adc,
                                    double threshold, bool b_on,
                                    bool b_holds);
static uint32_t line_ctrl_numerator (const line_ctrl_channel_t * p_channel,
                                     uint16_t raw);
static void line_ctrl_lost (const line_ctrl_runtime_t * p_runtime,
                            line_ctrl_state_t * p_state, uint32_t now_ms,
                            bool b_signal, line_ctrl_output_t * p_output);
static void line_ctrl_events (const line_ctrl_runtime_t * p_runtime,
                              line_ctrl_state_t * p_state,
                              uint32_t now_ms, bool b_signal,
                              line_ctrl_output_t * p_output);

/** @brief Set proposed thresholds; all calibration starts unconfirmed. */
void line_ctrl_defaults (line_ctrl_config_t * p_config)
{
    uint32_t channel;

    if (NULL == p_config)
    {
        return;
    }
    memset(p_config, 0, sizeof(*p_config));
    for (channel = 0U; channel < LINE_CTRL_CHANNELS; ++channel)
    {
        p_config->white_adc[channel] = 4095U;
        p_config->black_adc[channel] = 0U;
    }
    p_config->minimum_contrast_adc = 200U;
    p_config->maximum_adc          = 4095U;
    p_config->black_on             = 0.75;
    p_config->black_off            = 0.55;
    p_config->minimum_signal       = 0.15;
    p_config->junction_debounce_ms = 20U;
    p_config->lost_timeout_ms      = 100U;
    p_config->maximum_gap_ms       = 50U;
}

/** @brief Validate finite thresholds and all three calibration spans. */
bool line_ctrl_config_valid (const line_ctrl_config_t * p_config)
{
    uint32_t channel;
    double   contrast;

    if (NULL == p_config)
    {
        return false;
    }
    if ((0U == p_config->minimum_contrast_adc) ||
        (0U == p_config->maximum_adc) ||
        (0U == p_config->junction_debounce_ms) ||
        (0U == p_config->lost_timeout_ms) ||
        (0U == p_config->maximum_gap_ms) ||
        (p_config->maximum_gap_ms > 60000U) ||
        (p_config->lost_timeout_ms > 60000U) ||
        (p_config->junction_debounce_ms > 60000U) ||
        (!isfinite(p_config->black_on)) ||
        (!isfinite(p_config->black_off)) ||
        (!isfinite(p_config->minimum_signal)))
    {
        return false;
    }
    if ((p_config->black_on > 1.0) ||
        (p_config->black_off < 0.0) ||
        (p_config->black_off >= p_config->black_on) ||
        (p_config->minimum_signal <= 0.0) ||
        (p_config->minimum_signal >= 1.0))
    {
        return false;
    }
    for (channel = 0U; channel < LINE_CTRL_CHANNELS; ++channel)
    {
        if ((p_config->white_adc[channel] > p_config->maximum_adc) ||
            (p_config->black_adc[channel] > p_config->maximum_adc))
        {
            return false;
        }
        /* uint16_t values are represented exactly by double. */
        contrast = (double) p_config->black_adc[channel] -
                   (double) p_config->white_adc[channel];
        if (fabs(contrast) < p_config->minimum_contrast_adc)
        {
            return false;
        }
    }
    return true;
}

/** @brief Exact rational floor/ceiling of two doubles; see the header. */
bool line_ctrl_quotient (double numerator, double unit, bool b_ceil,
                         uint32_t * p_result)
{
    double  ratio;
    double  whole;
    int32_t side;

    if ((NULL == p_result) || (!isfinite(numerator)) ||
        (!isfinite(unit)) || (numerator < 0.0) || (unit <= 0.0))
    {
        return false;
    }
    ratio = numerator / unit;
    whole = b_ceil ? ceil(ratio) : floor(ratio);
    if ((0.0 == ratio) && (numerator > 0.0))
    {
        /* An underflowed quotient lies strictly between 0 and 1. */
        whole = b_ceil ? 1.0 : 0.0;
    }
    else if ((whole == ratio) && (ratio > 0.0) &&
             (ratio < (double) LINE_CTRL_QUOTIENT_LIMIT))
    {
        /* Rounding is monotone and integers are doubles, so only a
         * quotient that rounded onto an integer can lie on its other side.
         * whole is an integer in [1, 2^26): the conversion is exact.
         */
        side = line_ctrl_sign((uint32_t) whole, unit, numerator);
        if (b_ceil && (side < 0))
        {
            whole += 1.0;
        }
        else if ((!b_ceil) && (side > 0))
        {
            whole -= 1.0;
        }
        else
        {
            /* The rounded integer is already the exact result. */
        }
    }
    else
    {
        /* A non-integer rounded quotient has the exact floor/ceiling. */
    }
    if (!(whole < (double) LINE_CTRL_QUOTIENT_LIMIT))
    {
        return false;
    }
    /* whole is an integer in [0, 2^26): the conversion is exact. */
    *p_result = (uint32_t) whole;
    return true;
}

/** @brief Exact hysteresis by binary search over the legacy compares. */
bool line_ctrl_hysteresis (uint16_t white_adc, uint16_t black_adc,
                           double black_on, double black_off,
                           line_ctrl_hysteresis_t * p_hysteresis)
{
    bool     b_dark_high = black_adc > white_adc;
    uint32_t on_adc;
    uint32_t off_adc;

    if (NULL == p_hysteresis)
    {
        return false;
    }
    memset(p_hysteresis, 0, sizeof(*p_hysteresis));
    if ((white_adc == black_adc) || (white_adc > LINE_CTRL_ADC_MAX) ||
        (black_adc > LINE_CTRL_ADC_MAX) || (!isfinite(black_on)) ||
        (!isfinite(black_off)) || (black_off < 0.0) ||
        (black_off >= black_on) || (black_on > 1.0))
    {
        return false;
    }
    /* darkness(black) is exactly 1.0 >= black_on and darkness(white) is 0
     * <= black_off, so each boundary exists strictly inside 0..4095 and
     * every result fits 12 bits.
     */
    if (b_dark_high)
    {
        on_adc = line_ctrl_boundary(white_adc, black_adc, black_on,
                                    true, true);
        off_adc = line_ctrl_boundary(white_adc, black_adc, black_off,
                                     false, false) - 1U;
    }
    else
    {
        on_adc = line_ctrl_boundary(white_adc, black_adc, black_on,
                                    true, false) - 1U;
        off_adc = line_ctrl_boundary(white_adc, black_adc, black_off,
                                     false, true);
    }
    p_hysteresis->on_adc      = on_adc & LINE_CTRL_ADC_MASK;
    p_hysteresis->off_adc     = off_adc & LINE_CTRL_ADC_MASK;
    p_hysteresis->b_dark_high = b_dark_high;
    return true;
}

/** @brief Integer form of the legacy darkness >= on / <= off compares. */
line_ctrl_class_t line_ctrl_classify (
    const line_ctrl_hysteresis_t * p_hysteresis, uint16_t raw)
{
    uint32_t          value = raw;
    uint32_t          on_adc;
    uint32_t          off_adc;
    line_ctrl_class_t result = LINE_CTRL_BAND;

    if ((NULL == p_hysteresis) || (value > LINE_CTRL_ADC_MAX))
    {
        return LINE_CTRL_INVALID;
    }
    on_adc  = p_hysteresis->on_adc;
    off_adc = p_hysteresis->off_adc;
    if (p_hysteresis->b_dark_high)
    {
        if (value >= on_adc)
        {
            result = LINE_CTRL_DARK;
        }
        else if (value <= off_adc)
        {
            result = LINE_CTRL_LIGHT;
        }
        else
        {
            /* Inside the band: the caller keeps its last class. */
        }
    }
    else if (value <= on_adc)
    {
        result = LINE_CTRL_DARK;
    }
    else if (value >= off_adc)
    {
        result = LINE_CTRL_LIGHT;
    }
    else
    {
        /* Inside the band: the caller keeps its last class. */
    }
    return result;
}

/** @brief Validate once, then convert every threshold to integers. */
bool line_ctrl_derive (const line_ctrl_config_t * p_config,
                       line_ctrl_runtime_t * p_runtime)
{
    uint32_t channel;
    uint32_t white;
    uint32_t black;
    uint32_t span[LINE_CTRL_CHANNELS];

    if (NULL == p_runtime)
    {
        return false;
    }
    memset(p_runtime, 0, sizeof(*p_runtime));
    if ((!line_ctrl_config_valid(p_config)) ||
        (p_config->maximum_adc > LINE_CTRL_ADC_MAX))
    {
        return false;
    }
    for (channel = 0U; channel < LINE_CTRL_CHANNELS; ++channel)
    {
        white = p_config->white_adc[channel];
        black = p_config->black_adc[channel];
        if (!line_ctrl_hysteresis(p_config->white_adc[channel],
                                  p_config->black_adc[channel],
                                  p_config->black_on, p_config->black_off,
                                  &p_runtime->channel[channel].hysteresis))
        {
            memset(p_runtime, 0, sizeof(*p_runtime));
            return false;
        }
        span[channel] = (black > white) ? (black - white) : (white - black);
        p_runtime->channel[channel].white_adc = white & LINE_CTRL_ADC_MASK;
        p_runtime->channel[channel].span_adc  =
            span[channel] & LINE_CTRL_ADC_MASK;
    }
    /* minimum_signal < 1, so T <= S0 * S1 <= 4095^2 < 2^24. */
    p_runtime->signal_threshold = line_ctrl_ceil_product(
        p_config->minimum_signal, span[0] * span[1]) & LINE_CTRL_SIGNAL_MASK;
    p_runtime->maximum_adc = p_config->maximum_adc & LINE_CTRL_ADC_MASK;
    /* Validated intervals are 1..60000 ms, so each fits uint16_t. */
    p_runtime->junction_debounce_ms =
        (uint16_t) p_config->junction_debounce_ms;
    p_runtime->lost_timeout_ms = (uint16_t) p_config->lost_timeout_ms;
    p_runtime->maximum_gap_ms  = (uint16_t) p_config->maximum_gap_ms;
    p_runtime->b_calibration_confirmed = p_config->b_calibration_confirmed;
    p_runtime->b_valid = true;
    return true;
}

/** @brief Reset all ownership-local state to the unstarted condition. */
void line_ctrl_init (line_ctrl_state_t * p_state)
{
    if (p_state != NULL)
    {
        memset(p_state, 0, sizeof(*p_state));
    }
}

/** @brief Exact rational signal test and Q7 steering; see the header. */
bool line_ctrl_steer (const line_ctrl_runtime_t * p_runtime,
                      const uint16_t * p_raw, int8_t * p_steering_q7)
{
    uint32_t left_span;
    uint32_t right_span;
    uint32_t left;
    uint32_t right;
    uint32_t signal;
    uint32_t magnitude;
    uint32_t rounded;

    if (NULL == p_steering_q7)
    {
        return false;
    }
    *p_steering_q7 = 0;
    if ((NULL == p_runtime) || (NULL == p_raw) || (!p_runtime->b_valid) ||
        (p_raw[0] > p_runtime->maximum_adc) ||
        (p_raw[1] > p_runtime->maximum_adc))
    {
        return false;
    }
    left_span  = p_runtime->channel[0].span_adc;
    right_span = p_runtime->channel[1].span_adc;
    /* d0 = n0 / S0 and d1 = n1 / S1, both scaled by S0 * S1 <= 4095^2. */
    left  = line_ctrl_numerator(&p_runtime->channel[0], p_raw[0]) *
            right_span;
    right = line_ctrl_numerator(&p_runtime->channel[1], p_raw[1]) *
            left_span;
    signal = left + right;
    if (signal < p_runtime->signal_threshold)
    {
        return false;
    }
    magnitude = (right > left) ? (right - left) : (left - right);
    /* signal >= T >= 1. 254 * magnitude + signal <= 255 * 4095^2 < 2^32,
     * and magnitude <= signal keeps the quotient at most 127.
     */
    rounded = ((LINE_CTRL_STEERING_TWICE * magnitude) + signal) /
              (2U * signal);
    /* |rounded| <= 127 fits int8_t in either sign. */
    *p_steering_q7 = (right > left) ? (int8_t) rounded
                                    : (int8_t) (-(int32_t) rounded);
    return true;
}

/** @brief Normalize channels and derive evidence; see header contract. */
bool line_ctrl_step (const line_ctrl_runtime_t * p_runtime,
                     line_ctrl_state_t * p_state,
                     const uint16_t * p_raw, uint32_t now_ms,
                     line_ctrl_output_t * p_output)
{
    uint32_t          channel;
    uint32_t          span;
    uint32_t          numerator;
    uint32_t          bit;
    line_ctrl_class_t evidence;
    bool              b_signal;

    if (NULL == p_output)
    {
        return false;
    }
    memset(p_output, 0, sizeof(*p_output));
    p_output->status = LINE_CTRL_FAULT;
    if ((NULL == p_state) || (NULL == p_raw) || (NULL == p_runtime) ||
        (!p_runtime->b_valid))
    {
        line_ctrl_init(p_state);
        return false;
    }
    if (!p_runtime->b_calibration_confirmed)
    {
        line_ctrl_init(p_state);
        p_output->status = LINE_CTRL_INHIBITED;
        return true;
    }
    if ((p_state->b_started) &&
        ((now_ms - p_state->previous_ms) > p_runtime->maximum_gap_ms))
    {
        line_ctrl_init(p_state);
        return false;
    }
    for (channel = 0U; channel < LINE_CTRL_CHANNELS; ++channel)
    {
        if (p_raw[channel] > p_runtime->maximum_adc)
        {
            line_ctrl_init(p_state);
            return false;
        }
        span = p_runtime->channel[channel].span_adc;
        numerator = line_ctrl_numerator(&p_runtime->channel[channel],
                                        p_raw[channel]);
        /* Nearest Q12 of n / S (S >= 1); at most 4095, so fits uint16_t. */
        p_output->darkness_q12[channel] = (uint16_t)
            (((LINE_CTRL_DARKNESS_ONE * numerator) + (span / 2U)) / span);
        evidence = line_ctrl_classify(
            &p_runtime->channel[channel].hysteresis, p_raw[channel]);
        bit = 1U << channel;
        if (LINE_CTRL_DARK == evidence)
        {
            p_state->black_mask = (p_state->black_mask | bit) &
                                  LINE_CTRL_CHANNEL_MASK;
        }
        else if (LINE_CTRL_LIGHT == evidence)
        {
            p_state->black_mask = (p_state->black_mask & ~bit) &
                                  LINE_CTRL_CHANNEL_MASK;
        }
        else
        {
            /* Hysteresis retains the previous classification. */
        }
    }
    p_state->b_started   = true;
    p_state->previous_ms = now_ms;
    b_signal = line_ctrl_steer(p_runtime, p_raw, &p_output->steering_q7);
    p_output->b_barcode_black =
        0U != (p_state->black_mask & (1U << LINE_CTRL_BARCODE_CHANNEL));
    line_ctrl_events(p_runtime, p_state, now_ms, b_signal, p_output);
    return true;
}

/**
 * @brief Sign (-1, 0, 1) of the exact count * value - reference.
 * @details Valid for count < 2^26 when the rounded product is within a
 * factor of two of reference. Veltkamp splitting gives value = high + low
 * with at most 26 significant bits each, so count * high and count * low
 * are exact; count * high - reference is exact by Sterbenz's lemma, and
 * one rounding of the sum of two exact terms keeps its sign.
 */
static int32_t line_ctrl_sign (uint32_t count, double value,
                               double reference)
{
    double scaled = LINE_CTRL_SPLIT * value;
    double high = scaled - (scaled - value);
    double low = value - high;
    /* count < 2^26 converts to double exactly. */
    double factor = (double) count;
    double residual = ((factor * high) - reference) + (factor * low);

    if (residual > 0.0)
    {
        return 1;
    }
    return (residual < 0.0) ? -1 : 0;
}

/** @brief Exact ceil(fraction * count) for fraction in (0, 1), count < 2^24.
 */
static uint32_t line_ctrl_ceil_product (double fraction, uint32_t count)
{
    /* count < 2^24 converts to double exactly. */
    double product = fraction * (double) count;
    double whole = ceil(product);

    if ((whole == product) && (product > 0.0) &&
        (line_ctrl_sign(count, fraction, product) > 0))
    {
        /* The exact product lies just above the integer it rounded to. */
        whole += 1.0;
    }
    /* 0 <= whole <= count < 2^24 is an integer: exact conversion. */
    return (uint32_t) whole;
}

/** @brief Clamp finite calibrated darkness to the closed unit interval. */
static double line_ctrl_clamp (double value)
{
    if (value < 0.0)
    {
        return 0.0;
    }
    if (value > 1.0)
    {
        return 1.0;
    }
    return value;
}

/** @brief Darkness exactly as the legacy double implementation computed. */
static double line_ctrl_darkness (uint32_t raw, uint16_t white_adc,
                                  uint16_t black_adc)
{
    /* 12-bit ADC values are represented exactly by double. */
    double span = (double) black_adc - (double) white_adc;

    return line_ctrl_clamp(((double) raw - (double) white_adc) / span);
}

/**
 * @brief Smallest raw in 0..4096 from which the legacy compare equals
 * b_holds; 4096 when it never does.
 * @details The compare is darkness >= threshold (b_on) or darkness <=
 * threshold. Darkness is monotone in raw (an exact subtraction, then one
 * correctly rounded division by a constant and a clamp), so the compare
 * changes at most once and the binary search finds the exact boundary.
 */
static uint32_t line_ctrl_boundary (uint16_t white_adc, uint16_t black_adc,
                                    double threshold, bool b_on,
                                    bool b_holds)
{
    uint32_t low = 0U;
    uint32_t high = LINE_CTRL_ADC_COUNT;
    uint32_t middle;
    double   darkness;
    bool     b_compare;

    while (low < high)
    {
        middle = (low + high) / 2U;
        darkness = line_ctrl_darkness(middle, white_adc, black_adc);
        b_compare = b_on ? (darkness >= threshold) : (darkness <= threshold);
        if (b_compare == b_holds)
        {
            high = middle;
        }
        else
        {
            low = middle + 1U;
        }
    }
    return low;
}

/** @brief Darkness numerator n = clamp((raw - white) * polarity, 0, S). */
static uint32_t line_ctrl_numerator (const line_ctrl_channel_t * p_channel,
                                     uint16_t raw)
{
    uint32_t value = raw;
    uint32_t white = p_channel->white_adc;
    uint32_t span = p_channel->span_adc;
    uint32_t numerator = 0U;

    if (p_channel->hysteresis.b_dark_high)
    {
        if (value > white)
        {
            numerator = value - white;
        }
    }
    else if (value < white)
    {
        numerator = white - value;
    }
    else
    {
        /* At or beyond white: darkness clamps to zero. */
    }
    return (numerator > span) ? span : numerator;
}

/** @brief Keep uncertain evidence separate from timed-out line loss. */
static void line_ctrl_lost (const line_ctrl_runtime_t * p_runtime,
                            line_ctrl_state_t * p_state, uint32_t now_ms,
                            bool b_signal, line_ctrl_output_t * p_output)
{
    p_output->status = LINE_CTRL_TRACKING;
    /* !b_signal equals the legacy confidence < minimum_signal: confidence
     * clamped the same sum to 1 and minimum_signal < 1.
     */
    if (!b_signal)
    {
        if (!p_state->b_losing)
        {
            p_state->b_losing      = true;
            p_state->lost_since_ms = now_ms;
        }
        p_output->status = LINE_CTRL_SEARCHING;
        if ((now_ms - p_state->lost_since_ms) >= p_runtime->lost_timeout_ms)
        {
            p_output->status = LINE_CTRL_LOST;
        }
    }
    else
    {
        p_state->b_losing = false;
    }
}

/** @brief Debounce ambiguous junctions and uncertain/lost line evidence. */
static void line_ctrl_events (const line_ctrl_runtime_t * p_runtime,
                              line_ctrl_state_t * p_state,
                              uint32_t now_ms, bool b_signal,
                              line_ctrl_output_t * p_output)
{
    if (LINE_CTRL_EDGE_MASK == (p_state->black_mask & LINE_CTRL_EDGE_MASK))
    {
        p_state->b_losing = false;
        if (!p_state->b_junction_pending)
        {
            p_state->b_junction_pending  = true;
            p_state->junction_since_ms = now_ms;
        }
        p_output->status = LINE_CTRL_SEARCHING;
        if ((now_ms - p_state->junction_since_ms) >=
            p_runtime->junction_debounce_ms)
        {
            p_output->status = LINE_CTRL_JUNCTION;
            p_output->b_junction_edge = !p_state->b_junction_latched;
            p_state->b_junction_latched = true;
        }
    }
    else
    {
        p_state->b_junction_pending = false;
        p_state->b_junction_latched = false;
        line_ctrl_lost(p_runtime, p_state, now_ms, b_signal, p_output);
    }
}

/* End of file. */


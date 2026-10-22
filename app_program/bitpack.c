/**
 * @file bitpack.c
 * @brief Bounded masks, saturation, sign handling and LSB-first streams.
 * NOTE: Early returns preserve shallow validation (BARR-C 6.2.c).
 */
#include "bitpack.h"

#include <float.h>
#include <math.h>
#include <string.h>

#if DBL_MANT_DIG < 53
#error "bitpack quantisation requires at least 53 double mantissa bits"
#endif

#define BITPACK_BYTE_BITS (8U)

/** @brief Width-checked mask; the shift never reaches the word size. */
uint32_t bitpack_mask (uint32_t width)
{
    if (0U == width)
    {
        return 0U;
    }
    if (width >= BITPACK_MAX_WIDTH)
    {
        return UINT32_MAX;
    }
    return (UINT32_C(1) << width) - 1U;
}

/** @brief Values above the field are pinned to its largest code. */
uint32_t bitpack_saturate (uint32_t value, uint32_t width)
{
    uint32_t limit = bitpack_mask(width);

    return (value > limit) ? limit : value;
}

/** @brief Symmetric limit leaves the most negative code reserved. */
int32_t bitpack_signed_max (uint32_t width)
{
    if ((width < 2U) || (width > BITPACK_MAX_WIDTH))
    {
        return 0;
    }
    /* 2^(width-1)-1 is at most INT32_MAX for widths up to 32. */
    return (int32_t)bitpack_mask(width - 1U);
}

/** @brief Only the sign bit set: -2^(width-1) after sign extension. */
uint32_t bitpack_invalid (uint32_t width)
{
    if ((width < 2U) || (width > BITPACK_MAX_WIDTH))
    {
        return 0U;
    }
    return UINT32_C(1) << (width - 1U);
}

/** @brief Saturating two's complement encoding of a signed value. */
uint32_t bitpack_signed (int32_t value, uint32_t width)
{
    int32_t limit = bitpack_signed_max(width);

    if (0 == limit)
    {
        return 0U;
    }
    if (value > limit)
    {
        value = limit;
    }
    else if (value < -limit)
    {
        value = -limit;
    }
    else
    {
        /* Already representable without using the reserved code. */
    }
    /* C99 6.3.1.3: conversion to uint32_t is modulo 2^32, then masked. */
    return ((uint32_t)value) & bitpack_mask(width);
}

/** @brief Sign extension without implementation-defined conversion. */
int32_t bitpack_extend (uint32_t field, uint32_t width)
{
    uint32_t magnitude;

    if ((width < 2U) || (width > BITPACK_MAX_WIDTH))
    {
        return 0;
    }
    field &= bitpack_mask(width);
    if (0U == (field & bitpack_invalid(width)))
    {
        /* A clear sign bit leaves at most 31 value bits. */
        return (int32_t)field;
    }
    /* Negative magnitude is 2^width - field, in 1..2^(width-1). */
    magnitude = (bitpack_mask(width) - field) + 1U;
    if (magnitude > (uint32_t)INT32_MAX)
    {
        return INT32_MIN;
    }
    return -(int32_t)magnitude;
}

/** @brief Scale, round by the field's rule, then clamp before narrowing. */
bool bitpack_quantise (double value, double scale, bitpack_round_t rule,
                       int32_t minimum, int32_t maximum, int32_t * p_result)
{
    double scaled = 0.0;
    double rounded = 0.0;

    if ((NULL == p_result) || (minimum > maximum) || (!isfinite(scale)) ||
        (scale <= 0.0))
    {
        return false;
    }
    scaled = value * scale;
    if (!isfinite(scaled))
    {
        return false;
    }
    switch (rule)
    {
        case BITPACK_CEIL:
            rounded = ceil(scaled);
        break;
        case BITPACK_FLOOR:
            rounded = floor(scaled);
        break;
        case BITPACK_AWAY:
            rounded = (scaled < 0.0) ? floor(scaled) : ceil(scaled);
        break;
        case BITPACK_NEAREST:
        default:
            rounded = round(scaled);
        break;
    }
    if (rounded <= (double)minimum)
    {
        *p_result = minimum;
    }
    else if (rounded >= (double)maximum)
    {
        *p_result = maximum;
    }
    else
    {
        /* Strictly between two int32_t bounds, so the cast is exact. */
        *p_result = (int32_t)rounded;
    }
    return true;
}

/** @brief Zero-filled storage lets every write only set one bits. */
void bitpack_writer_init (bitpack_writer_t * p_writer, uint8_t * p_data,
                          size_t capacity)
{
    if (NULL == p_writer)
    {
        return;
    }
    p_writer->p_data   = p_data;
    p_writer->capacity = capacity;
    p_writer->bit      = 0U;
    p_writer->b_valid  = (NULL != p_data) && (capacity > 0U) &&
                         (capacity <= (SIZE_MAX / BITPACK_BYTE_BITS));
    if (p_writer->b_valid)
    {
        memset(p_data, 0, capacity);
    }
}

/** @brief Bit-serial append keeps the byte order independent of the CPU. */
void bitpack_write (bitpack_writer_t * p_writer, uint32_t value,
                    uint32_t width)
{
    uint32_t index;
    size_t   byte;
    uint8_t  bit_mask;

    if ((NULL == p_writer) || (!p_writer->b_valid))
    {
        return;
    }
    if ((0U == width) || (width > BITPACK_MAX_WIDTH) ||
        (value > bitpack_mask(width)) ||
        ((p_writer->bit + width) >
         (p_writer->capacity * BITPACK_BYTE_BITS)))
    {
        p_writer->b_valid = false;
        return;
    }
    for (index = 0U; index < width; index++)
    {
        if (0U != ((value >> index) & 1U))
        {
            byte = p_writer->bit / BITPACK_BYTE_BITS;
            /* The shift is 0..7, so the bit mask fits one byte. */
            bit_mask = (uint8_t)(1U << (p_writer->bit % BITPACK_BYTE_BITS));
            p_writer->p_data[byte] =
                (uint8_t)(p_writer->p_data[byte] | bit_mask);
        }
        p_writer->bit++;
    }
}

/** @brief A partly used final byte counts as one whole byte. */
size_t bitpack_writer_size (const bitpack_writer_t * p_writer)
{
    if ((NULL == p_writer) || (!p_writer->b_valid))
    {
        return 0U;
    }
    return (p_writer->bit + (BITPACK_BYTE_BITS - 1U)) / BITPACK_BYTE_BITS;
}

/** @brief A NULL frame is valid only when it is empty. */
void bitpack_reader_init (bitpack_reader_t * p_reader,
                          const uint8_t * p_data, size_t length)
{
    if (NULL == p_reader)
    {
        return;
    }
    p_reader->p_data  = p_data;
    p_reader->length  = length;
    p_reader->bit     = 0U;
    p_reader->b_valid = ((NULL != p_data) || (0U == length)) &&
                        (length <= (SIZE_MAX / BITPACK_BYTE_BITS));
}

/** @brief Mirror of bitpack_write; truncated frames read as invalid. */
uint32_t bitpack_read (bitpack_reader_t * p_reader, uint32_t width)
{
    uint32_t value = 0U;
    uint32_t index;
    size_t   byte;

    if ((NULL == p_reader) || (!p_reader->b_valid))
    {
        return 0U;
    }
    if ((0U == width) || (width > BITPACK_MAX_WIDTH) ||
        ((p_reader->bit + width) > (p_reader->length * BITPACK_BYTE_BITS)))
    {
        p_reader->b_valid = false;
        return 0U;
    }
    for (index = 0U; index < width; index++)
    {
        byte = p_reader->bit / BITPACK_BYTE_BITS;
        if (0U != (((uint32_t)p_reader->p_data[byte] >>
                    (p_reader->bit % BITPACK_BYTE_BITS)) & 1U))
        {
            value |= UINT32_C(1) << index;
        }
        p_reader->bit++;
    }
    return value;
}

/** @brief Exact frame length with zero padding guards against stale bytes.
 */
bool bitpack_reader_complete (const bitpack_reader_t * p_reader)
{
    size_t   used;
    uint32_t spare;

    if ((NULL == p_reader) || (!p_reader->b_valid))
    {
        return false;
    }
    used = (p_reader->bit + (BITPACK_BYTE_BITS - 1U)) / BITPACK_BYTE_BITS;
    if (used != p_reader->length)
    {
        return false;
    }
    /* The remainder of a division by 8 always fits uint32_t. */
    spare = (uint32_t)(p_reader->bit % BITPACK_BYTE_BITS);
    if (0U == spare)
    {
        return true;
    }
    return 0U == ((uint32_t)p_reader->p_data[used - 1U] >> spare);
}

/* End of file. */


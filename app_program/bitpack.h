/**
 * @file bitpack.h
 * @brief Minimum-width field arithmetic for shared, stored and sent data.
 * @details Saturating quantisation with an explicit rounding rule, two's
 * complement fields of 2..32 bits and a little-endian, least-significant-
 * bit-first stream for wire frames. Wire layouts use the stream, never C
 * bit-field structs, whose layout is implementation-defined. In-memory
 * records may use bit-fields; signed members are stored through
 * bitpack_signed()/bitpack_extend() so -Wconversion sees explicit masking.
 * Pure functions: no allocation, I/O or shared state.
 */
#ifndef BITPACK_H
#define BITPACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BITPACK_MAX_WIDTH (32U)

/** Rounding rule per field. For an integer threshold T in field units,
 * CEIL keeps x <= T and x > T decisions, FLOOR keeps x >= T and x < T,
 * and AWAY (magnitude rounded up) keeps |x| > T and |x| <= T, except
 * within one double rounding of the scaled value.
 */
typedef enum
{
    BITPACK_NEAREST = 0,
    BITPACK_CEIL,
    BITPACK_FLOOR,
    BITPACK_AWAY
} bitpack_round_t;

/** Appends fields from bit 0 of byte 0 upwards; unused high bits are 0. */
typedef struct
{
    uint8_t * p_data;
    size_t    capacity;
    size_t    bit;
    bool      b_valid;
} bitpack_writer_t;

/** Reads fields in the order and widths they were written. */
typedef struct
{
    const uint8_t * p_data;
    size_t          length;
    size_t          bit;
    bool            b_valid;
} bitpack_reader_t;

/** @brief Low width bits set; 0 for width 0, all ones from width 32. */
uint32_t bitpack_mask (uint32_t width);

/** @brief Clamp an unsigned value to 0..2^width-1. */
uint32_t bitpack_saturate (uint32_t value, uint32_t width);

/** @brief Largest symmetric magnitude 2^(width-1)-1; 0 outside 2..32. */
int32_t bitpack_signed_max (uint32_t width);

/** @brief Reserved most-negative code of a 2..32-bit signed field.
 * By project convention it means "invalid/unknown", never a measurement.
 */
uint32_t bitpack_invalid (uint32_t width);

/** @brief Saturate to +/-(2^(width-1)-1) and return the two's complement
 * field. Width outside 2..32 returns 0. The reserved code is never made.
 */
uint32_t bitpack_signed (int32_t value, uint32_t width);

/** @brief Sign-extend the low width bits; width outside 2..32 returns 0. */
int32_t bitpack_extend (uint32_t field, uint32_t width);

/** @brief Round value * scale by rule and clamp to [minimum, maximum].
 * False, with *p_result unchanged, for NULL, a non-finite product, a
 * non-positive scale or minimum > maximum. Callers encode the false case
 * with an explicit validity bit or reserved code, never as a measurement.
 */
bool bitpack_quantise (double value, double scale, bitpack_round_t rule,
                       int32_t minimum, int32_t maximum, int32_t * p_result);

/** @brief Start an empty stream; the whole capacity is zero-filled. */
void bitpack_writer_init (bitpack_writer_t * p_writer, uint8_t * p_data,
                          size_t capacity);

/** @brief Append value in width bits (1..32). A value wider than width or
 * a full buffer invalidates the writer; nothing later is written.
 */
void bitpack_write (bitpack_writer_t * p_writer, uint32_t value,
                    uint32_t width);

/** @brief Bytes used, rounded up to a whole byte; 0 for an invalid writer.
 */
size_t bitpack_writer_size (const bitpack_writer_t * p_writer);

/** @brief Start reading a received frame of length bytes. */
void bitpack_reader_init (bitpack_reader_t * p_reader,
                          const uint8_t * p_data, size_t length);

/** @brief Read width bits (1..32); past the end invalidates and returns 0.
 */
uint32_t bitpack_read (bitpack_reader_t * p_reader, uint32_t width);

/** @brief True when valid, every byte was used and padding bits are zero.
 */
bool bitpack_reader_complete (const bitpack_reader_t * p_reader);

#endif /* BITPACK_H */

/* End of file. */


/**
 * @file telemetry.c
 * @brief Fixed-layout binary frames built with explicit shifts and masks.
 * NOTE: Early returns preserve shallow validation (BARR-C 6.2.c).
 */
#include "telemetry.h"

#include <string.h>

#include "bitpack.h"

#define TELEMETRY_VERSION_BITS (2U)
#define TELEMETRY_TYPE_BITS    (2U)
#define TELEMETRY_SEQ_BITS     (8U)
#define TELEMETRY_MS_BITS      (32U)
#define TELEMETRY_COUNT_BITS   (8U)
#define TELEMETRY_AGE_BITS     (10U)
#define TELEMETRY_STATE_BITS   (4U)
#define TELEMETRY_SPEED_BITS   (10U)
#define TELEMETRY_ANGLE_BITS   (12U)
#define TELEMETRY_HEIGHT_BITS  (9U)
#define TELEMETRY_RANGE_BITS   (12U)
#define TELEMETRY_RUN_BITS     (8U)
#define TELEMETRY_CAL_BITS     (8U)
#define TELEMETRY_ACTION_BITS  (3U)
#define TELEMETRY_SYMBOL_BITS  (4U)
#define TELEMETRY_RATE_BITS    (13U)
#define TELEMETRY_CLASS_BITS   (4U)
#define TELEMETRY_SIDE_BITS    (2U)
#define TELEMETRY_SAMPLE_BITS  (6U)
#define TELEMETRY_FAULT_BITS   (4U)
#define TELEMETRY_MASK_BITS    (16U)
#define TELEMETRY_SOURCE_BITS  (2U)
#define TELEMETRY_FLAG_BITS    (1U)

static void telemetry_header (bitpack_writer_t * p_writer,
                              telemetry_frame_t type);
static bool telemetry_header_matches (bitpack_reader_t * p_reader,
                                      telemetry_frame_t type);
static void telemetry_unsigned (bitpack_writer_t * p_writer, uint32_t value,
                                uint32_t width);
static void telemetry_signed (bitpack_writer_t * p_writer, int16_t value,
                              uint32_t width);
static void telemetry_flag (bitpack_writer_t * p_writer, bool b_value);
static int16_t telemetry_read_signed (bitpack_reader_t * p_reader,
                                      uint32_t width);
static uint8_t telemetry_read_byte (bitpack_reader_t * p_reader,
                                    uint32_t width);
static uint16_t telemetry_read_half (bitpack_reader_t * p_reader,
                                     uint32_t width);

/** @brief Status layout: 131 bits, five zero padding bits. */
size_t telemetry_encode_status (const telemetry_status_t * p_status,
                                uint8_t * p_frame, size_t capacity)
{
    bitpack_writer_t writer;

    if ((NULL == p_status) || (NULL == p_frame) ||
        (capacity < TELEMETRY_STATUS_BYTES))
    {
        return 0U;
    }
    bitpack_writer_init(&writer, p_frame, TELEMETRY_STATUS_BYTES);
    telemetry_header(&writer, TELEMETRY_FRAME_STATUS);
    telemetry_unsigned(&writer, p_status->seq, TELEMETRY_SEQ_BITS);
    telemetry_unsigned(&writer, p_status->now_ms, TELEMETRY_MS_BITS);
    telemetry_unsigned(&writer, p_status->dropped, TELEMETRY_COUNT_BITS);
    telemetry_unsigned(&writer, p_status->ignored, TELEMETRY_COUNT_BITS);
    telemetry_unsigned(&writer, p_status->range_age_ms, TELEMETRY_AGE_BITS);
    telemetry_unsigned(&writer, p_status->state, TELEMETRY_STATE_BITS);
    telemetry_signed(&writer, p_status->vleft_mm_s, TELEMETRY_SPEED_BITS);
    telemetry_signed(&writer, p_status->vright_mm_s, TELEMETRY_SPEED_BITS);
    telemetry_signed(&writer, p_status->pitch_mrad, TELEMETRY_ANGLE_BITS);
    telemetry_unsigned(&writer, p_status->peak_mm, TELEMETRY_HEIGHT_BITS);
    telemetry_unsigned(&writer, p_status->range_mm, TELEMETRY_RANGE_BITS);
    telemetry_flag(&writer, p_status->b_stop);
    telemetry_flag(&writer, p_status->b_hardfault);
    telemetry_flag(&writer, p_status->b_heightvalid);
    telemetry_flag(&writer, p_status->b_rangevalid);
    return bitpack_writer_size(&writer);
}

/** @brief Perception layout: 126 bits, two zero padding bits. */
size_t telemetry_encode_perception (
    const telemetry_perception_t * p_status, uint8_t * p_frame,
    size_t capacity)
{
    bitpack_writer_t writer;

    if ((NULL == p_status) || (NULL == p_frame) ||
        (capacity < TELEMETRY_PERCEPTION_BYTES))
    {
        return 0U;
    }
    bitpack_writer_init(&writer, p_frame, TELEMETRY_PERCEPTION_BYTES);
    telemetry_header(&writer, TELEMETRY_FRAME_PERCEPTION);
    telemetry_unsigned(&writer, p_status->seq, TELEMETRY_SEQ_BITS);
    telemetry_unsigned(&writer, p_status->now_ms, TELEMETRY_MS_BITS);
    telemetry_unsigned(&writer, p_status->run_id, TELEMETRY_RUN_BITS);
    telemetry_unsigned(&writer, p_status->cal_version, TELEMETRY_CAL_BITS);
    telemetry_unsigned(&writer, p_status->barcode_action,
                       TELEMETRY_ACTION_BITS);
    telemetry_unsigned(&writer, p_status->barcode_symbol,
                       TELEMETRY_SYMBOL_BITS);
    telemetry_signed(&writer, p_status->yaw_mrad_s, TELEMETRY_RATE_BITS);
    telemetry_unsigned(&writer, p_status->motion_class,
                       TELEMETRY_CLASS_BITS);
    telemetry_unsigned(&writer, p_status->scan_width_mm,
                       TELEMETRY_RANGE_BITS);
    telemetry_unsigned(&writer, p_status->scan_nearest_mm,
                       TELEMETRY_RANGE_BITS);
    telemetry_unsigned(&writer, p_status->scan_side, TELEMETRY_SIDE_BITS);
    telemetry_unsigned(&writer, p_status->scan_valid, TELEMETRY_SAMPLE_BITS);
    telemetry_unsigned(&writer, p_status->scan_unknown,
                       TELEMETRY_SAMPLE_BITS);
    telemetry_flag(&writer, p_status->b_tilt_valid);
    telemetry_flag(&writer, p_status->b_hump_active);
    telemetry_flag(&writer, p_status->b_scan_valid);
    telemetry_flag(&writer, p_status->b_obstacle_seen);
    return bitpack_writer_size(&writer);
}

/** @brief Diagnostic layout: 132 bits, four zero padding bits. */
size_t telemetry_encode_diagnostic (
    const telemetry_diagnostic_t * p_status, uint8_t * p_frame,
    size_t capacity)
{
    bitpack_writer_t writer;

    if ((NULL == p_status) || (NULL == p_frame) ||
        (capacity < TELEMETRY_DIAGNOSTIC_BYTES))
    {
        return 0U;
    }
    bitpack_writer_init(&writer, p_frame, TELEMETRY_DIAGNOSTIC_BYTES);
    telemetry_header(&writer, TELEMETRY_FRAME_DIAGNOSTIC);
    telemetry_unsigned(&writer, p_status->seq, TELEMETRY_SEQ_BITS);
    telemetry_unsigned(&writer, p_status->now_ms, TELEMETRY_MS_BITS);
    telemetry_unsigned(&writer, p_status->run_id, TELEMETRY_RUN_BITS);
    telemetry_unsigned(&writer, p_status->fault_code, TELEMETRY_FAULT_BITS);
    telemetry_unsigned(&writer, p_status->first_fault_ms, TELEMETRY_MS_BITS);
    telemetry_unsigned(&writer, p_status->barcode_unknown,
                       TELEMETRY_COUNT_BITS);
    telemetry_unsigned(&writer, p_status->barcode_rejected,
                       TELEMETRY_COUNT_BITS);
    telemetry_unsigned(&writer, p_status->mission_peak_mm,
                       TELEMETRY_HEIGHT_BITS);
    telemetry_flag(&writer, p_status->b_mission_valid);
    telemetry_unsigned(&writer, p_status->fault_mask, TELEMETRY_MASK_BITS);
    telemetry_unsigned(&writer, p_status->stop_source,
                       TELEMETRY_SOURCE_BITS);
    return bitpack_writer_size(&writer);
}

/** @brief The first byte alone identifies the version and schema. */
bool telemetry_frame_type (const uint8_t * p_frame, size_t length,
                           telemetry_frame_t * p_type)
{
    uint32_t type;

    if ((NULL == p_frame) || (NULL == p_type) || (0U == length) ||
        (TELEMETRY_VERSION !=
         ((uint32_t)p_frame[0] & bitpack_mask(TELEMETRY_VERSION_BITS))))
    {
        return false;
    }
    type = ((uint32_t)p_frame[0] >> TELEMETRY_VERSION_BITS) &
           bitpack_mask(TELEMETRY_TYPE_BITS);
    switch (type)
    {
        case 0U:
            *p_type = TELEMETRY_FRAME_STATUS;
        break;
        case 1U:
            *p_type = TELEMETRY_FRAME_PERCEPTION;
        break;
        case 2U:
            *p_type = TELEMETRY_FRAME_DIAGNOSTIC;
        break;
        default:
            return false;
    }
    return true;
}

/** @brief Read back the status layout in the same order and widths. */
bool telemetry_decode_status (const uint8_t * p_frame, size_t length,
                              telemetry_status_t * p_status)
{
    bitpack_reader_t reader;
    telemetry_status_t status;

    if ((NULL == p_frame) || (NULL == p_status) ||
        (TELEMETRY_STATUS_BYTES != length))
    {
        return false;
    }
    memset(&status, 0, sizeof(status));
    bitpack_reader_init(&reader, p_frame, length);
    if (!telemetry_header_matches(&reader, TELEMETRY_FRAME_STATUS))
    {
        return false;
    }
    status.seq = telemetry_read_byte(&reader, TELEMETRY_SEQ_BITS);
    status.now_ms = bitpack_read(&reader, TELEMETRY_MS_BITS);
    status.dropped = telemetry_read_byte(&reader, TELEMETRY_COUNT_BITS);
    status.ignored = telemetry_read_byte(&reader, TELEMETRY_COUNT_BITS);
    status.range_age_ms = telemetry_read_half(&reader, TELEMETRY_AGE_BITS);
    status.state = telemetry_read_byte(&reader, TELEMETRY_STATE_BITS);
    status.vleft_mm_s = telemetry_read_signed(&reader, TELEMETRY_SPEED_BITS);
    status.vright_mm_s = telemetry_read_signed(&reader,
                                               TELEMETRY_SPEED_BITS);
    status.pitch_mrad = telemetry_read_signed(&reader, TELEMETRY_ANGLE_BITS);
    status.peak_mm = telemetry_read_half(&reader, TELEMETRY_HEIGHT_BITS);
    status.range_mm = telemetry_read_half(&reader, TELEMETRY_RANGE_BITS);
    status.b_stop = 0U != bitpack_read(&reader, TELEMETRY_FLAG_BITS);
    status.b_hardfault = 0U != bitpack_read(&reader, TELEMETRY_FLAG_BITS);
    status.b_heightvalid = 0U != bitpack_read(&reader, TELEMETRY_FLAG_BITS);
    status.b_rangevalid = 0U != bitpack_read(&reader, TELEMETRY_FLAG_BITS);
    if (!bitpack_reader_complete(&reader))
    {
        return false;
    }
    *p_status = status;
    return true;
}

/** @brief Read back the perception layout in the same order and widths. */
bool telemetry_decode_perception (const uint8_t * p_frame, size_t length,
                                  telemetry_perception_t * p_status)
{
    bitpack_reader_t reader;
    telemetry_perception_t status;

    if ((NULL == p_frame) || (NULL == p_status) ||
        (TELEMETRY_PERCEPTION_BYTES != length))
    {
        return false;
    }
    memset(&status, 0, sizeof(status));
    bitpack_reader_init(&reader, p_frame, length);
    if (!telemetry_header_matches(&reader, TELEMETRY_FRAME_PERCEPTION))
    {
        return false;
    }
    status.seq = telemetry_read_byte(&reader, TELEMETRY_SEQ_BITS);
    status.now_ms = bitpack_read(&reader, TELEMETRY_MS_BITS);
    status.run_id = telemetry_read_byte(&reader, TELEMETRY_RUN_BITS);
    status.cal_version = telemetry_read_byte(&reader, TELEMETRY_CAL_BITS);
    status.barcode_action = telemetry_read_byte(&reader,
                                                TELEMETRY_ACTION_BITS);
    status.barcode_symbol = telemetry_read_byte(&reader,
                                                TELEMETRY_SYMBOL_BITS);
    status.yaw_mrad_s = telemetry_read_signed(&reader, TELEMETRY_RATE_BITS);
    status.motion_class = telemetry_read_byte(&reader, TELEMETRY_CLASS_BITS);
    status.scan_width_mm = telemetry_read_half(&reader, TELEMETRY_RANGE_BITS);
    status.scan_nearest_mm = telemetry_read_half(&reader,
                                                 TELEMETRY_RANGE_BITS);
    status.scan_side = telemetry_read_byte(&reader, TELEMETRY_SIDE_BITS);
    status.scan_valid = telemetry_read_byte(&reader, TELEMETRY_SAMPLE_BITS);
    status.scan_unknown = telemetry_read_byte(&reader,
                                              TELEMETRY_SAMPLE_BITS);
    status.b_tilt_valid = 0U != bitpack_read(&reader, TELEMETRY_FLAG_BITS);
    status.b_hump_active = 0U != bitpack_read(&reader, TELEMETRY_FLAG_BITS);
    status.b_scan_valid = 0U != bitpack_read(&reader, TELEMETRY_FLAG_BITS);
    status.b_obstacle_seen = 0U != bitpack_read(&reader,
                                                TELEMETRY_FLAG_BITS);
    if (!bitpack_reader_complete(&reader))
    {
        return false;
    }
    *p_status = status;
    return true;
}

/** @brief Read back the diagnostic layout in the same order and widths. */
bool telemetry_decode_diagnostic (const uint8_t * p_frame, size_t length,
                                  telemetry_diagnostic_t * p_status)
{
    bitpack_reader_t reader;
    telemetry_diagnostic_t status;

    if ((NULL == p_frame) || (NULL == p_status) ||
        (TELEMETRY_DIAGNOSTIC_BYTES != length))
    {
        return false;
    }
    memset(&status, 0, sizeof(status));
    bitpack_reader_init(&reader, p_frame, length);
    if (!telemetry_header_matches(&reader, TELEMETRY_FRAME_DIAGNOSTIC))
    {
        return false;
    }
    status.seq = telemetry_read_byte(&reader, TELEMETRY_SEQ_BITS);
    status.now_ms = bitpack_read(&reader, TELEMETRY_MS_BITS);
    status.run_id = telemetry_read_byte(&reader, TELEMETRY_RUN_BITS);
    status.fault_code = telemetry_read_byte(&reader, TELEMETRY_FAULT_BITS);
    status.first_fault_ms = bitpack_read(&reader, TELEMETRY_MS_BITS);
    status.barcode_unknown = telemetry_read_byte(&reader,
                                                 TELEMETRY_COUNT_BITS);
    status.barcode_rejected = telemetry_read_byte(&reader,
                                                  TELEMETRY_COUNT_BITS);
    status.mission_peak_mm = telemetry_read_half(&reader,
                                                 TELEMETRY_HEIGHT_BITS);
    status.b_mission_valid = 0U != bitpack_read(&reader,
                                                TELEMETRY_FLAG_BITS);
    status.fault_mask = telemetry_read_half(&reader, TELEMETRY_MASK_BITS);
    status.stop_source = telemetry_read_byte(&reader, TELEMETRY_SOURCE_BITS);
    if (!bitpack_reader_complete(&reader))
    {
        return false;
    }
    *p_status = status;
    return true;
}

/** @brief Version occupies bits 0-1 and the frame type bits 2-3. */
static void telemetry_header (bitpack_writer_t * p_writer,
                              telemetry_frame_t type)
{
    bitpack_write(p_writer, TELEMETRY_VERSION, TELEMETRY_VERSION_BITS);
    /* Enumerators 0..2 fit the two-bit type field. */
    bitpack_write(p_writer, (uint32_t)type, TELEMETRY_TYPE_BITS);
}

/** @brief A decoder accepts only its own version and frame type. */
static bool telemetry_header_matches (bitpack_reader_t * p_reader,
                                      telemetry_frame_t type)
{
    uint32_t version = bitpack_read(p_reader, TELEMETRY_VERSION_BITS);
    uint32_t frame = bitpack_read(p_reader, TELEMETRY_TYPE_BITS);

    /* Enumerators 0..2 are exactly representable as uint32_t. */
    return (TELEMETRY_VERSION == version) && ((uint32_t)type == frame);
}

/** @brief Unsigned fields saturate instead of wrapping into small values. */
static void telemetry_unsigned (bitpack_writer_t * p_writer, uint32_t value,
                                uint32_t width)
{
    bitpack_write(p_writer, bitpack_saturate(value, width), width);
}

/** @brief Only TELEMETRY_INVALID produces the reserved signed code. */
static void telemetry_signed (bitpack_writer_t * p_writer, int16_t value,
                              uint32_t width)
{
    uint32_t field = (TELEMETRY_INVALID == value) ?
                     bitpack_invalid(width) : bitpack_signed(value, width);

    bitpack_write(p_writer, field, width);
}

/** @brief Boolean fields occupy exactly one bit. */
static void telemetry_flag (bitpack_writer_t * p_writer, bool b_value)
{
    bitpack_write(p_writer, b_value ? 1U : 0U, TELEMETRY_FLAG_BITS);
}

/** @brief Map the reserved code back to TELEMETRY_INVALID. */
static int16_t telemetry_read_signed (bitpack_reader_t * p_reader,
                                      uint32_t width)
{
    uint32_t field = bitpack_read(p_reader, width);

    if (bitpack_invalid(width) == field)
    {
        return TELEMETRY_INVALID;
    }
    /* Fields are at most 13 bits: the value fits int16_t exactly. */
    return (int16_t)bitpack_extend(field, width);
}

/** @brief Fields of at most eight bits fit uint8_t exactly. */
static uint8_t telemetry_read_byte (bitpack_reader_t * p_reader,
                                    uint32_t width)
{
    return (uint8_t)bitpack_read(p_reader, width);
}

/** @brief Fields of at most sixteen bits fit uint16_t exactly. */
static uint16_t telemetry_read_half (bitpack_reader_t * p_reader,
                                     uint32_t width)
{
    return (uint16_t)bitpack_read(p_reader, width);
}

/* End of file. */


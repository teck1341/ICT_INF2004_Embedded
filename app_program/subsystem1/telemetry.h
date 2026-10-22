/**
 * @file telemetry.h
 * @brief Compact binary telemetry frames; no printf, text or allocation.
 * @details Each frame is a little-endian, least-significant-bit-first
 * stream (bitpack.h) that starts with a 4-bit header: version (2 bits) then
 * frame type (2 bits). Fields follow in the order of the tables below.
 * Unsigned fields saturate at their largest code. Signed fields are two's
 * complement, saturate at +/-(2^(n-1)-1), and use the most negative code
 * only for TELEMETRY_INVALID (non-finite or unavailable). Padding bits in
 * the last byte are zero. scripts/telemetry_codec.py implements the same
 * contract; change TELEMETRY_VERSION with any layout change.
 *
 * Status, every 200 ms, 131 bits in 17 bytes:
 *   seq 8 (0..249) | ms 32 | drop 8 | ign 8 | age 10 ms (1023 = older) |
 *   st 4 (vehicle_state_t) | vl 10, vr 10 mm/s | pt 12 mrad | pk 9 mm |
 *   rg 12 mm (read only with rv) | stop 1 | fault 1 | hv 1 | rv 1
 * Perception, when seq % 5 == 0, 126 bits in 16 bytes:
 *   seq 8 | ms 32 | run 8 | cal 8 | ba 3 (barcode_action_t) | bl 4
 *   (codebook index, read only with ba != 0) | yr 13 mrad/s | mc 4
 *   (terrain_motion_t) | sw 12 mm (read only with so) | sn 12 mm (read
 *   only with sv > 0) | ss 2 (scanner_side_t) | sv 6 | su 6 | tv 1 | ha 1 |
 *   sc 1 | so 1
 * Diagnostic, with the perception frame, 132 bits in 17 bytes:
 *   seq 8 | ms 32 | run 8 | fc 4 (robot_app_fault_t) | fm 32 ms | bu 8 |
 *   br 8 | mp 9 mm | mv 1 | mask 16 (active fault bits) | src 2 (bit 0
 *   local STOP, bit 1 remote STOP)
 * Perception and diagnostic frames reuse the status seq, so a missing one
 * is a status seq divisible by 5 without its partner frame.
 */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TELEMETRY_VERSION          (1U)
#define TELEMETRY_STATUS_BYTES     (17U)
#define TELEMETRY_PERCEPTION_BYTES (16U)
#define TELEMETRY_DIAGNOSTIC_BYTES (17U)
#define TELEMETRY_FRAME_MAX        (17U)
#define TELEMETRY_SEQUENCE_MODULO  (250U)
/** Field value meaning "invalid"; encoded as the reserved signed code. */
#define TELEMETRY_INVALID          (INT16_MIN)

typedef enum
{
    TELEMETRY_FRAME_STATUS = 0,
    TELEMETRY_FRAME_PERCEPTION,
    TELEMETRY_FRAME_DIAGNOSTIC
} telemetry_frame_t;

/** @brief Status fields in wire units; see the layout above. */
typedef struct
{
    uint32_t now_ms;
    uint16_t range_age_ms;
    int16_t  vleft_mm_s;
    int16_t  vright_mm_s;
    int16_t  pitch_mrad;
    uint16_t peak_mm;
    uint16_t range_mm;
    uint8_t  seq;
    uint8_t  dropped;
    uint8_t  ignored;
    uint8_t  state;
    bool     b_stop;
    bool     b_hardfault;
    bool     b_heightvalid;
    bool     b_rangevalid;
} telemetry_status_t;

/** @brief Perception fields in wire units; see the layout above. */
typedef struct
{
    uint32_t now_ms;
    int16_t  yaw_mrad_s;
    uint16_t scan_width_mm;
    uint16_t scan_nearest_mm;
    uint8_t  seq;
    uint8_t  run_id;
    uint8_t  cal_version;
    uint8_t  barcode_action;
    uint8_t  barcode_symbol;
    uint8_t  motion_class;
    uint8_t  scan_side;
    uint8_t  scan_valid;
    uint8_t  scan_unknown;
    bool     b_tilt_valid;
    bool     b_hump_active;
    bool     b_scan_valid;
    bool     b_obstacle_seen;
} telemetry_perception_t;

/** @brief First-fault and fault-mask fields; see the layout above. */
typedef struct
{
    uint32_t now_ms;
    uint32_t first_fault_ms;
    uint16_t fault_mask;
    uint16_t mission_peak_mm;
    uint8_t  seq;
    uint8_t  run_id;
    uint8_t  fault_code;
    uint8_t  stop_source;
    uint8_t  barcode_unknown;
    uint8_t  barcode_rejected;
    bool     b_mission_valid;
} telemetry_diagnostic_t;

/** @brief Encode one status frame; return its byte count, or 0 for NULL
 * arguments or capacity below TELEMETRY_STATUS_BYTES (nothing written).
 */
size_t telemetry_encode_status (const telemetry_status_t * p_status,
                                uint8_t * p_frame, size_t capacity);

/** @brief Encode one perception frame with the same contract. */
size_t telemetry_encode_perception (
    const telemetry_perception_t * p_status, uint8_t * p_frame,
    size_t capacity);

/** @brief Encode one diagnostic frame with the same contract. */
size_t telemetry_encode_diagnostic (
    const telemetry_diagnostic_t * p_status, uint8_t * p_frame,
    size_t capacity);

/** @brief Frame type of a received frame of this version; false for NULL,
 * an empty frame, another version or the reserved type 3.
 */
bool telemetry_frame_type (const uint8_t * p_frame, size_t length,
                           telemetry_frame_t * p_type);

/** @brief Decode a status frame. False, leaving *p_status unchanged, for a
 * wrong length, version, type or nonzero padding.
 */
bool telemetry_decode_status (const uint8_t * p_frame, size_t length,
                              telemetry_status_t * p_status);

/** @brief Decode a perception frame with the same contract. */
bool telemetry_decode_perception (const uint8_t * p_frame, size_t length,
                                  telemetry_perception_t * p_status);

/** @brief Decode a diagnostic frame with the same contract. */
bool telemetry_decode_diagnostic (const uint8_t * p_frame, size_t length,
                                  telemetry_diagnostic_t * p_status);

#endif /* TELEMETRY_H */

/* End of file. */


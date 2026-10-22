/** @file motion.h
 * @brief Single-owner, leased wheel control; SI units throughout.
 * Calls require serialization by the owning motion task. Calibration is a gate.
 */
#ifndef MOTION_H
#define MOTION_H

#include "core.h"
#include <stdbool.h>
#include <stdint.h>

#define MOTION_LEASE_MS (50U)
#define MOTION_SPEED_BUCKETS (11U)
/* Command IDs: serial order modulo 2^8, half-range 128, 0 reserved. */
#define MOTION_ID_BITS (8U)
/* Two's complement widths of the shared request and snapshot fields. */
#define MOTION_FORWARD_BITS (9U)
#define MOTION_YAW_BITS (11U)
#define MOTION_TARGET_BITS (14U)
#define MOTION_SPEED_BITS (10U)

typedef enum
{
    MOTION_IDLE = 0,
    MOTION_RUNNING,
    MOTION_COMPLETE,
    MOTION_INHIBITED
} motion_state_t;

typedef enum
{
    MOTION_VELOCITY = 0,
    MOTION_DISTANCE,
    MOTION_TURN,
    MOTION_HALT
} motion_kind_t;

typedef enum
{
    MOTION_OK = 0,
    MOTION_INVALID,
    MOTION_REJECTED,
    MOTION_EXPIRED,
    MOTION_TIMED_OUT,
    MOTION_SENSOR_FAULT,
    MOTION_STOPPED
} motion_status_t;

typedef struct
{
    uint8_t         command_id;
    motion_kind_t   kind;
    core_real_t     forward_mps;
    core_real_t     yaw_rps;
    core_real_t     target;
} motion_request_t;

/** forward 1 mm/s (+/-255), yaw 1 mrad/s (+/-1023), target 1 mm for
 * DISTANCE or 1 mrad for TURN (+/-8191, 0 otherwise), magnitude rounded up
 * (AWAY), symmetric saturation; the reserved code means non-finite and
 * unpacks to NAN so motion_request_valid still rejects it. 44 bits.
 * AWAY keeps every |x| > T check exact on whole units; a TURN target with
 * |target| <= 2*pi is capped at 6283 mrad so the 2*pi check stays exact.
 * Saturated codes stay rejected only while max_wheel_mps < 0.255 and
 * max_yaw_rps < 1.023 (production 0.15 and 1.0).
 */
typedef struct
{
    uint32_t      forward_mm_s : 9;
    uint32_t      yaw_mrad_s   : 11;
    uint32_t      command_id   : 8;
    motion_kind_t kind         : 2;
    uint32_t      target       : 14;
} motion_request_packed_t;

typedef struct
{
    core_odometry_config_t  odometry;
    core_pid_config_t       wheel_pid;
    core_real_t             max_wheel_mps;
    core_real_t             max_yaw_rps;
    core_real_t             distance_tolerance_m;
    core_real_t             turn_tolerance_rad;
    core_real_t             stationary_mps;
    uint32_t                primitive_timeout_ms;
    bool                    b_calibration_confirmed;
} motion_config_t;

typedef struct
{
    motion_state_t  state;
    motion_status_t reason;
    uint8_t         command_id;
    core_pose_t     pose;
    core_real_t     left_duty;
    core_real_t     right_duty;
    core_real_t     left_speed_mps;
    core_real_t     right_speed_mps;
    core_real_t     signed_distance_m; /* Raw centre delta of latest step. */
    bool            b_stationary;
} motion_output_t;

/** pose (core_pose_packed_t), wheel speeds 1 mm/s in 10-bit signed fields
 * (+/-511, nearest, reserved code = non-finite -> NAN), state 2 bits,
 * reason 3 bits, command ID 8, stationary 1. Duties, centre path and the
 * latest signed distance are motion-task data and are not shared.
 */
typedef struct
{
    core_pose_packed_t pose;
    uint32_t           left_mm_s    : 10;
    uint32_t           right_mm_s   : 10;
    uint32_t           command_id   : 8;
    motion_state_t     state        : 2;
    motion_status_t    reason       : 3;
    bool               b_stationary : 1;
} motion_snapshot_t;

typedef struct
{
    core_real_t left_distance_m;
    core_real_t right_distance_m;
    core_real_t elapsed_s;
} motion_speed_bucket_t;

typedef struct
{
    motion_speed_bucket_t buckets[MOTION_SPEED_BUCKETS];
    uint32_t              current;
} motion_speed_window_t;

typedef struct
{
    motion_config_t     config;
    motion_request_t    request;
    motion_output_t     output;
    core_pid_state_t    left_pid;
    core_pid_state_t    right_pid;
    motion_speed_window_t speed_window;
    core_real_t         progress;
    uint32_t            renewed_ms;
    uint32_t            started_ms;
    bool                b_initialized   : 1;
    bool                b_has_command   : 1;
    bool                b_stop_latched  : 1;
    bool                b_start_pending : 1;
} motion_t;

/** Initialize zero drive output. Invalid config leaves an inhibited instance.
 */
motion_status_t motion_init (motion_t * p_motion,
                             const motion_config_t * p_config);

/** Submit/renew a request. IDs use serial order modulo 2^MOTION_ID_BITS: an
 * ID 128 or more ahead (modulo 256) is stale. IDs cannot be zero.
 * A repeated running primitive ID renews without restarting. An identical
 * completed request succeeds as a no-op; its ID cannot restart the primitive.
 * A velocity request may replace its same-ID velocity values.
 * HALT is non-latching and always stops; it cannot clear a previous STOP.
 */
motion_status_t motion_submit (motion_t * p_motion,
                               const motion_request_t * p_request,
                               uint32_t now_ms);

/** Latch inhibition and zero commanded output immediately. */
void motion_stop (motion_t * p_motion, motion_status_t reason);

/** Explicitly acknowledge a stop only with independently confirmed conditions.
 * A new ID is still required after reset. Reset does not start the robot.
 */
motion_status_t motion_reset (motion_t * p_motion, bool b_stationary,
                              bool b_fault_clear);

/** Update odometry and wheel PID. Positive encoder deltas mean forward.
 * now_ms is a wrapping millisecond clock; call gaps must be below 2^31 ms.
 * dt_s comes from the actual sample interval, never a nominal period.
 * PID speed feedback averages 50--55 ms after warm-up, using bounded 5 ms
 * buckets. During warm-up it uses only elapsed observations. Counts spanning
 * bucket boundaries are apportioned uniformly over their actual interval.
 * Pose/progress, signed_distance_m and the stationary indication use raw
 * deltas. signed_distance_m is zero when the latest feedback is invalid;
 * stationary still requires independent confirmation/dwell before reset.
 * Submit a new request before step/applying its first duty. That first sample
 * updates pose but excludes pre-command counts from primitive progress.
 * Hardware must apply the returned zero duties even when status is non-OK.
 */
motion_status_t motion_step (motion_t * p_motion, uint32_t now_ms,
                             core_real_t dt_s, int32_t left_delta,
                             int32_t right_delta, motion_output_t * p_output);

/** Pack the shared Vehicle -> Motion request; NULL has no effect.
 * HALT packs target 0; VELOCITY packs 0 unless its target is non-finite.
 * An undefined kind packs as VELOCITY with the reserved target (INVALID).
 * Pack from the producer's own request: AWAY is not idempotent, so a
 * decoded value of 2.007 units or more can re-pack one unit higher.
 */
void motion_request_pack (const motion_request_t * p_request,
                          motion_request_packed_t * p_packed);

/** Unpack a shared request; reserved codes give NAN. NULL has no effect. */
void motion_request_unpack (const motion_request_packed_t * p_packed,
                            motion_request_t * p_request);

/** Pack the shared Motion -> all snapshot; NULL has no effect. */
void motion_snapshot_pack (const motion_output_t * p_output,
                           motion_snapshot_t * p_snapshot);

/** Unpack a shared snapshot. Unshared fields unpack as 0. NULL has no
 * effect.
 */
void motion_snapshot_unpack (const motion_snapshot_t * p_snapshot,
                             motion_output_t * p_output);

#endif /* MOTION_H */

/** End of file. */


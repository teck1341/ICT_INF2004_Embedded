/**
 * @file board.h
 * @brief Pico W / Robo Pico peripheral adapter. Single-core ownership only.
 * @details SDK peripherals share the existing micro T-Kernel startup. Motion
 * owns motor output and encoder reads; line owns ADC; IMU owns I2C; scanner
 * owns ultrasonic and servo. Encoder/echo ISR data is copied with PRIMASK.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "core.h"

/** Motor direction code: 2-bit two's complement of -1, 0 or +1 (3, 0, 1),
 * the same code as bitpack_signed(direction, BOARD_DIRECTION_BITS).
 */
#define BOARD_DIRECTION_BITS (2U)
#define BOARD_DIRECTION_MASK (3U)
/** Reserved code 2 (-2), as bitpack_invalid(2U): never prepared; BRAKEs. */
#define BOARD_DIRECTION_INVALID (2U)
/** Encode -1, 0 or +1 only. A macro keeps the constant mask visible to
 * -Wconversion when the result is assigned to a 2-bit field.
 */
#define BOARD_DIRECTION_ENCODE(direction) \
    (((uint32_t)(direction)) & BOARD_DIRECTION_MASK)

/** board_read_encoders() error bits: sticky per wheel until consumed. */
#define BOARD_ENCODER_ERROR_LEFT (1U)
#define BOARD_ENCODER_ERROR_RIGHT (2U)

/** Precomputed PWM commit, produced only by board_motor_prepare(). 31 bits:
 * levels are PWM compare values 0..6250 (13 bits each) and directions are
 * BOARD_DIRECTION_BITS codes, read with board_direction_decode(). A per-call
 * message of one task; it is never shared with an ISR.
 */
typedef struct
{
    uint32_t left_level      : 13;
    uint32_t right_level     : 13;
    uint32_t left_direction  : 2;
    uint32_t right_direction : 2;
    bool     b_valid         : 1;
} board_motor_command_t;

/** Sign-extend a direction code, equal to bitpack_extend(code, 2U): 3 is -1,
 * 0 is 0, 1 is +1 and the reserved 2 is -2. Branch-free integer work, so the
 * GPIO ISR and IRQ-masked regions use it; host doubles share the rule.
 */
static inline int32_t board_direction_decode (uint32_t code)
{
    /* Flipping the sign bit maps codes 0..3 to 2, 3, 0, 1: minus 2 is exact. */
    return (int32_t)((code & BOARD_DIRECTION_MASK) ^ 2U) - 2;
}

/** Acquisition status distinguishes normal sample phase from sensor faults. */
typedef enum
{
    BOARD_ACCEL_INVALID,
    BOARD_ACCEL_PENDING,
    BOARD_ACCEL_FRESH
} board_accel_status_t;

/** LSM303DLHC STATUS_REG_A ZYXDA (Table 35): a new XYZ set is available. */
#define BOARD_ACCEL_ZYXDA 0x08U
/** STATUS_REG_A ZYXOR: a newer XYZ set overwrote one that was never read. */
#define BOARD_ACCEL_ZYXOR 0x80U

/** Classify one STATUS_REG_A value before the data burst; pure, no I/O.
 * Neither flag is PENDING. ZYXDA is FRESH with or without ZYXOR: the sensor
 * runs on its own clock, so a 10 ms poll of the 100 Hz ODR routinely finds
 * two samples; the burst returns the newest sample (BDU keeps each axis's
 * MSB/LSB pair from one sample, datasheet p27), and the caller's elapsed dt
 * spans the overwritten sample. ZYXOR without ZYXDA is assumed not to occur
 * (datasheet Table 35 gives no clearing rule) and is INVALID, like a bus
 * error: a reboot-only hard IMU fault while active. That assumption,
 * cross-axis coherence of overrun samples and flag clearing are to be
 * confirmed on the bench (T-09). Host doubles share this function so
 * application tests use the production rule.
 */
static inline board_accel_status_t board_accel_classify (uint8_t status)
{
    board_accel_status_t result = BOARD_ACCEL_INVALID;

    if (0U == (status & (BOARD_ACCEL_ZYXDA | BOARD_ACCEL_ZYXOR)))
    {
        result = BOARD_ACCEL_PENDING;
    }
    else if (0U != (status & BOARD_ACCEL_ZYXDA))
    {
        result = BOARD_ACCEL_FRESH;
    }
    else
    {
        /* Overrun without new-data readiness: assumed inconsistent (T-09). */
    }
    return result;
}

/** Kernel startup calls once after selecting the 12 MHz reference clock.
 * Release TIMER reset and explicitly provide its 1 MHz tick; never reset
 * the running counter. No RTOS calls or initialized globals are required.
 */
void board_boot_timebase (void);

/** Configure motor pins and PWM LOW after C runtime initialization.
 * Safe before board_init(); used if radio task startup prevents usermain.
 * Actual electrical braking and power/reset behavior require bench proof.
 */
void board_boot_brake (void);

/** Arm once after successful task startup, timeout 1..8000 milliseconds.
 * RP2040-E1 requires two load counts per microsecond. Debugging does not
 * pause the watchdog. False means not armed. Expiry resets hardware and
 * restarts normal flash boot; electrical stop behavior requires bench proof.
 */
bool board_watchdog_start (uint32_t timeout_ms);

/** Safety task alone reloads the watchdog after checking required heartbeats.
 * Does nothing before successful board_watchdog_start(). Never feed in ISR.
 */
void board_watchdog_feed (void);

/** True when the hardware reports a watchdog timer expiry at the last reset. */
bool board_watchdog_rebooted (void);

/** Initialise safe motor outputs and sensors. False means remain inhibited. */
bool board_init (void);

/** Last initialization result as a static diagnostic string; no allocation. */
const char * board_init_diagnostic (void);

/** Return the UART0 console pins, which the HC-SR04 uses after start-up
 * (ROBOT_PIN_CONSOLE_TX/RX), to UART0 so a start-up refusal can print its
 * reason. Call only after braking with interrupts masked; the sonar then
 * stops working until reboot.
 */
void board_console_restore (void);

/** Read the monotonic, coherent RP2040 timer in microseconds. */
uint64_t board_time_us (void);

/** Copy and consume signed encoder deltas and the sticky edge-error bits.
 * Null output pointers are ignored; counts are consumed only when all exist.
 * Must be called by one motion owner. Deltas and phases share one 16-bit
 * ISR word, the sticky error bits one byte; deltas and errors are read and
 * cleared in one IRQ-masked region and the phases stay, so the next edge
 * decodes from them.
 * Each delta holds +/-31 counts (6 bits). The 0.15 m/s cap is about 1.3
 * counts per 5 ms read and 6.6 per 25 ms; 31 counts is 17.6 mm, over
 * 3.5 m/s at a 5 ms read. A further count leaves the delta saturated and
 * sets that wheel's error bit; *p_errors holds BOARD_ENCODER_ERROR_LEFT and
 * BOARD_ENCODER_ERROR_RIGHT bits, never a count.
 * Multiple pending edge bits on either wheel report an error with no guessed
 * displacement, then resynchronize that wheel to the observed input phase.
 * A single edge must match the sampled phase change, pin and polarity.
 * Optional one-channel mode counts both A edges using applied motor demand;
 * B is ignored. Braked/unknown-direction edges report errors, no displacement.
 * This mode cannot observe rolling opposite to demand or free-wheel direction.
 * Hardware edge capture and worst-case IRQ latency still need validation.
 */
void board_read_encoders (int32_t * p_left, int32_t * p_right,
                          uint32_t * p_errors);

/** Discard deltas and error bits counted before the first consumer read.
 * The GPIO ISR runs from board_init(), so boot motion or input settling
 * could otherwise latch a false saturation or edge error into the first
 * board_read_encoders(). Phases and totals are kept. Call once at the end
 * of application initialization, before the motion task runs.
 */
void board_encoders_discard (void);

/** Snapshot cumulative decoded position, without consuming deltas.
 * Values are signed positions represented modulo 2^16 (two halves of one
 * ISR word): a forward edge adds one, reverse subtracts one. Subtract
 * snapshots modulo 2^16 with a 0x8000 half-range (18.6 m at 0.567 mm per
 * count). Polarity is raw; apply confirmed signs outside the ISR. Multiple
 * readers may independently subtract their snapshots. Null pointers leave
 * both outputs unchanged. One GPIO ISR writes totals.
 */
void board_encoder_totals (uint16_t * p_left, uint16_t * p_right);

/** Read ADC0/1/2 into three 0..4095 samples. Null returns false. */
bool board_read_ir (uint16_t samples[3]);

/** Read a fresh unsaturated XYZ sample, milligravity, LSM303 sensor axes.
 * PENDING means no new sample yet; do not consume output or refresh its age.
 * FRESH includes an overrun (ZYXOR with ZYXDA): the newest sample (BDU
 * protects each axis's byte pair; cross-axis coherence is unconfirmed, T-09).
 * Overruns are not counted or reported; bench logging needs instrumentation.
 * INVALID means uninitialized/null, bus error, an assumed-inconsistent status
 * (ZYXOR without ZYXDA, T-09) or saturation. Every flagged status drains the
 * BDU latch.
 * Six-face scale/bias and mounting transform belong to terrain configuration.
 */
board_accel_status_t board_read_accel (int16_t milligravity[3]);

/** Issue one >=10 us pulse if no request is pending and >=65 ms elapsed.
 * Interrupts remain enabled during the short pulse. False means not issued.
 */
bool board_ultrasonic_trigger (void);

/** Advance the timeout state; call from the scanner task every 2 ms. */
void board_ultrasonic_service (void);

/** Consume completed echo or timeout. False means not yet complete.
 * A returned result with *p_valid false is UNKNOWN, never free space.
 */
bool board_ultrasonic_result (uint32_t * p_pulse_us, bool * p_valid);

/** Command validated servo pulse; false for unconfirmed/out-of-range values. */
bool board_servo_us (uint16_t pulse_us);

/** Command signed duty [-1,1]. Unconfirmed/nonfinite input forces BRAKE.
 * Switching polarity inserts a zero-output call before opposite drive.
 */
void board_motor_duty (core_real_t left_duty, core_real_t right_duty);

/** Prepare finite capped duty outside any IRQ-masked region.
 * False or a null output means no valid commit; otherwise levels and signs
 * contain integer-only hardware demand. Invalid input produces b_valid=false.
 */
bool board_motor_prepare (core_real_t left_duty, core_real_t right_duty,
                          board_motor_command_t * p_command);

/** Commit precomputed demand with integer operations only.
 * Caller holds app_main_lock while rechecking latest software inhibition.
 * Physical STOP, calibration and wiring are checked again here. Null or
 * invalid commands BRAKE. The function also masks locally for safe reuse.
 */
void board_motor_commit (const board_motor_command_t * p_command);

/** Immediate low/low BRAKE, including on fault before scheduler starts. */
void board_motor_brake (void);

/** Read GP20 STOP (internal pull-up), assumed active-low: measure the button
 * polarity at commissioning stage 1, because an active-high button would
 * never register. Mechanical debouncing belongs to the app.
 */
bool board_stop_pressed (void);

/** Read GP21 START (internal pull-up), assumed active-low like STOP.
 * Mechanical debouncing belongs to the app.
 */
bool board_start_pressed (void);

#endif /* BOARD_H */

/* End of file. */


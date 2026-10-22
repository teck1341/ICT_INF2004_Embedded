/**
 * @file board.c
 * @brief Pico SDK peripheral subset hosted by the pinned micro T-Kernel BSP.
 * @details No SDK startup, interrupt dispatcher, scheduler or spinlocks are
 * linked. The kernel installs GPIO IRQ 13 on core 0. D-ADAPTER: SDK ABI names,
 * native SDK integer types and compiler intrinsics stay in this adapter.
 * Verify edge loss, current transients and timing on actual hardware.
 */
#include "board.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

#include "app_main.h"
#include "robot_config.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/resets.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/timer.h"
#include "hardware/structs/watchdog.h"

#define BOARD_ACCEL_ADDRESS 0x19U
#define BOARD_ACCEL_CTRL1 0x20U
#define BOARD_ACCEL_CTRL4 0x23U
#define BOARD_ACCEL_STATUS 0x27U
#define BOARD_ACCEL_DATA 0xA8U
#define BOARD_ACCEL_ODR_ENABLE 0x57U
#define BOARD_ACCEL_BDU_HR_2G 0x88U
#define BOARD_ACCEL_AXES 3U
#define BOARD_ACCEL_BYTES 6U
#define BOARD_ADC_TIMEOUT_US 100U
/* IO_BANK0 latches four status bits per GPIO and eight GPIOs per bank. */
#define BOARD_IRQ_BANK(pin) ((pin) / 8U)
#define BOARD_IRQ_SHIFT(pin) (((pin) % 8U) * 4U)
#define BOARD_EDGE_BITS(pin) (0xCU << BOARD_IRQ_SHIFT(pin))
#define BOARD_LEFT_BANK BOARD_IRQ_BANK(ROBOT_PIN_ENCODER_LEFT_A)
#define BOARD_RIGHT_BANK BOARD_IRQ_BANK(ROBOT_PIN_ENCODER_RIGHT_A)
#define BOARD_ECHO_BANK BOARD_IRQ_BANK(ROBOT_PIN_ECHO)
#if ROBOT_ENCODER_CHANNELS == 2
#define BOARD_LEFT_MASK (BOARD_EDGE_BITS(ROBOT_PIN_ENCODER_LEFT_A) | \
                         BOARD_EDGE_BITS(ROBOT_PIN_ENCODER_LEFT_B))
#define BOARD_RIGHT_MASK (BOARD_EDGE_BITS(ROBOT_PIN_ENCODER_RIGHT_A) | \
                          BOARD_EDGE_BITS(ROBOT_PIN_ENCODER_RIGHT_B))
#else
#define BOARD_LEFT_MASK BOARD_EDGE_BITS(ROBOT_PIN_ENCODER_LEFT_A)
#define BOARD_RIGHT_MASK BOARD_EDGE_BITS(ROBOT_PIN_ENCODER_RIGHT_A)
#endif
#define BOARD_ECHO_MASK BOARD_EDGE_BITS(ROBOT_PIN_ECHO)
/* Pending bits of a source latched in bank `source`, if that is `bank`. */
#define BOARD_SAME_BANK(bank, source, pending) \
    (((bank) == (source)) ? (pending) : 0U)
#if (BOARD_IRQ_BANK(ROBOT_PIN_ENCODER_LEFT_A) != \
     BOARD_IRQ_BANK(ROBOT_PIN_ENCODER_LEFT_B)) || \
    (BOARD_IRQ_BANK(ROBOT_PIN_ENCODER_RIGHT_A) != \
     BOARD_IRQ_BANK(ROBOT_PIN_ENCODER_RIGHT_B))
#error "Both encoder inputs of one wheel must share an IO_BANK0 bank"
#endif
/* The boot console drives its TX pin, so only TRIG (a sensor input) may
 * share it; ECHO (a sensor output) may share only the console RX pin.
 */
#if (ROBOT_PIN_ECHO == ROBOT_PIN_CONSOLE_TX) || \
    (ROBOT_PIN_TRIGGER == ROBOT_PIN_CONSOLE_RX)
#error "HC-SR04 on the console pins needs TRIG on TX and ECHO on RX"
#endif
/* RP2040 I2C pairs: SDA on GPIO 4k is I2C0, on 4k + 2 is I2C1; SCL is the
 * next GPIO. The accelerometer's controller follows its configured pins.
 */
#if (ROBOT_PIN_IMU_SCL != (ROBOT_PIN_IMU_SDA + 1U))
#error "IMU SCL must be the GPIO after SDA"
#elif ((ROBOT_PIN_IMU_SDA % 4U) == 0U)
#define BOARD_I2C i2c0
#elif ((ROBOT_PIN_IMU_SDA % 4U) == 2U)
#define BOARD_I2C i2c1
#else
#error "IMU SDA must be an RP2040 I2C SDA pin"
#endif
#define BOARD_MOTOR_SLICE_LEFT 4U
#define BOARD_MOTOR_SLICE_RIGHT 5U
#define BOARD_SERVO_SLICE 6U
/* RP2040 GPIO n drives PWM slice (n >> 1) & 7, channel A when n is even.
 * Motor setup loops LEFT_A..RIGHT_B; board_apply_motor_locked drives A then B.
 */
#define BOARD_PWM_SLICE(pin) (((pin) >> 1U) & 7U)
#if (BOARD_PWM_SLICE(ROBOT_PIN_MOTOR_LEFT_A) != BOARD_MOTOR_SLICE_LEFT) || \
    (BOARD_PWM_SLICE(ROBOT_PIN_MOTOR_RIGHT_A) != BOARD_MOTOR_SLICE_RIGHT) || \
    (0U != (ROBOT_PIN_MOTOR_LEFT_A & 1U)) || \
    (0U != (ROBOT_PIN_MOTOR_RIGHT_A & 1U)) || \
    (ROBOT_PIN_MOTOR_LEFT_B != (ROBOT_PIN_MOTOR_LEFT_A + 1U)) || \
    (ROBOT_PIN_MOTOR_RIGHT_A != (ROBOT_PIN_MOTOR_LEFT_B + 1U)) || \
    (ROBOT_PIN_MOTOR_RIGHT_B != (ROBOT_PIN_MOTOR_RIGHT_A + 1U))
#error "Motor pins must be contiguous A/B channel pairs of PWM slices 4 and 5"
#endif
#if (BOARD_PWM_SLICE(ROBOT_PIN_SERVO) != BOARD_SERVO_SLICE) || \
    (0U != (ROBOT_PIN_SERVO & 1U))
#error "Servo pin must be channel A of PWM slice 6"
#endif
#if (ROBOT_PWM_WRAP != ((ROBOT_CLOCK_HZ / ROBOT_PWM_HZ) - 1U)) || \
    (ROBOT_CLOCK_HZ != (125U * 1000000U))
#error "Motor wrap and the servo divider of 125 assume a 125 MHz clock"
#endif
#define BOARD_ECHO_MIN_US 100U
#define BOARD_ECHO_MAX_US 25000U
#define BOARD_WATCHDOG_MAX_MS 8000U
#define BOARD_WATCHDOG_COUNTS_MS 2000U
/* Encoder ISR word: left delta bits 0-5, right delta bits 6-11 (two's
 * complement, saturating at +/-31, so code 0x20 is never stored), left
 * phase bits 12-13, right phase bits 14-15 (phase = A << 1 | B). Wheel 0 is
 * left and wheel 1 right: a wheel's delta shifts by 6 * wheel, its phase by
 * 12 + 2 * wheel, its total by 16 * wheel and its error bit is 1 << wheel.
 */
#define BOARD_LEFT 0U
#define BOARD_RIGHT 1U
#define BOARD_DELTA_MASK 0x3FU
#define BOARD_DELTA_SIGN 0x20U
#define BOARD_DELTA_MAX 31
#define BOARD_PHASE_SHIFT 12U
#define BOARD_PHASE_BITS 0xF000U
#define BOARD_ENCODER_BITS 0xFFFFU
/* Sticky edge-error byte: bit 0 left, bit 1 right (1 << wheel). */
#define BOARD_ENCODER_ERRORS \
    (BOARD_ENCODER_ERROR_LEFT | BOARD_ENCODER_ERROR_RIGHT)
/* Totals word: left modulo-2^16 total in bits 0-15, right in bits 16-31. */
#define BOARD_TOTAL_MASK 0xFFFFU
/* Task-written motor direction byte: left code bits 0-1, right bits 2-3. */
#define BOARD_LEFT_DIRECTION_SHIFT 0U
#define BOARD_RIGHT_DIRECTION_SHIFT 2U
#define BOARD_DIRECTIONS_MASK 0xFU
/* Motor command levels are PWM compare values 0..6250 in 13 bits. */
#define BOARD_LEVEL_MASK 0x1FFFU
#if (ROBOT_PWM_WRAP + 1U) > BOARD_LEVEL_MASK
#error "Motor levels up to the PWM wrap must fit the 13-bit command field"
#endif
/* Echo word: state bits 0-2, completed pulse in microseconds bits 3-17. */
#define BOARD_ECHO_STATE_MASK 0x7U
#define BOARD_ECHO_PULSE_SHIFT 3U
#define BOARD_ECHO_PULSE_MASK 0x7FFFU
#if ROBOT_ECHO_TIMEOUT_US > BOARD_ECHO_PULSE_MASK
#error "Echo pulses up to the timeout must fit the 15-bit pulse field"
#endif

typedef enum
{
    BOARD_ECHO_IDLE,
    BOARD_ECHO_WAIT_RISE,
    BOARD_ECHO_WAIT_FALL,
    BOARD_ECHO_READY,
    BOARD_ECHO_TIMEOUT
} board_echo_state_t;

static bool                         gb_initialized;
static bool                         gb_accel_initialized;
static const char *                gp_init_diagnostic = "not initialized";
static uint16_t                     g_motor_level_limit;
static uint32_t                     g_watchdog_reload;
/* Tasks write directions only inside app_main_lock(); the ISR reads them. */
static volatile uint8_t             g_motor_directions;
/* The GPIO ISR stores each word once per edge batch and only sets error
 * bits; tasks read and clear them only inside app_main_lock().
 */
static volatile uint16_t            g_encoder_word;
static volatile uint32_t            g_encoder_totals;
static volatile uint8_t             g_encoder_errors;
static volatile uint32_t            g_echo_word;
/* ISR-private rise time: the low timer word, subtracted modulo 2^32. */
static uint32_t                     g_echo_rise_us;
static uint64_t                     g_trigger_time_us;
static bool                         gb_triggered_once;

static void board_gpio_isr (uint32_t interrupt_number);
static uint32_t board_count_batch (uint32_t left_pending,
                                   uint32_t right_pending,
                                   uint32_t input_levels,
                                   uint32_t * p_encoder,
                                   uint32_t * p_totals);
static void board_record_error (uint32_t errors);
static void board_echo_edge (uint32_t echo_pending, uint32_t input_levels,
                             uint32_t current_us);
static uint32_t board_wheel_edges (uint32_t pending, uint32_t pin_a,
                                   uint32_t pin_b);
static uint32_t board_wheel_phase (uint32_t input_levels, uint32_t pin_a,
                                   uint32_t pin_b);
static void board_init_motor (void);
static void board_init_inputs (void);
static bool board_init_accel (void);
static bool board_read_registers (uint8_t reg, uint8_t * p_data,
                                  size_t count);
static bool board_write_register (uint8_t reg, uint8_t value);
static uint32_t board_count_wheel (uint32_t wheel, uint32_t pending,
                                   uint32_t current, int32_t direction,
                                   uint32_t * p_encoder,
                                   uint32_t * p_totals);
static int32_t board_count_encoder (uint32_t pending, uint32_t current,
                                    uint32_t previous, int32_t direction);
#if ROBOT_ENCODER_CHANNELS == 2
static int32_t board_count_transition (uint32_t pending, uint32_t current,
                                       uint32_t previous);
#endif
static uint32_t board_add_count (uint32_t wheel, int32_t increment,
                                 uint32_t * p_encoder, uint32_t * p_totals);
static int32_t board_delta (uint32_t encoder, uint32_t wheel);
static void board_apply_motor_locked (uint32_t slice, int32_t direction,
                                      uint16_t level, uint32_t shift);
static void board_store_direction_locked (uint32_t shift, int32_t direction);
static uint16_t board_duty_level (core_real_t duty);
static uint32_t board_echo_word (board_echo_state_t state, uint32_t pulse_us);
static board_echo_state_t board_echo_state (uint32_t echo);

void board_boot_timebase (void)
{
    /* Same tick programming as SDK 2.2 hardware_ticks/ticks.c on RP2040.
     * This enables the tick generator, not the watchdog reset mechanism.
     */
    watchdog_hw->tick = 12U | WATCHDOG_TICK_ENABLE_BITS;
    unreset_block_num_wait_blocking(RESET_TIMER);
}

bool board_watchdog_start (uint32_t timeout_ms)
{
    uint32_t saved;
    bool b_started;

    if ((!gb_initialized) || (0U != g_watchdog_reload) || (0U == timeout_ms) ||
        (timeout_ms > BOARD_WATCHDOG_MAX_MS))
    {
        return false;
    }
    saved = app_main_lock();
    /* SDK 2.2 watchdog.c uses this domain selection and RP2040-E1 factor.
     * 8000 * 2000 = 16000000 fits the 24-bit LOAD field and uint32_t.
     */
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    hw_set_bits(&psm_hw->wdsel,
        PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS));
    hw_clear_bits(&watchdog_hw->ctrl,
        WATCHDOG_CTRL_PAUSE_DBG0_BITS | WATCHDOG_CTRL_PAUSE_DBG1_BITS |
        WATCHDOG_CTRL_PAUSE_JTAG_BITS);
    /* Clear any boot-ROM address-redirect magic: restart normal flash. */
    watchdog_hw->scratch[4] = 0U;
    g_watchdog_reload = timeout_ms * BOARD_WATCHDOG_COUNTS_MS;
    watchdog_hw->load = g_watchdog_reload;
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    b_started = (0U != (watchdog_hw->ctrl & WATCHDOG_CTRL_ENABLE_BITS));
    if (!b_started)
    {
        g_watchdog_reload = 0U;
    }
    app_main_unlock(saved);
    return b_started;
}

void board_watchdog_feed (void)
{
    if (0U != g_watchdog_reload)
    {
        watchdog_hw->load = g_watchdog_reload;
    }
}

bool board_watchdog_rebooted (void)
{
    return (0U != (watchdog_hw->reason & WATCHDOG_REASON_TIMER_BITS));
}

void board_boot_brake (void)
{
    uint32_t saved = app_main_lock();

    board_init_motor();
    board_motor_brake();
    app_main_unlock(saved);
}

bool board_init (void)
{
    bool b_duty_valid = (isfinite(ROBOT_MAX_DUTY)) &&
                        (ROBOT_MAX_DUTY > 0.0) &&
                        (ROBOT_MAX_DUTY <= 1.0);

    if (b_duty_valid)
    {
        g_motor_level_limit = board_duty_level(1.0);
    }
    board_boot_brake();
    gp_init_diagnostic = "invalid maximum motor duty";
    if (!b_duty_valid)
    {
        return false;
    }
    gp_init_diagnostic = "ROBOT_WIRING_CONFIRMED=0; review wiring";
    if (0 == ROBOT_WIRING_CONFIRMED)
    {
        return false;
    }
#if ROBOT_ENCODER_CHANNELS == 1
    gp_init_diagnostic = "single-channel direction/counts not commissioned";
    if ((0 == ROBOT_SINGLE_CHANNEL_DIRECTION_CONFIRMED) ||
        (0U == ROBOT_ENCODER_COUNTS_PER_REV))
    {
        return false;
    }
#endif
    board_init_inputs();
    gp_init_diagnostic = "GPIO interrupt registration failed";
    if (!app_main_install_gpio_irq(board_gpio_isr))
    {
        return false;
    }
    io_bank0_hw->intr[BOARD_LEFT_BANK] = BOARD_LEFT_MASK;
    io_bank0_hw->intr[BOARD_RIGHT_BANK] = BOARD_RIGHT_MASK;
    io_bank0_hw->intr[BOARD_ECHO_BANK] = BOARD_ECHO_MASK;
    io_bank0_hw->proc0_irq_ctrl.inte[BOARD_LEFT_BANK] |= BOARD_LEFT_MASK;
    io_bank0_hw->proc0_irq_ctrl.inte[BOARD_RIGHT_BANK] |= BOARD_RIGHT_MASK;
    io_bank0_hw->proc0_irq_ctrl.inte[BOARD_ECHO_BANK] |= BOARD_ECHO_MASK;
    adc_init();
    adc_gpio_init(ROBOT_PIN_IR_LEFT);
    adc_gpio_init(ROBOT_PIN_IR_RIGHT);
    adc_gpio_init(ROBOT_PIN_IR_BARCODE);
    gb_accel_initialized = board_init_accel();
    gb_initialized = true;
    gp_init_diagnostic = gb_accel_initialized ? "ready" :
                         "accelerometer configuration/readback failed";
    return gb_accel_initialized;
}

const char * board_init_diagnostic (void)
{
    return gp_init_diagnostic;
}

uint64_t board_time_us (void)
{
    uint32_t high_before;
    uint32_t low_part;
    uint32_t high_after;

    do
    {
        high_before = timer_hw->timerawh;
        low_part = timer_hw->timerawl;
        high_after = timer_hw->timerawh;
    } while (high_before != high_after);
    /* Widening preserves all 32 bits before the upper-word shift. */
    return (((uint64_t)high_before) << 32U) | low_part;
}

void board_read_encoders (int32_t * p_left, int32_t * p_right,
                          uint32_t * p_errors)
{
    uint32_t saved;
    uint32_t encoder;
    uint32_t errors;

    if ((NULL == p_left) || (NULL == p_right) || (NULL == p_errors))
    {
        return;
    }
    /* Read and clear in one masked region, so no edge can fall between the
     * read and the clear (M0+ has no exclusive access). Phases are kept.
     */
    saved = app_main_lock();
    encoder = g_encoder_word;
    errors = g_encoder_errors;
    g_encoder_word = encoder & BOARD_PHASE_BITS;
    g_encoder_errors = 0U;
    app_main_unlock(saved);
    *p_left = board_delta(encoder, BOARD_LEFT);
    *p_right = board_delta(encoder, BOARD_RIGHT);
    *p_errors = errors;
}

void board_encoders_discard (void)
{
    uint32_t saved = app_main_lock();

    /* Phases and totals stay, so the next edge decodes from the live phase
     * and position readers keep their reference.
     */
    g_encoder_word = g_encoder_word & BOARD_PHASE_BITS;
    g_encoder_errors = 0U;
    app_main_unlock(saved);
}

void board_encoder_totals (uint16_t * p_left, uint16_t * p_right)
{
    uint32_t saved;
    uint32_t totals;

    if ((NULL == p_left) || (NULL == p_right))
    {
        return;
    }
    saved = app_main_lock();
    totals = g_encoder_totals;
    app_main_unlock(saved);
    /* Each 16-bit half is one wheel's total, so both narrowings are exact. */
    *p_left = (uint16_t)(totals & BOARD_TOTAL_MASK);
    *p_right = (uint16_t)(totals >> 16U);
}

bool board_read_ir (uint16_t samples[3])
{
    uint32_t channel;
    uint64_t deadline;

    if ((NULL == samples) || (!gb_initialized))
    {
        return false;
    }
    for (channel = 0U; channel < BOARD_ACCEL_AXES; channel++)
    {
        adc_select_input(channel);
        hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS);
        deadline = board_time_us() + BOARD_ADC_TIMEOUT_US;
        while (0U == (adc_hw->cs & ADC_CS_READY_BITS))
        {
            if (board_time_us() >= deadline)
            {
                return false;
            }
        }
        if (0U != (adc_hw->cs & ADC_CS_ERR_BITS))
        {
            return false;
        }
        /* ADC result is a 12-bit unsigned value, masked before narrowing. */
        samples[channel] = (uint16_t)(adc_hw->result & 0xFFFU);
    }
    return true;
}

board_accel_status_t board_read_accel (int16_t milligravity[3])
{
    uint8_t  status;
    uint8_t  bytes[BOARD_ACCEL_BYTES];
    uint32_t axis;
    uint32_t raw_sample;
    int32_t  signed_sample;
    bool     b_valid;
    board_accel_status_t sample;

    if ((NULL == milligravity) || (!gb_accel_initialized))
    {
        return BOARD_ACCEL_INVALID;
    }
    if (!board_read_registers(BOARD_ACCEL_STATUS, &status, 1U))
    {
        return BOARD_ACCEL_INVALID;
    }
    sample = board_accel_classify(status);
    if (BOARD_ACCEL_PENDING == sample)
    {
        return BOARD_ACCEL_PENDING;
    }
    /* An overrun (ZYXOR with ZYXDA) is FRESH: the burst returns the newest
     * sample; BDU keeps each axis's byte pair together (cross-axis coherence
     * and flag clearing: T-09). Read after every flagged status to release
     * the latch.
     */
    b_valid = (BOARD_ACCEL_FRESH == sample);
    if (!board_read_registers(BOARD_ACCEL_DATA, bytes, BOARD_ACCEL_BYTES))
    {
        return BOARD_ACCEL_INVALID;
    }
    for (axis = 0U; axis < BOARD_ACCEL_AXES; axis++)
    {
        /* Both byte values widen exactly into a 16-bit packed sample. */
        raw_sample = ((uint32_t)bytes[2U * axis + 1U]) << 8U;
        raw_sample |= bytes[2U * axis];
        raw_sample >>= 4U;
        /* raw_sample <= 4095; subtraction produces -2048..2047 exactly. */
        signed_sample = (int32_t)raw_sample;
        if (raw_sample >= 2048U)
        {
            signed_sample -= 4096;
        }
        if ((signed_sample <= -2048) || (signed_sample >= 2047))
        {
            b_valid = false;
        }
        /* Signed 12-bit range fits int16_t; HR +/-2g gives 1 mg/LSB. */
        milligravity[axis] = (int16_t)signed_sample;
    }
    return b_valid ? BOARD_ACCEL_FRESH : BOARD_ACCEL_INVALID;
}

bool board_ultrasonic_trigger (void)
{
    uint64_t current_us = board_time_us();
    uint32_t saved;

    if ((!gb_initialized) ||
        (gb_triggered_once &&
         ((current_us - g_trigger_time_us) < ROBOT_TRIGGER_INTERVAL_US)))
    {
        return false;
    }
    saved = app_main_lock();
    if (BOARD_ECHO_IDLE != board_echo_state(g_echo_word))
    {
        app_main_unlock(saved);
        return false;
    }
    if (gpio_get(ROBOT_PIN_ECHO))
    {
        g_echo_word = board_echo_word(BOARD_ECHO_TIMEOUT, 0U);
        app_main_unlock(saved);
        return false;
    }
    g_echo_word = board_echo_word(BOARD_ECHO_WAIT_RISE, 0U);
    g_trigger_time_us = current_us;
    gb_triggered_once = true;
    app_main_unlock(saved);
    gpio_put(ROBOT_PIN_TRIGGER, true);
    current_us = board_time_us();
    /* No RTOS delay: wait only for pulse minimum, with IRQs still enabled.
     * The start read may fall late in its 1 us tick, so wait one extra tick:
     * the pulse is then at least 10 us (HC-SR04). Preemption can extend it;
     * measure the width on the board.
     */
    while ((board_time_us() - current_us) <= ROBOT_TRIGGER_PULSE_US)
    {
        tight_loop_contents();
    }
    gpio_put(ROBOT_PIN_TRIGGER, false);
    return true;
}

void board_ultrasonic_service (void)
{
    uint64_t current_us = board_time_us();
    uint32_t saved = app_main_lock();
    board_echo_state_t state = board_echo_state(g_echo_word);

    if (((BOARD_ECHO_WAIT_RISE == state) ||
         (BOARD_ECHO_WAIT_FALL == state)) &&
        ((current_us - g_trigger_time_us) >= ROBOT_ECHO_TIMEOUT_US))
    {
        g_echo_word = board_echo_word(BOARD_ECHO_TIMEOUT, 0U);
    }
    app_main_unlock(saved);
}

bool board_ultrasonic_result (uint32_t * p_pulse_us, bool * p_valid)
{
    uint32_t saved;
    uint32_t echo;
    uint32_t pulse_us;
    board_echo_state_t state;

    if ((NULL == p_pulse_us) || (NULL == p_valid))
    {
        return false;
    }
    saved = app_main_lock();
    echo = g_echo_word;
    state = board_echo_state(echo);
    if ((BOARD_ECHO_READY != state) && (BOARD_ECHO_TIMEOUT != state))
    {
        app_main_unlock(saved);
        return false;
    }
    /* State and pulse come from one word, so they are always one result. */
    pulse_us = (echo >> BOARD_ECHO_PULSE_SHIFT) & BOARD_ECHO_PULSE_MASK;
    *p_valid = ((BOARD_ECHO_READY == state) &&
                (pulse_us >= BOARD_ECHO_MIN_US) &&
                (pulse_us <= BOARD_ECHO_MAX_US));
    *p_pulse_us = *p_valid ? pulse_us : 0U;
    g_echo_word = board_echo_word(BOARD_ECHO_IDLE, 0U);
    app_main_unlock(saved);
    return true;
}

bool board_servo_us (uint16_t pulse_us)
{
    if ((!gb_initialized) || (0 == ROBOT_SERVO_CONFIRMED) ||
        (pulse_us < ROBOT_SERVO_MIN_US) ||
        (pulse_us > ROBOT_SERVO_MAX_US))
    {
        return false;
    }
    pwm_set_chan_level(BOARD_SERVO_SLICE, PWM_CHAN_A, pulse_us);
    return true;
}

void board_motor_duty (core_real_t left_duty, core_real_t right_duty)
{
    board_motor_command_t command = {0};

    if (!board_motor_prepare(left_duty, right_duty, &command))
    {
        board_motor_brake();
        return;
    }
    board_motor_commit(&command);
}

bool board_motor_prepare (core_real_t left_duty, core_real_t right_duty,
                          board_motor_command_t * p_command)
{
    uint16_t left_level;
    uint16_t right_level;
    int32_t  left_direction;
    int32_t  right_direction;

    if (NULL == p_command)
    {
        return false;
    }
    *p_command = (board_motor_command_t){0};

    if ((!gb_initialized) || (0 == ROBOT_CALIBRATION_CONFIRMED) ||
        (0 == ROBOT_WIRING_CONFIRMED) ||
        (!isfinite(left_duty)) || (!isfinite(right_duty)) ||
        (fabs(left_duty) > 1.0) || (fabs(right_duty) > 1.0))
    {
        return false;
    }
    /* Soft floating-point work stays outside the short GPIO critical region. */
    left_duty *= ROBOT_LEFT_MOTOR_SIGN;
    right_duty *= ROBOT_RIGHT_MOTOR_SIGN;
    left_level = board_duty_level(left_duty);
    right_level = board_duty_level(right_duty);
    left_direction = (left_duty > 0.0) ? 1 : -1;
    right_direction = (right_duty > 0.0) ? 1 : -1;
    if (0U == left_level)
    {
        left_direction = 0;
    }
    if (0U == right_level)
    {
        right_direction = 0;
    }
    /* |duty| <= 1 bounds levels to 6250, which the #if check proves fits
     * 13 bits, so the masks are exact; directions -1, 0, +1 become codes.
     */
    p_command->left_level = left_level & BOARD_LEVEL_MASK;
    p_command->right_level = right_level & BOARD_LEVEL_MASK;
    p_command->left_direction = BOARD_DIRECTION_ENCODE(left_direction);
    p_command->right_direction = BOARD_DIRECTION_ENCODE(right_direction);
    p_command->b_valid = true;
    return true;
}

void board_motor_commit (const board_motor_command_t * p_command)
{
    uint32_t saved = app_main_lock();

    if ((NULL == p_command) || (!p_command->b_valid) ||
        (!gb_initialized) || (0 == ROBOT_CALIBRATION_CONFIRMED) ||
        (0 == ROBOT_WIRING_CONFIRMED) || board_stop_pressed())
    {
        board_motor_brake();
        app_main_unlock(saved);
        return;
    }
    /* A 2-bit code holds -2..1: only the reserved code is out of range. */
    if ((p_command->left_level > g_motor_level_limit) ||
        (p_command->right_level > g_motor_level_limit) ||
        (BOARD_DIRECTION_INVALID == p_command->left_direction) ||
        (BOARD_DIRECTION_INVALID == p_command->right_direction))
    {
        board_motor_brake();
        app_main_unlock(saved);
        return;
    }
    board_apply_motor_locked(BOARD_MOTOR_SLICE_LEFT,
        board_direction_decode(p_command->left_direction),
        p_command->left_level, BOARD_LEFT_DIRECTION_SHIFT);
    board_apply_motor_locked(BOARD_MOTOR_SLICE_RIGHT,
        board_direction_decode(p_command->right_direction),
        p_command->right_level, BOARD_RIGHT_DIRECTION_SHIFT);
    app_main_unlock(saved);
}

void board_motor_brake (void)
{
    uint32_t saved = app_main_lock();

    pwm_set_both_levels(BOARD_MOTOR_SLICE_LEFT, 0U, 0U);
    pwm_set_both_levels(BOARD_MOTOR_SLICE_RIGHT, 0U, 0U);
    /* Both wheels braked: code 0 for each, one store. */
    g_motor_directions = 0U;
    app_main_unlock(saved);
}

bool board_stop_pressed (void)
{
    return !gpio_get(ROBOT_PIN_STOP);
}

bool board_start_pressed (void)
{
    return !gpio_get(ROBOT_PIN_START);
}

#if !TM_WIFI_CYW43
/* D-ADAPTER: names below are required Pico SDK ABI hooks, not robot APIs.
 * The Wi-Fi profile already supplies these in cyw43_utk_compat.c.
 */
/** Return frequencies configured by the pinned BSP, without changing them. */
uint32_t clock_get_hz (clock_handle_t clock)
{
    if ((clk_sys == clock) || (clk_peri == clock))
    {
        return ROBOT_CLOCK_HZ;
    }
    if ((clk_adc == clock) || (clk_usb == clock))
    {
        return 48000000U;
    }
    return 0U;
}

/** Supply the SDK's timestamp hook using the coherent free-running timer. */
uint64_t time_us_64 (void)
{
    return board_time_us();
}
#endif

/** Initialise four motor PWM outputs low before selecting PWM function. */
static void board_init_motor (void)
{
    uint32_t pin;
    pwm_config config = pwm_get_default_config();

    /* USE_PTMR=0 is enforced by installer; the kernel uses SysTick. */
    unreset_block_num_wait_blocking(RESET_PWM);
    pwm_config_set_clkdiv_int(&config, 1U);
    pwm_config_set_wrap(&config, ROBOT_PWM_WRAP);
    pwm_init(BOARD_MOTOR_SLICE_LEFT, &config, false);
    pwm_init(BOARD_MOTOR_SLICE_RIGHT, &config, false);
    for (pin = ROBOT_PIN_MOTOR_LEFT_A;
         pin <= ROBOT_PIN_MOTOR_RIGHT_B; pin++)
    {
        gpio_init(pin);
        gpio_put(pin, false);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_set_function(pin, GPIO_FUNC_PWM);
    }
    pwm_set_both_levels(BOARD_MOTOR_SLICE_LEFT, 0U, 0U);
    pwm_set_both_levels(BOARD_MOTOR_SLICE_RIGHT, 0U, 0U);
    pwm_set_enabled(BOARD_MOTOR_SLICE_LEFT, true);
    pwm_set_enabled(BOARD_MOTOR_SLICE_RIGHT, true);
}

/** Initialise passive inputs, trigger low and a disabled-pulse servo. */
static void board_init_inputs (void)
{
    static const uint32_t encoder_pins[4] =
    {
        ROBOT_PIN_ENCODER_LEFT_A, ROBOT_PIN_ENCODER_LEFT_B,
        ROBOT_PIN_ENCODER_RIGHT_A, ROBOT_PIN_ENCODER_RIGHT_B
    };
    uint32_t index;
    uint32_t input_levels;
    pwm_config config = pwm_get_default_config();

    for (index = 0U; index < 4U; index++)
    {
        gpio_init(encoder_pins[index]);
        gpio_set_dir(encoder_pins[index], GPIO_IN);
        gpio_pull_up(encoder_pins[index]);
    }
    input_levels = gpio_get_all();
    /* The ISR is not registered yet: one plain store seeds both phases. */
    g_encoder_word =
        ((board_wheel_phase(input_levels, ROBOT_PIN_ENCODER_LEFT_A,
                            ROBOT_PIN_ENCODER_LEFT_B) << BOARD_PHASE_SHIFT) |
         (board_wheel_phase(input_levels, ROBOT_PIN_ENCODER_RIGHT_A,
                            ROBOT_PIN_ENCODER_RIGHT_B) <<
          (BOARD_PHASE_SHIFT + 2U))) & BOARD_PHASE_BITS;
    gpio_init(ROBOT_PIN_STOP);
    gpio_pull_up(ROBOT_PIN_STOP);
    gpio_init(ROBOT_PIN_START);
    gpio_pull_up(ROBOT_PIN_START);
    gpio_init(ROBOT_PIN_ECHO);
    gpio_pull_down(ROBOT_PIN_ECHO);
    gpio_init(ROBOT_PIN_TRIGGER);
    gpio_put(ROBOT_PIN_TRIGGER, false);
    gpio_set_dir(ROBOT_PIN_TRIGGER, GPIO_OUT);
    gpio_init(ROBOT_PIN_SERVO);
    gpio_put(ROBOT_PIN_SERVO, false);
    gpio_set_dir(ROBOT_PIN_SERVO, GPIO_OUT);
    if (0 != ROBOT_SERVO_CONFIRMED)
    {
        pwm_config_set_clkdiv_int(&config, 125U);
        pwm_config_set_wrap(&config, ROBOT_SERVO_FRAME_US - 1U);
        pwm_init(BOARD_SERVO_SLICE, &config, false);
        pwm_set_chan_level(BOARD_SERVO_SLICE, PWM_CHAN_A, 0U);
        gpio_set_function(ROBOT_PIN_SERVO, GPIO_FUNC_PWM);
        pwm_set_enabled(BOARD_SERVO_SLICE, true);
    }
}

/** Hand the shared Grove 1 pins back to UART0 for the refusal message. */
void board_console_restore (void)
{
    gpio_set_function(ROBOT_PIN_CONSOLE_TX, GPIO_FUNC_UART);
    gpio_set_function(ROBOT_PIN_CONSOLE_RX, GPIO_FUNC_UART);
}

/** Configure only LSM303DLHC acceleration: no nonexistent gyro is assumed. */
static bool board_init_accel (void)
{
    uint8_t control_one;
    uint8_t control_four;
    uint32_t actual_hz;

    gpio_set_function(ROBOT_PIN_IMU_SDA, GPIO_FUNC_I2C);
    gpio_set_function(ROBOT_PIN_IMU_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(ROBOT_PIN_IMU_SDA);
    gpio_pull_up(ROBOT_PIN_IMU_SCL);
    actual_hz = i2c_init(BOARD_I2C, ROBOT_I2C_HZ);
    if ((0U == actual_hz) || (actual_hz > 400000U))
    {
        return false;
    }
    if ((!board_write_register(BOARD_ACCEL_CTRL1, BOARD_ACCEL_ODR_ENABLE)) ||
        (!board_write_register(BOARD_ACCEL_CTRL4, BOARD_ACCEL_BDU_HR_2G)))
    {
        return false;
    }
    if ((!board_read_registers(BOARD_ACCEL_CTRL1, &control_one, 1U)) ||
        (!board_read_registers(BOARD_ACCEL_CTRL4, &control_four, 1U)))
    {
        return false;
    }
    return ((BOARD_ACCEL_ODR_ENABLE == control_one) &&
            (BOARD_ACCEL_BDU_HR_2G == control_four));
}

/** Read a small burst with the SDK's whole-transfer timeout per phase. */
static bool board_read_registers (uint8_t reg, uint8_t * p_data,
                                  size_t count)
{
    int written;
    int received;

    if ((NULL == p_data) || (0U == count) || (count > BOARD_ACCEL_BYTES))
    {
        return false;
    }
    written = i2c_write_timeout_us(BOARD_I2C, BOARD_ACCEL_ADDRESS, &reg, 1U,
                                  true, ROBOT_I2C_TIMEOUT_US);
    if (1 != written)
    {
        return false;
    }
    received = i2c_read_timeout_us(BOARD_I2C, BOARD_ACCEL_ADDRESS, p_data,
                                  count, false, ROBOT_I2C_TIMEOUT_US);
    /* count is checked <= 6, so int conversion is exact. */
    return ((int)count == received);
}

/** Write one LSM303 register, accepting only the complete two-byte result. */
static bool board_write_register (uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    int result = i2c_write_timeout_us(BOARD_I2C, BOARD_ACCEL_ADDRESS, payload,
                                      sizeof(payload), false,
                                      ROBOT_I2C_TIMEOUT_US);

    return (2 == result);
}

/** GPIO ISR: acknowledge owned edges only; do no kernel waits or I/O.
 * Constant work per call, no loops: each shared word is loaded once and
 * stored once per edge batch, and the echo time is one timer-word read.
 */
static void board_gpio_isr (uint32_t interrupt_number)
{
    uint32_t left_pending;
    uint32_t right_pending;
    uint32_t echo_pending;
    uint32_t input_levels;
    uint32_t current_us;
    uint32_t encoder;
    uint32_t totals;
    uint32_t errors;

    /* D-ADAPTER: kernel passes IRQ number; this handler is GPIO-only. */
    (void)interrupt_number;
    left_pending = io_bank0_hw->proc0_irq_ctrl.ints[BOARD_LEFT_BANK] &
                   BOARD_LEFT_MASK;
    right_pending = io_bank0_hw->proc0_irq_ctrl.ints[BOARD_RIGHT_BANK] &
                    BOARD_RIGHT_MASK;
    echo_pending = io_bank0_hw->proc0_irq_ctrl.ints[BOARD_ECHO_BANK] &
                   BOARD_ECHO_MASK;
    /* One write-1-to-clear per bank register, merging sources that share
     * a bank (ECHO on GP1 shares bank 0 with the left encoder).
     */
    io_bank0_hw->intr[BOARD_LEFT_BANK] = left_pending |
        BOARD_SAME_BANK(BOARD_LEFT_BANK, BOARD_RIGHT_BANK, right_pending) |
        BOARD_SAME_BANK(BOARD_LEFT_BANK, BOARD_ECHO_BANK, echo_pending);
#if (BOARD_RIGHT_BANK != BOARD_LEFT_BANK)
    io_bank0_hw->intr[BOARD_RIGHT_BANK] = right_pending |
        BOARD_SAME_BANK(BOARD_RIGHT_BANK, BOARD_ECHO_BANK, echo_pending);
#endif
#if (BOARD_ECHO_BANK != BOARD_LEFT_BANK) && \
    (BOARD_ECHO_BANK != BOARD_RIGHT_BANK)
    io_bank0_hw->intr[BOARD_ECHO_BANK] = echo_pending;
#endif
    /* One SIO read keeps all four encoder levels in one coherent sample. */
    input_levels = gpio_get_all();
    /* The raw low timer word needs no high/low retry loop: echo times are
     * differences modulo 2^32 us (71 minutes), far above the 30 ms timeout.
     */
    current_us = timer_hw->timerawl;
    if (0U != (left_pending | right_pending))
    {
        /* Decode on local copies, then publish each word with one store,
         * so a task never observes a partly applied edge batch.
         */
        encoder = g_encoder_word;
        totals = g_encoder_totals;
        errors = board_count_batch(left_pending, right_pending,
                                   input_levels, &encoder, &totals);
        /* The word uses bits 0-15 only, so the mask keeps it exact. */
        g_encoder_word = encoder & BOARD_ENCODER_BITS;
        g_encoder_totals = totals;
        board_record_error(errors);
    }
    if (0U != echo_pending)
    {
        board_echo_edge(echo_pending, input_levels, current_us);
    }
}

/** Count both wheels' latched edges into the caller's local word copies.
 * Writes no shared word. Returns the error bits to latch, else 0.
 */
static uint32_t board_count_batch (uint32_t left_pending,
                                   uint32_t right_pending,
                                   uint32_t input_levels,
                                   uint32_t * p_encoder,
                                   uint32_t * p_totals)
{
    uint32_t errors = 0U;
    int32_t  left_direction = 0;
    int32_t  right_direction = 0;
#if ROBOT_ENCODER_CHANNELS == 1
    uint32_t directions = g_motor_directions;

    /* One-channel counting signs each A edge with the applied demand. Only
     * tasks write the codes; they decode exactly as bitpack_extend().
     */
    left_direction = board_direction_decode(directions >>
                                            BOARD_LEFT_DIRECTION_SHIFT) *
                     ROBOT_LEFT_MOTOR_SIGN * ROBOT_LEFT_ENCODER_SIGN;
    right_direction = board_direction_decode(directions >>
                                             BOARD_RIGHT_DIRECTION_SHIFT) *
                      ROBOT_RIGHT_MOTOR_SIGN * ROBOT_RIGHT_ENCODER_SIGN;
#endif

    if (0U != left_pending)
    {
        errors |= board_count_wheel(BOARD_LEFT,
            board_wheel_edges(left_pending, ROBOT_PIN_ENCODER_LEFT_A,
                              ROBOT_PIN_ENCODER_LEFT_B),
            board_wheel_phase(input_levels, ROBOT_PIN_ENCODER_LEFT_A,
                              ROBOT_PIN_ENCODER_LEFT_B),
            left_direction, p_encoder, p_totals);
    }
    if (0U != right_pending)
    {
        errors |= board_count_wheel(BOARD_RIGHT,
            board_wheel_edges(right_pending, ROBOT_PIN_ENCODER_RIGHT_A,
                              ROBOT_PIN_ENCODER_RIGHT_B),
            board_wheel_phase(input_levels, ROBOT_PIN_ENCODER_RIGHT_A,
                              ROBOT_PIN_ENCODER_RIGHT_B),
            right_direction, p_encoder, p_totals);
    }
    return errors;
}

/** Latch sticky per-wheel error bits; GPIO ISR context only. Only the
 * motion owner clears them, inside app_main_lock().
 */
static void board_record_error (uint32_t errors)
{
    if (0U != errors)
    {
        g_encoder_errors = (g_encoder_errors | errors) & BOARD_ENCODER_ERRORS;
    }
}

/** Advance the echo word for one ECHO edge batch with a single store. */
static void board_echo_edge (uint32_t echo_pending, uint32_t input_levels,
                             uint32_t current_us)
{
    uint32_t echo_edges = (echo_pending >> BOARD_IRQ_SHIFT(ROBOT_PIN_ECHO)) &
                          0xCU;
    board_echo_state_t state = board_echo_state(g_echo_word);
    uint32_t elapsed_us;

    if ((BOARD_ECHO_WAIT_RISE == state) &&
        ((uint32_t)GPIO_IRQ_EDGE_RISE == echo_edges) &&
        (0U != (input_levels & (1U << ROBOT_PIN_ECHO))))
    {
        g_echo_rise_us = current_us;
        g_echo_word = board_echo_word(BOARD_ECHO_WAIT_FALL, 0U);
    }
    else if ((BOARD_ECHO_WAIT_FALL == state) &&
             ((uint32_t)GPIO_IRQ_EDGE_FALL == echo_edges) &&
             (0U == (input_levels & (1U << ROBOT_PIN_ECHO))))
    {
        /* Unsigned subtraction stays exact across the 2^32 us timer wrap. */
        elapsed_us = current_us - g_echo_rise_us;
        if (elapsed_us <= ROBOT_ECHO_TIMEOUT_US)
        {
            /* State and pulse (<= 30000, 15 bits) publish in one store. */
            g_echo_word = board_echo_word(BOARD_ECHO_READY, elapsed_us);
        }
        else
        {
            g_echo_word = board_echo_word(BOARD_ECHO_TIMEOUT, 0U);
        }
    }
    else if (BOARD_ECHO_IDLE != state)
    {
        /* Both edges coalesced, or unexpected order: no valid timing. */
        g_echo_word = board_echo_word(BOARD_ECHO_TIMEOUT, 0U);
    }
    else
    {
        /* An ECHO edge without a pending trigger is ignored. */
    }
}

/** Normalise one wheel's latched edge bits: A in bits 2..3, B in 6..7. */
static uint32_t board_wheel_edges (uint32_t pending, uint32_t pin_a,
                                   uint32_t pin_b)
{
    return ((pending >> BOARD_IRQ_SHIFT(pin_a)) & 0xCU) |
           (((pending >> BOARD_IRQ_SHIFT(pin_b)) & 0xCU) << 4U);
}

/** Sample one wheel's phase, A in bit 1 and B in bit 0, from one SIO read. */
static uint32_t board_wheel_phase (uint32_t input_levels, uint32_t pin_a,
                                   uint32_t pin_b)
{
    return (((input_levels >> pin_a) & 1U) << 1U) |
           ((input_levels >> pin_b) & 1U);
}

/** Count one wheel's latched edges into the local word copies and adopt the
 * sampled phase. Returns the wheel's error bit, 1 << wheel, for a rejected
 * edge or a saturated delta, else 0.
 */
static uint32_t board_count_wheel (uint32_t wheel, uint32_t pending,
                                   uint32_t current, int32_t direction,
                                   uint32_t * p_encoder,
                                   uint32_t * p_totals)
{
    uint32_t shift = BOARD_PHASE_SHIFT + (2U * wheel);
    uint32_t previous = (*p_encoder >> shift) & 3U;
    int32_t  increment = board_count_encoder(pending, current, previous,
                                             direction);

    /* Every batch adopts the sampled phase: a rejected edge resynchronizes. */
    *p_encoder = (*p_encoder & ~(3U << shift)) | (current << shift);
    if (0 == increment)
    {
        return 1U << wheel;
    }
    return board_add_count(wheel, increment, p_encoder, p_totals);
}

/** Select the explicitly configured acquisition model without guessing B.
 * Returns +1 or -1 for one accepted edge and 0 for a rejected one.
 */
static int32_t board_count_encoder (uint32_t pending, uint32_t current,
                                    uint32_t previous, int32_t direction)
{
#if ROBOT_ENCODER_CHANNELS == 2
    (void)direction;
    return board_count_transition(pending, current, previous);
#else
    uint32_t expected = (0U != (current & 2U)) ?
                        (uint32_t)GPIO_IRQ_EDGE_RISE :
                        (uint32_t)GPIO_IRQ_EDGE_FALL;

    /* Both A edges count. B is not a sensor in this mode. Coalesced edges,
     * inconsistent samples and unknown sign cannot yield safe displacement.
     * Raw sign matches the quadrature contract; app applies encoder polarity.
     */
    if ((0 == ROBOT_SINGLE_CHANNEL_DIRECTION_CONFIRMED) ||
        ((1 != direction) && (-1 != direction)) ||
        (0U == ((current ^ previous) & 2U)) || (pending != expected))
    {
        return 0;
    }
    return direction;
#endif
}

#if ROBOT_ENCODER_CHANNELS == 2
/** Reject ambiguous latched edges before interpreting the sampled phase.
 * Returns the x4 increment, +1 or -1, or 0 for a rejected edge.
 */
static int32_t board_count_transition (uint32_t pending, uint32_t current,
                                       uint32_t previous)
{
    static const int32_t increments[16] =
    {
         0,  1, -1,  0,
        -1,  0,  0,  1,
         1,  0,  0, -1,
         0, -1,  1,  0
    };
    uint32_t changed = current ^ previous;
    uint32_t expected;

    /* Pending is normalized to A's GPIO nibble then B's nibble. Only one
     * edge consistent with the sampled change can represent an x4 count.
     * Several bits lose ordering; a single inconsistent bit can arise when
     * another edge races the status acknowledgement and SIO sample. Never
     * invent displacement; caller resynchronizes the observed phase.
     */
    /* The SDK edge enumerators are positive int constants: widen first. */
    if (2U == changed)
    {
        expected = (0U != (current & 2U)) ? (uint32_t)GPIO_IRQ_EDGE_RISE :
                                           (uint32_t)GPIO_IRQ_EDGE_FALL;
    }
    else
    {
        expected = ((0U != (current & 1U)) ? (uint32_t)GPIO_IRQ_EDGE_RISE :
                                            (uint32_t)GPIO_IRQ_EDGE_FALL)
                   << 4U;
    }
    if (((1U != changed) && (2U != changed)) || (pending != expected))
    {
        return 0;
    }
    /* One changed channel always selects +1 or -1 from the table. */
    return increments[(previous << 2U) | current];
}
#endif

/** Advance one wheel's modulo-2^16 total, then its saturating 6-bit delta.
 * The total counts even when the delta saturates, as before. Returns the
 * wheel's error bit when the delta is already at +/-31, else 0.
 */
static uint32_t board_add_count (uint32_t wheel, int32_t increment,
                                 uint32_t * p_encoder, uint32_t * p_totals)
{
    uint32_t total_shift = 16U * wheel;
    uint32_t delta_shift = 6U * wheel;
    /* Adding 0xFFFF subtracts one modulo 2^16; the mask stops any carry or
     * borrow reaching the other wheel's half.
     */
    uint32_t step = (increment > 0) ? 1U : BOARD_TOTAL_MASK;
    uint32_t total = ((*p_totals >> total_shift) + step) & BOARD_TOTAL_MASK;
    int32_t  delta = board_delta(*p_encoder, wheel);

    *p_totals = (*p_totals & ~(BOARD_TOTAL_MASK << total_shift)) |
                (total << total_shift);
    if (((increment > 0) && (BOARD_DELTA_MAX == delta)) ||
        ((increment < 0) && (-BOARD_DELTA_MAX == delta)))
    {
        return 1U << wheel;
    }
    /* |delta + increment| <= 31: its 6-bit two's complement code is exact. */
    *p_encoder = (*p_encoder & ~(BOARD_DELTA_MASK << delta_shift)) |
                 (((uint32_t)(delta + increment) & BOARD_DELTA_MASK) <<
                  delta_shift);
    return 0U;
}

/** Sign-extend one wheel's 6-bit delta, equal to bitpack_extend(field, 6U).
 */
static int32_t board_delta (uint32_t encoder, uint32_t wheel)
{
    uint32_t field = (encoder >> (6U * wheel)) & BOARD_DELTA_MASK;

    /* Flipping the sign bit maps codes 0..63 to 32..63, 0..31: the biased
     * value is at most 63, so the conversion and subtraction are exact.
     */
    return (int32_t)(field ^ BOARD_DELTA_SIGN) - (int32_t)BOARD_DELTA_SIGN;
}

/** Insert one zero-demand cycle before changing polarity; caller masks IRQ.
 * The applied direction is the wheel's code in the task-written byte.
 */
static void board_apply_motor_locked (uint32_t slice, int32_t direction,
                                      uint16_t level, uint32_t shift)
{
    int32_t applied = board_direction_decode((uint32_t)g_motor_directions >>
                                             shift);

    if ((0 != applied) &&
        (direction != applied))
    {
        pwm_set_both_levels(slice, 0U, 0U);
        board_store_direction_locked(shift, 0);
        return;
    }
    if (direction > 0)
    {
        pwm_set_both_levels(slice, level, 0U);
    }
    else if (direction < 0)
    {
        pwm_set_both_levels(slice, 0U, level);
    }
    else
    {
        pwm_set_both_levels(slice, 0U, 0U);
    }
    board_store_direction_locked(shift, direction);
}

/** Replace one wheel's 2-bit direction code; callers hold the lock. */
static void board_store_direction_locked (uint32_t shift, int32_t direction)
{
    uint32_t directions = g_motor_directions;

    directions &= ~(BOARD_DIRECTION_MASK << shift);
    directions |= BOARD_DIRECTION_ENCODE(direction) << shift;
    /* Two 2-bit codes occupy bits 0-3, so the mask keeps the byte exact. */
    g_motor_directions = directions & BOARD_DIRECTIONS_MASK;
}

/** Map validated duty to its capped compare level outside the IRQ mask. */
static uint16_t board_duty_level (core_real_t duty)
{
    core_real_t magnitude = fabs(duty);

    if (magnitude > ROBOT_MAX_DUTY)
    {
        magnitude = ROBOT_MAX_DUTY;
    }
    /* Validated duty <= 1 and wrap 6249 bound level to 0..6250. */
    return (uint16_t)(magnitude * (ROBOT_PWM_WRAP + 1U));
}

/** Compose the echo word: state bits 0-2, pulse bits 3-17 (READY only). */
static uint32_t board_echo_word (board_echo_state_t state, uint32_t pulse_us)
{
    return ((uint32_t)state & BOARD_ECHO_STATE_MASK) |
           ((pulse_us & BOARD_ECHO_PULSE_MASK) << BOARD_ECHO_PULSE_SHIFT);
}

/** Decode the state bits of an echo word built by board_echo_word().
 * Only codes 0..4 are stored; 5..7 could only come from a corrupted word
 * and read as TIMEOUT (UNKNOWN), so they can never yield a pulse.
 */
static board_echo_state_t board_echo_state (uint32_t echo)
{
    uint32_t code = echo & BOARD_ECHO_STATE_MASK;

    if (code > (uint32_t)BOARD_ECHO_TIMEOUT)
    {
        code = (uint32_t)BOARD_ECHO_TIMEOUT;
    }
    /* code is an enumerator value 0..4 here, so the conversion is exact. */
    return (board_echo_state_t)code;
}

/* End of file. */


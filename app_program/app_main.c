/** @file app_main.c
 * @brief Actual micro T-Kernel task creation and RP2040 interrupt boundary.
 * BARR-ABI-01: usermain and kernel callback casts follow the required BSP ABI.
 * BARR-ASM-01: GNU inline assembly is restricted to this target boundary.
 */
#include "app_main.h"
#include "robot_app.h"
#include "board.h"
#include <tk/tkernel.h>
#include <tk/syslib.h>
#include <tm/tmonitor.h>

#if TK_SUPPORT_SMP
#error "Robot peripheral ownership is validated for SMP=0 only"
#endif

#define APP_MAIN_TASKS (8U)
#define APP_MAIN_STACK_BYTES (4096)
#define APP_MAIN_GPIO_IRQ (13U)

/* usermain is the sole external entry name mandated by this BSP. */
INT usermain (void);

/* Written with the GPIO interrupt disabled, read only by its dispatcher. */
static void (* gp_gpio_handler)(uint32_t) = NULL;

/** @brief Match the kernel dispatcher's exact UW interrupt parameter. */
static void app_main_gpio_isr (UW intno);

static void app_main_safety_task (INT start_code, void * p_extra);
static void app_main_motion_task (INT start_code, void * p_extra);
static void app_main_line_task (INT start_code, void * p_extra);
static void app_main_vehicle_task (INT start_code, void * p_extra);
static void app_main_imu_task (INT start_code, void * p_extra);
static void app_main_scan_task (INT start_code, void * p_extra);
static void app_main_comm_task (INT start_code, void * p_extra);
static void app_main_telemetry_task (INT start_code, void * p_extra);

/** @brief Bound delay conversion to the kernel's unsigned RELTIM type. */
void app_main_delay (uint32_t delay_ms)
{
    if (delay_ms > 1000U)
    {
        delay_ms = 1000U;
    }
    if (0U == delay_ms)
    {
        delay_ms = 1U;
    }
    /* 1..1000 is exactly representable by the kernel timeout type. */
    if (E_OK != tk_dly_tsk((RELTIM)delay_ms))
    {
        (void)app_main_lock();
        board_motor_brake();
        for (;;)
        {
            __asm__ volatile ("nop");
        }
    }
}

/** @brief Save PRIMASK atomically; memory clobber orders shared snapshots. */
uint32_t app_main_lock (void)
{
    uint32_t saved_mask = 0U;

    __asm__ volatile (
        "mrs %0, primask\n"
        "cpsid i\n" : "=r"(saved_mask) : : "memory");
    return saved_mask;
}

/** @brief Restore prior masking, allowing already-pending kernel scheduling. */
void app_main_unlock (uint32_t saved_mask)
{
    __asm__ volatile (
        "msr primask, %0" : : "r"(saved_mask) : "memory");
}

/** @brief GPIO callback matches the kernel high-level interrupt convention. */
bool app_main_install_gpio_irq (void (* p_handler)(uint32_t))
{
    T_DINT definition = {0};
    ER     result     = E_OK;

    if (NULL == p_handler)
    {
        return false;
    }
    DisableInt(APP_MAIN_GPIO_IRQ);
    definition.intatr = TA_HLNG;
    /* Kernel FP invokes UW intno; the private wrapper has that exact type. */
    definition.inthdr = (FP)app_main_gpio_isr;
    result = tk_def_int(APP_MAIN_GPIO_IRQ, &definition);
    if (E_OK != result)
    {
        return false;
    }
    /* Publish before re-enabling this interrupt; SMP is compile-time denied. */
    gp_gpio_handler = p_handler;
    /* Level 1 is the port's highest permitted external-interrupt level. */
    EnableInt(APP_MAIN_GPIO_IRQ, 1);
    return true;
}

/** @brief Initialize before task creation; any failed object leaves BRAKE. */
INT usermain (void)
{
    const FP entries[APP_MAIN_TASKS] =
    {
        /* Kernel task FP ABI is INT, void*: no data-pointer conversion. */
        (FP)app_main_safety_task, (FP)app_main_motion_task,
        (FP)app_main_line_task, (FP)app_main_vehicle_task,
        (FP)app_main_imu_task, (FP)app_main_scan_task,
        (FP)app_main_comm_task, (FP)app_main_telemetry_task
    };
    ID       h_task_ids[APP_MAIN_TASKS] = {0};
    T_CTSK   definition = {0};
    uint32_t index      = 0U;
    bool     b_ready    = board_init();
    bool     b_app_ready = robot_app_init();
    const char * p_refusal = board_init_diagnostic();

    if (b_ready && (!b_app_ready))
    {
        p_refusal = "application configuration rejected";
    }
    b_ready = b_app_ready && b_ready;
    for (index = 0U; (index < APP_MAIN_TASKS) && b_ready; ++index)
    {
        definition.tskatr  = TA_HLNG | TA_RNG3;
        definition.task    = entries[index];
        definition.stksz   = APP_MAIN_STACK_BYTES;
        /* index+1 is in [1,8], inside PRI range. */
        definition.itskpri = (PRI)(index + 1U);
        h_task_ids[index] = tk_cre_tsk(&definition);
        b_ready = h_task_ids[index] > E_OK;
        p_refusal = "kernel task creation failed";
    }
    for (index = 0U; (index < APP_MAIN_TASKS) && b_ready; ++index)
    {
        b_ready = E_OK == tk_sta_tsk(h_task_ids[index], 0);
        p_refusal = "kernel task start failed";
    }
    if (b_ready)
    {
        b_ready = board_watchdog_start(1000U);
        p_refusal = "watchdog could not be armed";
    }
    if (!b_ready)
    {
        /* No task can enable motors once this initial task masks interrupts. */
        (void)app_main_lock();
        board_motor_brake();
        /* GP0/GP1 serve the HC-SR04 after board_init(); give them back to
         * UART0 so the refusal reason still reaches a console probe.
         */
        board_console_restore();
        /* Pinned UART monitor uses bounded polling and preserves PRIMASK.
         * Print after braking/masking so partially started tasks cannot drive.
         * D-ADAPTER: unsigned-byte format is required by the kernel ABI.
         */
        (void)tm_printf((const UB *)"\nROBOT STARTUP REFUSED: %s\n"
                        "Motors inhibited; review configuration.\n",
                        p_refusal);
        for (;;)
        {
            __asm__ volatile ("nop");
        }
    }
    for (;;)
    {
        /* A wake cannot accidentally return and shut down the kernel. */
        (void)tk_slp_tsk(TMO_FEVR);
    }
}

/** @brief Forward the GPIO IRQ through the fixed-width board boundary. */
static void app_main_gpio_isr (UW intno)
{
    if ((APP_MAIN_GPIO_IRQ == intno) && (NULL != gp_gpio_handler))
    {
        /* Only IRQ 13 is accepted, exactly representable in uint32_t. */
        gp_gpio_handler((uint32_t)intno);
    }
}

/** @brief Safety task's required kernel calling convention. */
static void app_main_safety_task (INT start_code, void * p_extra)
{
    (void)start_code;
    (void)p_extra;
    robot_app_safety_task();
}

/** @brief Motion task's required kernel calling convention. */
static void app_main_motion_task (INT start_code, void * p_extra)
{
    (void)start_code;
    (void)p_extra;
    robot_app_motion_task();
}

/** @brief Optical task's required kernel calling convention. */
static void app_main_line_task (INT start_code, void * p_extra)
{
    (void)start_code;
    (void)p_extra;
    robot_app_line_task();
}

/** @brief Vehicle task's required kernel calling convention. */
static void app_main_vehicle_task (INT start_code, void * p_extra)
{
    (void)start_code;
    (void)p_extra;
    robot_app_vehicle_task();
}

/** @brief IMU task's required kernel calling convention. */
static void app_main_imu_task (INT start_code, void * p_extra)
{
    (void)start_code;
    (void)p_extra;
    robot_app_imu_task();
}

/** @brief Scan task's required kernel calling convention. */
static void app_main_scan_task (INT start_code, void * p_extra)
{
    (void)start_code;
    (void)p_extra;
    robot_app_scan_task();
}

/** @brief Queue-service task; CYW43 owns actual lwIP polling separately. */
static void app_main_comm_task (INT start_code, void * p_extra)
{
    (void)start_code;
    (void)p_extra;
    robot_app_comm_task();
}

/** @brief Telemetry task's required kernel calling convention. */
static void app_main_telemetry_task (INT start_code, void * p_extra)
{
    (void)start_code;
    (void)p_extra;
    robot_app_telemetry_task();
}

/* End of file. */


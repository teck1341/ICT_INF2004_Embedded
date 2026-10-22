/** @file app_main.h
 * @brief Fixed-width boundary between ISO C application and kernel headers.
 * The pinned kernel has a non-ISO size_t; do not combine it with SDK headers.
 */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#include <stdbool.h>
#include <stdint.h>

/** Delay in kernel milliseconds; bounded by the caller to <=1000 ms. */
void app_main_delay (uint32_t delay_ms);

/** Mask interrupts with a compiler memory barrier. Single-core tasks only. */
uint32_t app_main_lock (void);

/** Restore the previous interrupt mask; never wait inside this region. */
void app_main_unlock (uint32_t saved_mask);

/** Register GPIO through the kernel dispatcher, preserving kernel vectors.
 * Use during initialization; a kernel error leaves GPIO IRQ disabled.
 * The adapter converts the kernel's UW interrupt number to uint32_t.
 */
bool app_main_install_gpio_irq (void (* p_handler)(uint32_t));

#endif /* APP_MAIN_H */

/* End of file. */


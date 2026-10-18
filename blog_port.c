/**
 * @file    blog_port.c
 * @brief   BLOG platform port example for Cortex-M + CMSIS-RTOS2/RTX5.
 * @author  yangfan
 * @date    2026-10-18
 */

#include "blog.h"

#include "cmsis_compiler.h"
#include "cmsis_os2.h"

/**
 * @brief Get timestamp when a BLOG record is generated.
 *
 * @return Current RTOS kernel tick.
 */
uint32_t blog_port_timestamp(void) { return osKernelGetTickCount(); }

/**
 * @brief Enter a short BLOG critical section.
 *
 * @return Previous PRIMASK value.
 */
uint32_t blog_port_lock(void) {
  const uint32_t key = __get_PRIMASK();
  __disable_irq();
  return key;
}

/**
 * @brief Leave BLOG critical section and restore interrupt state.
 *
 * @param key Previous PRIMASK value returned by blog_port_lock().
 */
void blog_port_unlock(uint32_t key) { __set_PRIMASK(key); }

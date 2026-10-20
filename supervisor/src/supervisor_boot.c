/**
 * @file supervisor_boot.c
 * @brief Supervisor启动控制对共享安全ENV执行器的适配
 * @author Dawn
 * @version 0.3.0
 * @date 2026-10-20
 */

#include "supervisor_boot.h"

/****************************** 启动状态 ******************************/

/**
 * @brief 转交共用启动控制的actual操作。
 */
int supervisor_boot_actual(supervisor_kernel_t *kernel)
{
    return linkg_boot_actual(kernel);
}

/**
 * @brief 转交共用启动控制的trial操作。
 */
int supervisor_boot_trial(supervisor_kernel_t target, supervisor_kernel_t stable)
{
    return linkg_boot_trial(target, stable);
}

/**
 * @brief 转交共用启动控制的stable操作。
 */
int supervisor_boot_stable(supervisor_kernel_t kernel)
{
    return linkg_boot_stable(kernel);
}

/**
 * @brief 转交共用启动控制的reboot操作。
 */
int supervisor_boot_reboot(void)
{
    return linkg_boot_reboot();
}

/**
 * @brief 转交共用启动控制的is_stable操作。
 */
int supervisor_boot_is_stable(supervisor_kernel_t kernel, bool *stable)
{
    return linkg_boot_is_stable(kernel, stable);
}

/**
 * @brief 转交共用启动控制的checkpoint操作。
 */
int supervisor_boot_checkpoint(void)
{
    return linkg_boot_checkpoint();
}

/**
 * @brief 转交共用启动控制的set_progress操作。
 */
void supervisor_boot_set_progress(supervisor_boot_progress_t progress, void *context)
{
    linkg_boot_set_progress(progress, context);
}

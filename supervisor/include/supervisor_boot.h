/**
 * @file supervisor_boot.h
 * @brief 实际Kernel槽位及Tina U-Boot环境交互接口
 */

#ifndef SUPERVISOR_BOOT_H
#define SUPERVISOR_BOOT_H

#include "supervisor_version.h"

/****************************** 类型定义 ******************************/

typedef int (*supervisor_boot_progress_t)(void *context); // 单线程辅助命令等待期间的进程健康检查

/****************************** 启动状态 ******************************/

int  supervisor_boot_actual(supervisor_kernel_t *kernel);
int  supervisor_boot_trial(supervisor_kernel_t target, supervisor_kernel_t stable);
int  supervisor_boot_stable(supervisor_kernel_t kernel);
int  supervisor_boot_reboot(void);
int  supervisor_boot_is_stable(supervisor_kernel_t kernel, bool *stable);
int  supervisor_boot_checkpoint(void);
void supervisor_boot_set_progress(supervisor_boot_progress_t progress, void *context);

#endif

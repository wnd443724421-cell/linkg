/**
 * @file linkg_boot_env.h
 * @brief LinkG与Supervisor共用的安全ENV工具及实际Kernel接口
 */

#ifndef LINKG_BOOT_ENV_H
#define LINKG_BOOT_ENV_H

#include "linkg_release_state.h"

/****************************** 模块常量 ******************************/

#define LINKG_BOOT_ENV_LOCK_FILE     "/var/run/linkg-supervisor-env.lock" // 全部ENV事务共用，写工具继承锁
#define LINKG_BOOT_ENV_READ_MS       1000U                                // 读取硬超时，单位毫秒
#define LINKG_BOOT_ENV_WRITE_WARN_MS 1000U                                // 写入只告警，禁止超时强杀
#define LINKG_BOOT_ENV_WAIT_LOG_MS   30000U                               // 持续等待告警间隔
#define LINKG_BOOT_ENV_POLL_MS       50U                                  // 等待及健康检查周期

/****************************** 类型定义 ******************************/

typedef int (*linkg_boot_progress_t)(void *context); // 单线程ENV等待期间的取消或健康检查

/****************************** 启动状态 ******************************/

int  linkg_boot_actual(linkg_kernel_slot_t *kernel);
int  linkg_boot_trial(linkg_kernel_slot_t target, linkg_kernel_slot_t stable);
int  linkg_boot_prepare(linkg_kernel_slot_t target, linkg_kernel_slot_t stable);
int  linkg_boot_stable(linkg_kernel_slot_t kernel);
int  linkg_boot_reboot(void);
int  linkg_boot_is_stable(linkg_kernel_slot_t kernel, bool *stable);
int  linkg_boot_checkpoint(void);
void linkg_boot_set_progress(linkg_boot_progress_t progress, void *context);

#endif

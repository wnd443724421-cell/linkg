/**
 * @file upgrade_kernel.h
 * @brief LinkG升级包Kernel槽位比较、刷写和校验接口
 */

#ifndef UPGRADE_KERNEL_H
#define UPGRADE_KERNEL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 安装接口 ******************************/

int upgrade_kernel_prepare(const char *upgrade_dir, const char *stable_kernel_slot, char *target_kernel_slot, size_t slot_size);

#ifdef __cplusplus
}
#endif

#endif // UPGRADE_KERNEL_H

/**
 * @file supervisor_version.h
 * @brief 固定版本JSON及Release选择接口
 */

#ifndef SUPERVISOR_VERSION_H
#define SUPERVISOR_VERSION_H

#include <stdbool.h>
#include <stddef.h>

#include "linkg_release_state.h"

/****************************** 类型定义 ******************************/

#define SUPERVISOR_KERNEL_NONE LINKG_KERNEL_NONE // 未安装映射
#define SUPERVISOR_KERNEL_A    LINKG_KERNEL_A    // boot_a
#define SUPERVISOR_KERNEL_B    LINKG_KERNEL_B    // boot_b

typedef linkg_kernel_slot_t   supervisor_kernel_t;  // 共用Kernel类型
typedef linkg_release_state_t supervisor_version_t; // 共用版本与升级状态

/****************************** 状态文件 ******************************/

int  supervisor_version_load(const char *path, supervisor_version_t *version);
int  supervisor_version_save(const char *path, const supervisor_version_t *version);
int  supervisor_version_replace(const char *path, const supervisor_version_t *expected, const supervisor_version_t *desired);
bool supervisor_version_equal(const supervisor_version_t *left, const supervisor_version_t *right);

/****************************** 版本选择 ******************************/

const char *supervisor_version_name(unsigned int release);
const char *supervisor_version_kernel_name(supervisor_kernel_t kernel);
int         supervisor_version_check(const char *app_root, const char *web_root, unsigned int release);
int         supervisor_version_program(const char *app_root, char *program, size_t capacity);

#endif

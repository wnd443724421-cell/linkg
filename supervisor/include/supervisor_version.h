/**
 * @file supervisor_version.h
 * @brief 固定版本JSON及Release选择接口
 */

#ifndef SUPERVISOR_VERSION_H
#define SUPERVISOR_VERSION_H

#include <stdbool.h>
#include <stddef.h>

/****************************** 类型定义 ******************************/

typedef enum
{
    SUPERVISOR_KERNEL_NONE = 0, // Release尚未安装
    SUPERVISOR_KERNEL_A    = 1, // boot_a
    SUPERVISOR_KERNEL_B    = 2  // boot_b
} supervisor_kernel_t;

typedef struct
{
    unsigned int        target;    // 目标Release索引，0为v1，1为v2
    unsigned int        stable;    // 已确认Release索引
    supervisor_kernel_t kernel[2]; // 两个Release的Kernel映射，NONE表示未安装
} supervisor_version_t;

/****************************** 状态文件 ******************************/

int  supervisor_version_load(const char *path, supervisor_version_t *version);
int  supervisor_version_save(const char *path, const supervisor_version_t *version);
bool supervisor_version_equal(const supervisor_version_t *left, const supervisor_version_t *right);

/****************************** 版本选择 ******************************/

const char *supervisor_version_name(unsigned int release);
const char *supervisor_version_kernel_name(supervisor_kernel_t kernel);
int         supervisor_version_check(const char *app_root, const char *web_root, unsigned int release);
int         supervisor_version_program(const char *app_root, char *program, size_t capacity);

#endif

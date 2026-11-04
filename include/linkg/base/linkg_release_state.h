/**
 * @file linkg_release_state.h
 * @brief LinkG与Supervisor共用的版本状态及原子事务接口
 */

#ifndef LINKG_RELEASE_STATE_H
#define LINKG_RELEASE_STATE_H

#include <stdbool.h>
#include <stddef.h>

/****************************** 模块常量 ******************************/

#define LINKG_RELEASE_FAILURE_STAGE_MAX   40U  // 失败阶段名称容量
#define LINKG_RELEASE_FAILURE_CHECK_MAX   40U  // 失败检查项名称容量
#define LINKG_RELEASE_FAILURE_TIME_MAX    32U  // UTC时间字符串容量
#define LINKG_RELEASE_FAILURE_REASON_MAX  128U // 简短失败原因容量

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_KERNEL_NONE = 0, // Release尚未安装
    LINKG_KERNEL_A    = 1, // boot_a
    LINKG_KERNEL_B    = 2  // boot_b
} linkg_kernel_slot_t;

typedef enum
{
    LINKG_UPGRADE_PHASE_IDLE = 0,       // 初始状态
    LINKG_UPGRADE_PHASE_VERIFYING,      // 解包及校验
    LINKG_UPGRADE_PHASE_INSTALLING,     // 安装非稳定Release
    LINKG_UPGRADE_PHASE_FLASHING,       // 擦写并读回备用Kernel
    LINKG_UPGRADE_PHASE_REBOOTING,      // 候选已提交，准备重启
    LINKG_UPGRADE_PHASE_TESTING,        // Supervisor试运行候选
    LINKG_UPGRADE_PHASE_COMPLETED,      // 已完成稳定确认
    LINKG_UPGRADE_PHASE_ROLLING_BACK,   // 旧版本恢复尚未完成
    LINKG_UPGRADE_PHASE_ROLLED_BACK,    // 旧版本已恢复
    LINKG_UPGRADE_PHASE_FAILED,         // 安装失败或中断
    LINKG_UPGRADE_PHASE_COUNT           // 状态边界，不写入文件
} linkg_upgrade_phase_t;

typedef struct
{
    unsigned int          target;    // 目标Release索引，0为v1，1为v2
    unsigned int          stable;    // 已确认Release索引
    linkg_kernel_slot_t   kernel[2]; // Kernel映射，NONE表示未安装
    linkg_upgrade_phase_t upgrade;   // 唯一持久化升级阶段
} linkg_release_state_t;

/**
 * 独立持久化诊断记录，不改变version_state.json既有Schema。
 */
typedef struct
{
    unsigned int candidate;                                           // 本次失败的候选Release索引
    unsigned int rollback;                                            // 回滚稳定Release索引
    int          error_code;                                          // 原始负errno或退出错误码
    char         failed_stage[LINKG_RELEASE_FAILURE_STAGE_MAX];        // 失败所在事务阶段
    char         failed_check[LINKG_RELEASE_FAILURE_CHECK_MAX];        // 具体故障检查项
    char         failure_time[LINKG_RELEASE_FAILURE_TIME_MAX];        // 记录时间（UTC）
    char         failure_reason[LINKG_RELEASE_FAILURE_REASON_MAX];    // 简短原因，不包含日志正文
} linkg_release_failure_t;

/****************************** 状态文件 ******************************/

int         linkg_release_load(const char *path, linkg_release_state_t *state);
int         linkg_release_save(const char *path, const linkg_release_state_t *state);
int         linkg_release_replace(const char *path, const linkg_release_state_t *expected, const linkg_release_state_t *desired);
int         linkg_release_lock(const char *path, const char *suffix, bool nonblocking);
int         linkg_release_failure_save(const char *state_path, const linkg_release_failure_t *failure);
int         linkg_release_failure_load(const char *state_path, linkg_release_failure_t *failure);
int         linkg_release_failure_clear(const char *state_path);
bool        linkg_release_equal(const linkg_release_state_t *left, const linkg_release_state_t *right);
const char *linkg_release_name(unsigned int release);
const char *linkg_release_kernel_name(linkg_kernel_slot_t kernel);
const char *linkg_release_phase_name(linkg_upgrade_phase_t phase);
const char *linkg_release_path(const char *test_name, const char *production);

#endif

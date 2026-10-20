/**
 * @file upgrade_commit.h
 * @brief 升级安装租约和JSON到ENV候选提交接口
 */

#ifndef UPGRADE_COMMIT_H
#define UPGRADE_COMMIT_H

#include "linkg_release_state.h"

/****************************** 类型定义 ******************************/

typedef struct
{
    linkg_release_state_t state;    // 最近持久化的完整状态快照
    int                   lease_fd; // 全程持有的安装互斥租约
} upgrade_commit_t;

/****************************** 事务接口 ******************************/

const char *upgrade_commit_state_path(void);
int upgrade_commit_begin(upgrade_commit_t *transaction);
int upgrade_commit_stage(upgrade_commit_t *transaction, linkg_upgrade_phase_t phase);
int upgrade_commit_candidate(upgrade_commit_t *transaction, linkg_kernel_slot_t kernel);
int upgrade_commit_fail(upgrade_commit_t *transaction);
void upgrade_commit_end(upgrade_commit_t *transaction);
int upgrade_commit_reboot(void);

#endif

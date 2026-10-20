/**
 * @file upgrade_commit.c
 * @brief 安装互斥、持久化升级阶段及候选提交
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#include "upgrade_commit.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linkg_boot_env.h"
#include "linkg_lifecycle.h"
#include "linkg_supervisor_client.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 仅允许支持扩展状态格式的真实父Supervisor管理下发起升级。
 */
static int _upgrade_commit_manager(void)
{
    return linkg_supervisor_client_can_upgrade() ? 0 : -EPROTONOSUPPORT;
}

/**
 * @brief 检查实际Kernel和完整稳定ENV，未知状态不允许开始安装。
 */
static int _upgrade_commit_kernel(const linkg_release_state_t *state)
{
    linkg_kernel_slot_t actual;
    bool               confirmed;
    int                ret;

    ret = linkg_boot_actual(&actual);
    if (ret == 0 && actual != state->kernel[state->stable])
    {
        ret = -ESTALE;
    }
    if (ret == 0)
    {
        ret = linkg_boot_is_stable(actual, &confirmed);
    }
    return ret == 0 && !confirmed ? -EBUSY : ret;
}

/****************************** 事务接口 ******************************/

/**
 * @brief 产品固定使用同一状态文件，隔离构建才允许临时目录。
 */
const char *upgrade_commit_state_path(void)
{
    return linkg_release_path("LINKG_OTA_TEST_STATE", "/app/version_state.json");
}

/**
 * @brief 获取安装租约并读取稳定状态，此时尚未接受请求或修改JSON。
 */
int upgrade_commit_begin(upgrade_commit_t *transaction)
{
    int ret;

    memset(transaction, 0, sizeof(*transaction));
    transaction->lease_fd = -1;
    ret = _upgrade_commit_manager();
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_release_lock(upgrade_commit_state_path(), ".ota.lock", true);
    if (ret < 0)
    {
        return ret;
    }
    transaction->lease_fd = ret;
    ret = linkg_release_load(upgrade_commit_state_path(), &transaction->state);
    if (ret == 0 && (transaction->state.target != transaction->state.stable ||
        (transaction->state.upgrade != LINKG_UPGRADE_PHASE_IDLE && transaction->state.upgrade != LINKG_UPGRADE_PHASE_COMPLETED &&
         transaction->state.upgrade != LINKG_UPGRADE_PHASE_ROLLED_BACK && transaction->state.upgrade != LINKG_UPGRADE_PHASE_FAILED)))
    {
        ret = -EBUSY;
    }
    if (ret == 0)
    {
        ret = _upgrade_commit_kernel(&transaction->state);
    }
    if (ret != 0)
    {
        upgrade_commit_end(transaction);
    }
    return ret;
}

/**
 * @brief 在实际阶段开始前比较并持久化状态，不覆盖其他进程的字段。
 */
int upgrade_commit_stage(upgrade_commit_t *transaction, linkg_upgrade_phase_t phase)
{
    linkg_release_state_t desired;
    int                   ret;

    if (transaction->lease_fd < 0)
    {
        return -EINVAL;
    }
    desired = transaction->state;
    desired.upgrade = phase;
    ret = linkg_release_replace(upgrade_commit_state_path(), &transaction->state, &desired);
    if (ret == 0)
    {
        transaction->state = desired;
    }
    return ret;
}

/**
 * @brief 所有文件与Kernel校验后，先提交候选JSON，再批量写ENV并读回。
 */
int upgrade_commit_candidate(upgrade_commit_t *transaction, linkg_kernel_slot_t kernel)
{
    linkg_release_state_t current;
    linkg_release_state_t desired;
    int                   ret;

    if (transaction->lease_fd < 0 || (kernel != LINKG_KERNEL_A && kernel != LINKG_KERNEL_B))
    {
        return -EINVAL;
    }
    ret = linkg_release_load(upgrade_commit_state_path(), &current);
    if (ret == 0 && (!linkg_release_equal(&current, &transaction->state) || current.target != current.stable))
    {
        ret = -ESTALE;
    }
    if (ret == 0)
    {
        ret = _upgrade_commit_kernel(&current);
    }
    if (ret != 0)
    {
        return ret;
    }

    desired = current;
    desired.target = 1U - desired.stable;
    desired.kernel[desired.target] = kernel;
    desired.upgrade = LINKG_UPGRADE_PHASE_REBOOTING;
    ret = linkg_release_replace(upgrade_commit_state_path(), &current, &desired);
    if (ret == 0)
    {
        transaction->state = desired;
        ret = linkg_boot_prepare(kernel, desired.kernel[desired.stable]);
    }
    return ret;
}

/**
 * @brief 失败先撤销候选JSON再修复稳定ENV，结果不明确时由下次启动继续修复。
 */
int upgrade_commit_fail(upgrade_commit_t *transaction)
{
    linkg_release_state_t current;
    linkg_release_state_t desired;
    int                   ret;

    ret = linkg_release_load(upgrade_commit_state_path(), &current);
    if (ret != 0 || current.stable != transaction->state.stable ||
        current.kernel[current.stable] != transaction->state.kernel[transaction->state.stable])
    {
        return ret != 0 ? ret : -ESTALE;
    }
    // 安装租约仍在，允许恢复本次rename生效但目录同步失败的状态。
    transaction->state = current;
    desired = current;
    desired.target = desired.stable;
    desired.upgrade = LINKG_UPGRADE_PHASE_FAILED;
    ret = linkg_release_replace(upgrade_commit_state_path(), &transaction->state, &desired);
    if (ret == 0)
    {
        transaction->state = desired;
        ret = linkg_boot_stable(desired.kernel[desired.stable]);
    }
    return ret;
}

/**
 * @brief 释放安装租约，不删除锁文件或中断ENV工具。
 */
void upgrade_commit_end(upgrade_commit_t *transaction)
{
    if (transaction->lease_fd >= 0)
    {
        close(transaction->lease_fd);
        transaction->lease_fd = -1;
    }
}

/**
 * @brief 请求应用既有清理及整机重启流程，不在工作线程直接重启。
 */
int upgrade_commit_reboot(void)
{
    return linkg_lifecycle_request_exit(LINKG_EXIT_ACTION_REBOOT_SYSTEM);
}

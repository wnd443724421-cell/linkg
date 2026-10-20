/**
 * @file supervisor_release.c
 * @brief Release选择、可中断确认事务及失败回滚协调
 * @author Dawn
 * @version 0.2.0
 * @date 2026-10-19
 */

#include "supervisor_release.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "supervisor_boot.h"
#include "supervisor_links.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 确认目录和实际Kernel均匹配，禁止用期望ENV冒充运行状态。
 */
static int _supervisor_release_check(supervisor_release_t *release, unsigned int target, supervisor_kernel_t *actual)
{
    int ret;

    ret = supervisor_boot_actual(actual);
    if (ret == 0)
    {
        ret = supervisor_version_check(release->paths.app_root, release->paths.web_root, target);
    }

    return ret;
}

/**
 * @brief ENV成功后才提交稳定JSON，内存状态仅在持久化成功后更新。
 */
static int _supervisor_release_commit(supervisor_release_t *release, const supervisor_version_t *confirmed)
{
    int ret;

    ret = supervisor_boot_stable(confirmed->kernel[confirmed->stable]);
    if (ret == 0)
    {
        ret = supervisor_boot_checkpoint();
    }
    if (ret != 0)
    {
        return ret;
    }

    ret = supervisor_version_replace(release->paths.state_path, &release->version, confirmed);
    if (ret != 0)
    {
        return ret;
    }

    release->version = *confirmed;
    release->trial   = false;
    fprintf(stderr, "Supervisor release=CONFIRMED stable=%s kernel=%s\n",
            supervisor_version_name(confirmed->stable), supervisor_version_kernel_name(confirmed->kernel[confirmed->stable]));
    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化借用路径，不在获取进程锁前操作持久化状态。
 */
int supervisor_release_init(supervisor_release_t *release, const supervisor_release_paths_t *paths)
{
    if (paths->state_path == NULL || paths->app_root == NULL || paths->web_root == NULL ||
        paths->state_path[0] != '/' || paths->app_root[0] != '/' || paths->web_root[0] != '/')
    {
        return -EINVAL;
    }

    memset(release, 0, sizeof(*release));
    release->paths = *paths;
    return supervisor_version_program(paths->app_root, release->program, sizeof(release->program));
}

/**
 * @brief 回收旧子进程后恢复稳定目标及ENV，跨Kernel时请求重启而不启动旧App。
 */
int supervisor_release_rollback(supervisor_release_t *release)
{
    supervisor_version_t desired;
    supervisor_kernel_t  actual;
    int                  ret;

    ret = _supervisor_release_check(release, release->version.stable, &actual);
    if (ret != 0)
    {
        return ret;
    }

    desired        = release->version;
    desired.target = desired.stable;
    desired.upgrade = LINKG_UPGRADE_PHASE_ROLLING_BACK;
    // 先持久化回滚目标；中途断电后仍能从JSON继续修复ENV及软链接。
    ret = supervisor_version_replace(release->paths.state_path, &release->version, &desired);

    if (ret != 0)
    {
        return ret;
    }

    release->version = desired;
    ret = supervisor_boot_stable(desired.kernel[desired.stable]);
    if (ret != 0)
    {
        return ret;
    }

    release->trial = false;
    fprintf(stderr, "Supervisor release=ROLLBACK stable=%s kernel=%s reboot=%d\n",
            supervisor_version_name(desired.stable), supervisor_version_kernel_name(desired.kernel[desired.stable]),
            actual != desired.kernel[desired.stable]);
    return actual == desired.kernel[desired.stable] ? 0 : supervisor_boot_reboot();
}

/**
 * @brief 锁内读取状态、恢复中断事务、验证实际Kernel并协调两条软链接。
 */
static int _supervisor_release_prepare(supervisor_release_t *release)
{
    supervisor_version_t confirmed;
    supervisor_kernel_t  actual;
    bool                 env_confirmed;
    int                  ret;

    ret = supervisor_version_load(release->paths.state_path, &release->version);
    if (ret != 0)
    {
        return ret;
    }

    release->trial = release->version.target != release->version.stable;
    ret = _supervisor_release_check(release, release->version.stable, &actual);
    if (ret != 0)
    {
        return ret;
    }

    ret = release->version.kernel[release->version.target] == SUPERVISOR_KERNEL_NONE ? -ENOENT :
          supervisor_version_check(release->paths.app_root, release->paths.web_root, release->version.target);
    if (release->trial && (ret != 0 || actual != release->version.kernel[release->version.target]))
    {
        // 包括U-Boot已经回退稳定Kernel、但JSON仍指向候选的情况。
        ret = supervisor_release_rollback(release);
        if (ret != 0)
        {
            return ret;
        }
    }
    else if (ret != 0)
    {
        return ret;
    }
    else if (release->trial && release->version.kernel[release->version.target] != release->version.kernel[release->version.stable])
    {
        ret = supervisor_boot_is_stable(release->version.kernel[release->version.target], &env_confirmed);
        if (ret != 0)
        {
            return ret;
        }

        if (env_confirmed)
        {
            // 只有跨Kernel且四项ENV完整确认，才能补完ENV先于JSON的成功事务。
            confirmed        = release->version;
            confirmed.stable = confirmed.target;
            confirmed.upgrade = LINKG_UPGRADE_PHASE_COMPLETED;
            fprintf(stderr, "Supervisor ENV confirmation recovering\n");
            ret = _supervisor_release_commit(release, &confirmed);
            if (ret != 0)
            {
                return ret;
            }
        }
    }

    if (!release->trial)
    {
        ret = supervisor_boot_stable(release->version.kernel[release->version.stable]);
        if (ret == 0 && actual != release->version.kernel[release->version.stable])
        {
            return supervisor_boot_reboot();
        }
    }
    else
    {
        ret = supervisor_boot_trial(release->version.kernel[release->version.target], release->version.kernel[release->version.stable]);
    }

    if (ret == 0)
    {
        ret = supervisor_links_select(release->paths.app_root, release->paths.web_root, release->version.target);
    }

    if (ret != 0 && release->trial)
    {
        fprintf(stderr, "Supervisor candidate preparation failed error=%d\n", ret);
        ret = supervisor_release_rollback(release);
        return ret == 0 ? -EAGAIN : ret;
    }

    if (ret == 0 && release->trial)
    {
        confirmed = release->version;
        confirmed.upgrade = LINKG_UPGRADE_PHASE_TESTING;
        ret = supervisor_version_replace(release->paths.state_path, &release->version, &confirmed);
        if (ret == 0)
        {
            release->version = confirmed;
        }
        else
        {
            // 阶段显示失败不跳过必要启动，确认仍需完整事务校验。
            fprintf(stderr, "Supervisor testing phase pending error=%d\n", ret);
            ret = 0;
        }
    }

    if (ret == 0)
    {
        fprintf(stderr, "Supervisor release=SELECTED target=%s stable=%s trial=%d\n",
                supervisor_version_name(release->version.target), supervisor_version_name(release->version.stable), (int)release->trial);
    }

    return ret;
}

/**
 * @brief 连续健康窗口通过后先提交完整ENV，再提交稳定JSON，不创建额外状态文件。
 */
int supervisor_release_confirm(supervisor_release_t *release)
{
    supervisor_version_t current;
    supervisor_version_t confirmed;
    supervisor_kernel_t  actual;
    int                  ret;

    if (!release->trial)
    {
        return 0;
    }

    ret = _supervisor_release_check(release, release->version.target, &actual);
    if (ret == 0 && actual != release->version.kernel[release->version.target])
    {
        ret = -ESTALE;
    }

    if (ret == 0)
    {
        ret = supervisor_version_load(release->paths.state_path, &current);
    }

    if (ret == 0 && current.target == release->version.target && current.stable == release->version.stable &&
        current.kernel[0] == release->version.kernel[0] && current.kernel[1] == release->version.kernel[1] &&
        current.upgrade == LINKG_UPGRADE_PHASE_TESTING)
    {
        // testing的rename可能成功但目录同步失败，读回同一候选后继续安全确认。
        release->version = current;
    }
    confirmed         = release->version;
    confirmed.stable  = confirmed.target;
    confirmed.upgrade = LINKG_UPGRADE_PHASE_COMPLETED;
    if (ret == 0 && !supervisor_version_equal(&current, &release->version) && !supervisor_version_equal(&current, &confirmed))
    {
        ret = -ESTALE;
    }

    if (ret != 0)
    {
        return ret;
    }

    return _supervisor_release_commit(release, &confirmed);
}

/**
 * @brief 安装租约保护启动恢复，避免安装期间按旧快照修改版本状态。
 */
int supervisor_release_prepare(supervisor_release_t *release)
{
    int descriptor;
    int ret;

    descriptor = linkg_release_lock(release->paths.state_path, ".ota.lock", true);
    if (descriptor < 0)
    {
        return descriptor;
    }

    ret = _supervisor_release_prepare(release);
    close(descriptor);
    return ret;
}

/**
 * @brief 稳定App已READY后才记录回滚完成，安装中断结果可安全重试。
 */
void supervisor_release_running(supervisor_release_t *release)
{
    supervisor_version_t desired;
    int                  ret;

    desired = release->version;
    if (desired.upgrade == LINKG_UPGRADE_PHASE_ROLLING_BACK)
    {
        desired.upgrade = LINKG_UPGRADE_PHASE_ROLLED_BACK;
    }
    else if (desired.upgrade == LINKG_UPGRADE_PHASE_VERIFYING || desired.upgrade == LINKG_UPGRADE_PHASE_INSTALLING ||
             desired.upgrade == LINKG_UPGRADE_PHASE_FLASHING || desired.upgrade == LINKG_UPGRADE_PHASE_REBOOTING ||
             desired.upgrade == LINKG_UPGRADE_PHASE_TESTING)
    {
        desired.upgrade = LINKG_UPGRADE_PHASE_FAILED;
    }
    else
    {
        return;
    }

    ret = supervisor_version_replace(release->paths.state_path, &release->version, &desired);
    if (ret == 0)
    {
        release->version = desired;
        release->status_warned = false;
    }
    else if (!release->status_warned)
    {
        fprintf(stderr, "Supervisor final phase pending error=%d\n", ret);
        release->status_warned = true;
    }
}

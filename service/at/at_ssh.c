/**
 * @file at_ssh.c
 * @brief LinkG AT临时SSH维护生命周期
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-26
 */

#include "at_internal.h"

#include <errno.h>
#include <stdint.h>

#include "linkg_log.h"
#include "linkg_os.h"
#include "linkg_platform_config.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_AT_SSH_TIMEOUT_US       3600000000ULL // 单次维护有效期，60分钟
#define LINKG_AT_SSH_POLL_MS          1000         // 维护期间检查周期，单位毫秒
#define LINKG_AT_SSH_RETRY_US         5000000ULL   // 关闭失败重试间隔，5秒
#define LINKG_AT_SSH_STATUS_TRIES     10U          // 状态确认次数
#define LINKG_AT_SSH_STATUS_DELAY_MS  100U         // 状态确认间隔，单位毫秒

/****************************** 内部类型 ******************************/

typedef struct
{
    uint64_t deadline_us; // 维护截止时间，不因重复开启而延长
    uint64_t retry_us;    // 下次关闭重试时间
    bool     managed;     // AT已接管本次SSH维护服务
    bool     stopping;    // 正在关闭，不再允许开启
} linkg_at_ssh_context_t;

/****************************** 全局上下文 ******************************/

// 运行期间仅由AT服务线程访问，线程回收后由生命周期接口接管。
static linkg_at_ssh_context_t g_at_ssh;

/****************************** 内部辅助 ******************************/

/**
 * @brief 有界等待Dropbear实际进程状态与目标状态一致。
 */
static int _linkg_at_ssh_wait_state(bool enable)
{
    unsigned int index;
    bool         running;
    int          ret;

    for (index = 0U; index < LINKG_AT_SSH_STATUS_TRIES; index++)
    {
        ret = linkg_os_get_ssh_running(&running);
        if (ret != 0)
        {
            return ret;
        }

        if (running == enable)
        {
            return 0;
        }

        if (index + 1U < LINKG_AT_SSH_STATUS_TRIES)
        {
            ret = linkg_time_sleep_ms(LINKG_AT_SSH_STATUS_DELAY_MS);
            if (ret != 0)
            {
                return ret;
            }
        }
    }

    return -ETIMEDOUT;
}

/**
 * @brief 关闭SSH并确认进程退出，失败时保留待清理状态。
 */
static int _linkg_at_ssh_close(void)
{
    int ret;

    g_at_ssh.managed  = true;
    g_at_ssh.stopping = true;

    ret = linkg_os_set_ssh_enable(false);
    if (ret == 0)
    {
        ret = _linkg_at_ssh_wait_state(false);
    }

    if (ret != 0)
    {
        g_at_ssh.retry_us = linkg_time_monotonic_us() + LINKG_AT_SSH_RETRY_US;
        LINKG_LOG_WARN("AT: SSH maintenance cleanup pending, error=%d", ret);
        return ret;
    }

    g_at_ssh = (linkg_at_ssh_context_t){0};
    LINKG_LOG_INFO("AT: SSH maintenance closed");

    return 0;
}

/****************************** 维护控制 ******************************/

/**
 * @brief 正式版本启动时检查SSH关闭策略，失败交由服务线程重试。
 */
void _linkg_at_ssh_prepare(void)
{
    bool running;

    if (LINKG_PLATFORM_SSH_ENABLE == 0U &&
        (linkg_os_get_ssh_running(&running) != 0 || running))
    {
        (void)_linkg_at_ssh_close();
    }
}

/**
 * @brief 开启或关闭临时SSH，重复开启不延长原有期限。
 */
int _linkg_at_ssh_set(bool enable)
{
    uint64_t now_us;
    int      ret;

    if (!enable)
    {
        bool running;

        if (!g_at_ssh.managed && linkg_os_get_ssh_running(&running) == 0 && !running)
        {
            return 0;
        }

        return _linkg_at_ssh_close();
    }

    now_us = linkg_time_monotonic_us();
    if (now_us == 0U)
    {
        return -EIO;
    }

    if (g_at_ssh.managed)
    {
        if (g_at_ssh.stopping || now_us >= g_at_ssh.deadline_us)
        {
            return -EBUSY;
        }

        ret = _linkg_at_ssh_wait_state(true);
        now_us = linkg_time_monotonic_us();
        if (ret == 0 && (now_us == 0U || now_us >= g_at_ssh.deadline_us))
        {
            ret = -ETIMEDOUT;
        }

        if (ret != 0)
        {
            (void)_linkg_at_ssh_close();
        }

        return ret;
    }

    // 启动失败也必须尝试清理可能已经启动的服务。
    g_at_ssh.managed     = true;
    g_at_ssh.deadline_us = now_us + LINKG_AT_SSH_TIMEOUT_US;

    ret = linkg_os_set_ssh_enable(true);
    if (ret == 0)
    {
        ret = _linkg_at_ssh_wait_state(true);
    }

    now_us = linkg_time_monotonic_us();
    if (ret == 0 && (now_us == 0U || now_us >= g_at_ssh.deadline_us))
    {
        ret = -ETIMEDOUT;
    }

    if (ret != 0)
    {
        (void)_linkg_at_ssh_close();
        return ret;
    }

    LINKG_LOG_INFO("AT: SSH maintenance opened");

    return 0;
}

/**
 * @brief 查询实际Dropbear进程状态。
 */
int _linkg_at_ssh_get(bool *running)
{
    return linkg_os_get_ssh_running(running);
}

/**
 * @brief 无维护任务时无限等待，有任务时检查维护期限。
 */
int _linkg_at_ssh_poll_timeout(void)
{
    return g_at_ssh.managed ? LINKG_AT_SSH_POLL_MS : -1;
}

/**
 * @brief 检查SSH超时和待清理任务，不创建额外线程。
 */
void _linkg_at_ssh_tick(void)
{
    uint64_t now_us;

    if (!g_at_ssh.managed)
    {
        return;
    }

    now_us = linkg_time_monotonic_us();

    if (now_us != 0U)
    {
        if (g_at_ssh.stopping && now_us < g_at_ssh.retry_us)
        {
            return;
        }

        if (!g_at_ssh.stopping && now_us < g_at_ssh.deadline_us)
        {
            return;
        }
    }

    (void)_linkg_at_ssh_close();
}

/**
 * @brief 在AT服务退出时回收本模块开启的SSH服务。
 */
int _linkg_at_ssh_shutdown(void)
{
    if (!g_at_ssh.managed)
    {
        return 0;
    }

    return _linkg_at_ssh_close();
}

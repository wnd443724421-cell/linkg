/**
 * @file cellular_health.c
 * @brief LinkG蜂窝ONLINE维护与IPv4/IPv6公网健康检测实现
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-10
 */

#include "cellular_health.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "linkg_os.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"
#include "linkg_time.h"

#include "cellular_fsm_internal.h"
#include "cellular_internal.h"

/****************************** 检测策略 ******************************/

#define CELLULAR_HEALTH_NORMAL_INTERVAL_MS     30000U  // 正常检测周期
#define CELLULAR_HEALTH_FAILED_INTERVAL_MS      5000U  // 异常时完整检测周期
#define CELLULAR_HEALTH_PROBE_TIMEOUT_MS        3000U  // 单目标子进程的硬超时
#define CELLULAR_HEALTH_PROBE_POLL_MS            100U  // 检测子进程退出和停止请求的周期
#define CELLULAR_HEALTH_FAILED_ROUNDS             3U  // 连续三轮全部失败才确认不可达
#define CELLULAR_HEALTH_HOST_GRACE_MS            2500U  // 明确Host异常确认时间
#define CELLULAR_HEALTH_UNKNOWN_GRACE_MS        15000U  // 状态采集未知时的宽限时间
#define CELLULAR_HEALTH_RECOVERY_INTERVAL_MS    60000U  // 两次分级恢复之间的最小时间
#define CELLULAR_HEALTH_PDP_COOLDOWN_MS        600000U  // PDP重建后的冷却时间
#define CELLULAR_HEALTH_PDP_WINDOW_MS         3600000U  // PDP重建次数统计窗口
#define CELLULAR_HEALTH_PDP_MAX_PER_WINDOW          2U  // 每窗口最多执行两次PDP重建

/****************************** 探测目标 ******************************/

static const char *const g_cellular_health_targets_ipv4[] =
{
    "www.baidu.com",
    "www.taobao.com",
    "www.bilibili.com",
    "223.5.5.5" // 数值地址兜底，降低DNS故障造成的误判
};

static const char *const g_cellular_health_targets_ipv6[] =
{
    "www.baidu.com",
    "www.taobao.com",
    "www.bilibili.com",
    "2400:3200::1" // 数值IPv6地址兜底
};

/****************************** 恢复动作 ******************************/

typedef enum
{
    CELLULAR_HEALTH_RECOVERY_NONE = 0,
    CELLULAR_HEALTH_RECOVERY_HOST,
    CELLULAR_HEALTH_RECOVERY_NETDEV,
    CELLULAR_HEALTH_RECOVERY_PDP
} cellular_health_recovery_t;

/****************************** 内部上下文 ******************************/

typedef struct
{
    pthread_mutex_t       lock;                  // 只保护Health上下文，不持锁执行Ping
    linkg_thread_t        thread;                // 独立公网检测工作线程，不占用Owner
    cellular_health_info_t info;                 // 双栈检测结果
    uint64_t              epoch;                 // 连接切换代次，丢弃旧进程返回的结果
    uint64_t              next_probe_ms;         // 下次完整探测时间
    uint64_t              next_recovery_ms;      // 下一次允许升级恢复时间
    uint64_t              pdp_window_started_ms; // PDP重建次数统计窗口起点
    uint32_t              recovery_stage;        // 已执行的恢复级别，0=未执行
    uint32_t              pdp_rebuild_count;     // 当前窗口内已经发起的PDP重建次数
    bool                  initialized;           // 是否完成软件资源初始化
    bool                  started;               // 工作线程是否已启动
    bool                  online;                // FSM当前是否处于ONLINE
} cellular_health_context_t;

static cellular_health_context_t g_cellular_health =
{
    .lock = PTHREAD_MUTEX_INITIALIZER
};

/****************************** 时间和状态辅助 ******************************/

/**
 * @brief 计算不会产生无符号溢出的下一执行期限。
 */
static uint64_t _cellular_health_next_time(uint64_t now_ms, uint64_t delay_ms)
{
    return now_ms > UINT64_MAX - delay_ms ? UINT64_MAX : now_ms + delay_ms;
}

/**
 * @brief 失效当前双栈探测结果，调用者持有Health锁。
 */
static void _cellular_health_invalidate_locked(void)
{
    memset(&g_cellular_health.info, 0, sizeof(g_cellular_health.info));
    g_cellular_health.epoch++;
    g_cellular_health.next_probe_ms = 0U;
}

/**
 * @brief 等待检测线程唤醒或超时，不在Owner线程执行。
 */
static void _cellular_health_wait(linkg_thread_t *thread, int timeout_ms)
{
    struct pollfd descriptor;
    int           ret;

    descriptor.fd      = linkg_thread_get_wakeup_fd(thread);
    descriptor.events  = POLLIN;
    descriptor.revents = 0;

    do
    {
        ret = poll(&descriptor, 1U, timeout_ms);
    }
    while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

    if (ret > 0 && (descriptor.revents & POLLIN) != 0)
    {
        (void)linkg_thread_clear_wakeup(thread);
    }
}

/**
 * @brief 判断当前探测仍属于正在运行的同一次ONLINE会话。
 */
static bool _cellular_health_probe_active(linkg_thread_t *thread, uint64_t epoch)
{
    bool active;

    pthread_mutex_lock(&g_cellular_health.lock);
    active = g_cellular_health.started && g_cellular_health.online && g_cellular_health.epoch == epoch;
    pthread_mutex_unlock(&g_cellular_health.lock);

    return linkg_thread_is_running(thread) && active;
}

/****************************** Ping子进程 ******************************/

/**
 * @brief 只由探测线程等待并回收一个子进程，不丢弃退出码。
 *
 * @return 1表示收到ICMP响应，0表示目标未响应，负值表示探测设施错误或被取消。
 */
static int _cellular_health_ping(linkg_thread_t *thread, uint64_t epoch, bool ipv6, const char *target)
{
    pid_t    child;
    pid_t    waited;
    uint64_t deadline_ms;
    int      status;
    int      ret;

    if (!_cellular_health_probe_active(thread, epoch))
    {
        return -ECANCELED;
    }

    child = (pid_t)-1;
    if (ipv6)
    {
        ret = linkg_os_spawn(&child, "ping6", "-q", "-c", "1", "-W", "2", "-I", LINKG_RESOURCE_INTERFACE_CELLULAR, target, NULL);
    }
    else
    {
        ret = linkg_os_spawn(&child, "ping", "-4", "-q", "-c", "1", "-W", "2", "-I", LINKG_RESOURCE_INTERFACE_CELLULAR, target, NULL);
    }
    if (ret != 0)
    {
        return ret;
    }

    deadline_ms = _cellular_health_next_time(linkg_time_elapsed_ms(), CELLULAR_HEALTH_PROBE_TIMEOUT_MS);

    for (;;)
    {
        waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
        {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            {
                return 1;
            }

            if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
            {
                return -ENOENT;
            }

            return 0;
        }

        if (waited < 0 && errno != EINTR)
        {
            return errno != 0 ? -errno : -EIO;
        }

        if (!_cellular_health_probe_active(thread, epoch) || linkg_time_elapsed_ms() >= deadline_ms)
        {
            ret = _cellular_health_probe_active(thread, epoch) ? 0 : -ECANCELED;
            (void)kill(child, SIGKILL);

            do
            {
                waited = waitpid(child, &status, 0);
            }
            while (waited < 0 && errno == EINTR);

            return ret;
        }

        _cellular_health_wait(thread, (int)CELLULAR_HEALTH_PROBE_POLL_MS);
    }
}

/**
 * @brief 对一个地址族逐目标检测，任意目标成功就提前结束。
 */
static int _cellular_health_probe_family(linkg_thread_t *thread, uint64_t epoch, bool ipv6)
{
    const char *const *targets;
    size_t             count;
    size_t             index;
    int                ret;

    targets = ipv6 ? g_cellular_health_targets_ipv6 : g_cellular_health_targets_ipv4;
    count   = ipv6 ? sizeof(g_cellular_health_targets_ipv6) / sizeof(g_cellular_health_targets_ipv6[0])
                   : sizeof(g_cellular_health_targets_ipv4) / sizeof(g_cellular_health_targets_ipv4[0]);

    for (index = 0U; index < count; index++)
    {
        ret = _cellular_health_ping(thread, epoch, ipv6, targets[index]);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** 探测结果更新 ******************************/

/**
 * @brief 更新一个地址族的连续失败轮数和经过确认的可达性。
 */
static void _cellular_health_update_family(bool reachable, bool *valid, bool *available, uint32_t *failed_rounds)
{
    if (reachable)
    {
        *valid         = true;
        *available     = true;
        *failed_rounds = 0U;
        return;
    }

    if (*failed_rounds != UINT32_MAX)
    {
        (*failed_rounds)++;
    }

    if (*failed_rounds >= CELLULAR_HEALTH_FAILED_ROUNDS)
    {
        *valid     = true;
        *available = false;
    }
}

/**
 * @brief 提交同一会话的一轮双栈结果，丢弃已失效的异步结果。
 */
static void _cellular_health_commit_round(uint64_t epoch, int ipv4_result, int ipv6_result)
{
    uint64_t now_ms;
    bool     recovered;
    bool     failed;
    bool     status_changed;
    bool     ipv4_valid;
    bool     ipv4_available;
    bool     ipv6_valid;
    bool     ipv6_available;

    now_ms = linkg_time_elapsed_ms();
    status_changed = false;
    ipv4_valid = false;
    ipv4_available = false;
    ipv6_valid = false;
    ipv6_available = false;

    pthread_mutex_lock(&g_cellular_health.lock);

    if (g_cellular_health.started && g_cellular_health.online && g_cellular_health.epoch == epoch)
    {
        ipv4_valid     = g_cellular_health.info.ipv4_valid;
        ipv4_available = g_cellular_health.info.ipv4_available;
        ipv6_valid     = g_cellular_health.info.ipv6_valid;
        ipv6_available = g_cellular_health.info.ipv6_available;

        if (ipv4_result >= 0)
        {
            _cellular_health_update_family(ipv4_result > 0, &g_cellular_health.info.ipv4_valid, &g_cellular_health.info.ipv4_available, &g_cellular_health.info.ipv4_failed_rounds);
        }
        else
        {
            g_cellular_health.info.ipv4_valid = false;
            g_cellular_health.info.ipv4_failed_rounds = 0U;
        }

        if (ipv6_result >= 0)
        {
            _cellular_health_update_family(ipv6_result > 0, &g_cellular_health.info.ipv6_valid, &g_cellular_health.info.ipv6_available, &g_cellular_health.info.ipv6_failed_rounds);
        }
        else
        {
            g_cellular_health.info.ipv6_valid = false;
            g_cellular_health.info.ipv6_failed_rounds = 0U;
        }

        g_cellular_health.info.updated_ms = now_ms;

        recovered = (g_cellular_health.info.ipv4_valid && g_cellular_health.info.ipv4_available) ||
                    (g_cellular_health.info.ipv6_valid && g_cellular_health.info.ipv6_available);
        failed = (ipv4_result == 0 || ipv6_result == 0);

        if (recovered)
        {
            g_cellular_health.recovery_stage = 0U;
            g_cellular_health.next_recovery_ms = 0U;
        }

        g_cellular_health.next_probe_ms = _cellular_health_next_time(now_ms, failed ? CELLULAR_HEALTH_FAILED_INTERVAL_MS : CELLULAR_HEALTH_NORMAL_INTERVAL_MS);

        status_changed = ipv4_valid != g_cellular_health.info.ipv4_valid ||
                         (ipv4_valid && ipv4_available != g_cellular_health.info.ipv4_available) ||
                         ipv6_valid != g_cellular_health.info.ipv6_valid ||
                         (ipv6_valid && ipv6_available != g_cellular_health.info.ipv6_available);
        ipv4_valid     = g_cellular_health.info.ipv4_valid;
        ipv4_available = g_cellular_health.info.ipv4_available;
        ipv6_valid     = g_cellular_health.info.ipv6_valid;
        ipv6_available = g_cellular_health.info.ipv6_available;
    }

    pthread_mutex_unlock(&g_cellular_health.lock);

    if (status_changed)
    {
        CELLULAR_INFO("public health changed, ipv4=%s, ipv6=%s", ipv4_valid ? (ipv4_available ? "reachable" : "unreachable") : "unknown", ipv6_valid ? (ipv6_available ? "reachable" : "unreachable") : "unknown");
    }

    if (ipv4_result < 0 && ipv4_result != -ECANCELED)
    {
        CELLULAR_WARN("IPv4 public probe unavailable, error=%d", ipv4_result);
    }

    if (ipv6_result < 0 && ipv6_result != -ECANCELED)
    {
        CELLULAR_WARN("IPv6 public probe unavailable, error=%d", ipv6_result);
    }
}

/**
 * @brief 单个工作线程按次序执行双栈检测，不能阻塞Owner。
 */
static void _cellular_health_worker(linkg_thread_t *thread, void *user_data)
{
    uint64_t epoch;
    uint64_t deadline_ms;
    uint64_t now_ms;
    bool     online;
    int      ipv4_result;
    int      ipv6_result;
    int      timeout_ms;

    (void)user_data;

    while (linkg_thread_is_running(thread))
    {
        pthread_mutex_lock(&g_cellular_health.lock);
        online      = g_cellular_health.started && g_cellular_health.online;
        epoch       = g_cellular_health.epoch;
        deadline_ms = g_cellular_health.next_probe_ms;
        pthread_mutex_unlock(&g_cellular_health.lock);

        if (!online)
        {
            _cellular_health_wait(thread, -1);
            continue;
        }

        now_ms = linkg_time_elapsed_ms();
        if (deadline_ms > now_ms)
        {
            timeout_ms = deadline_ms - now_ms > 30000U ? 30000 : (int)(deadline_ms - now_ms);
            _cellular_health_wait(thread, timeout_ms);
            continue;
        }

        ipv4_result = _cellular_health_probe_family(thread, epoch, false);
        if (ipv4_result == -ECANCELED)
        {
            continue;
        }

        ipv6_result = _cellular_health_probe_family(thread, epoch, true);
        if (ipv6_result == -ECANCELED)
        {
            continue;
        }

        _cellular_health_commit_round(epoch, ipv4_result, ipv6_result);
    }
}

/****************************** 模块生命周期 ******************************/

/**
 * @brief 初始化公网检测资源，暂时不启动探测线程。
 */
int cellular_health_init(void)
{
    int ret;

    pthread_mutex_lock(&g_cellular_health.lock);
    if (g_cellular_health.initialized)
    {
        pthread_mutex_unlock(&g_cellular_health.lock);
        return -EALREADY;
    }

    _cellular_health_invalidate_locked();
    g_cellular_health.recovery_stage        = 0U;
    g_cellular_health.next_recovery_ms      = 0U;
    g_cellular_health.pdp_rebuild_count     = 0U;
    g_cellular_health.pdp_window_started_ms = 0U;
    g_cellular_health.online               = false;
    g_cellular_health.started              = false;
    pthread_mutex_unlock(&g_cellular_health.lock);

    ret = linkg_thread_init(&g_cellular_health.thread, "cell-health", _cellular_health_worker, NULL);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_cellular_health.lock);
    g_cellular_health.initialized = true;
    pthread_mutex_unlock(&g_cellular_health.lock);

    return 0;
}

/**
 * @brief 启动公网探测线程，尚未ONLINE时保持休眠。
 */
int cellular_health_start(void)
{
    int ret;

    pthread_mutex_lock(&g_cellular_health.lock);
    if (!g_cellular_health.initialized || g_cellular_health.started)
    {
        ret = !g_cellular_health.initialized ? -ENODEV : -EALREADY;
        pthread_mutex_unlock(&g_cellular_health.lock);
        return ret;
    }

    g_cellular_health.started = true;
    pthread_mutex_unlock(&g_cellular_health.lock);

    ret = linkg_thread_start(&g_cellular_health.thread);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_cellular_health.lock);
        g_cellular_health.started = false;
        pthread_mutex_unlock(&g_cellular_health.lock);
    }

    return ret;
}

/**
 * @brief 终止在运行的Ping并回收工作线程。
 */
int cellular_health_stop(void)
{
    bool started;
    int  ret;

    pthread_mutex_lock(&g_cellular_health.lock);
    started = g_cellular_health.started;
    g_cellular_health.online = false;
    _cellular_health_invalidate_locked();
    pthread_mutex_unlock(&g_cellular_health.lock);

    if (!started)
    {
        return 0;
    }

    ret = linkg_thread_stop(&g_cellular_health.thread);
    if (ret == 0)
    {
        pthread_mutex_lock(&g_cellular_health.lock);
        g_cellular_health.started = false;
        pthread_mutex_unlock(&g_cellular_health.lock);
    }

    return ret;
}

/**
 * @brief 反初始化Health软件资源。
 */
void cellular_health_deinit(void)
{
    if (cellular_health_stop() != 0)
    {
        CELLULAR_WARN("health thread stop failed, resources retained");
        return;
    }

    pthread_mutex_lock(&g_cellular_health.lock);
    if (!g_cellular_health.initialized)
    {
        pthread_mutex_unlock(&g_cellular_health.lock);
        return;
    }

    g_cellular_health.initialized = false;
    pthread_mutex_unlock(&g_cellular_health.lock);

    linkg_thread_deinit(&g_cellular_health.thread);
}

/****************************** 状态查询与会话同步 ******************************/

/**
 * @brief 设置ONLINE状态；离开ONLINE立即使旧公网结果失效。
 */
void cellular_health_set_online(bool online)
{
    bool changed;

    pthread_mutex_lock(&g_cellular_health.lock);
    changed = g_cellular_health.started && g_cellular_health.online != online;
    if (changed)
    {
        g_cellular_health.online = online;
        _cellular_health_invalidate_locked();
    }
    pthread_mutex_unlock(&g_cellular_health.lock);

    if (changed)
    {
        (void)linkg_thread_wakeup(&g_cellular_health.thread);
    }
}

/**
 * @brief 新SIM会话开始或结束时清除旧检测和恢复阶段。
 */
void cellular_health_reset_session(void)
{
    pthread_mutex_lock(&g_cellular_health.lock);
    g_cellular_health.online = false;
    _cellular_health_invalidate_locked();
    g_cellular_health.recovery_stage = 0U;
    g_cellular_health.next_recovery_ms = 0U;
    pthread_mutex_unlock(&g_cellular_health.lock);

    if (g_cellular_health.started)
    {
        (void)linkg_thread_wakeup(&g_cellular_health.thread);
    }
}

/**
 * @brief 读取最近一轮双栈检测结果；有效性表示可区分尚未检测和真实不可达。
 */
int cellular_health_get_info(cellular_health_info_t *info)
{
    if (info == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_cellular_health.lock);
    if (!g_cellular_health.initialized)
    {
        pthread_mutex_unlock(&g_cellular_health.lock);
        return -ENODEV;
    }

    *info = g_cellular_health.info;
    pthread_mutex_unlock(&g_cellular_health.lock);

    return 0;
}

/**
 * @brief 只在IPv4、IPv6均已确认连续失败且本轮事实未过期时返回真。
 */
static bool _cellular_health_dual_unreachable_locked(uint64_t now_ms)
{
    return g_cellular_health.started && g_cellular_health.online &&
           g_cellular_health.info.ipv4_valid && !g_cellular_health.info.ipv4_available &&
           g_cellular_health.info.ipv6_valid && !g_cellular_health.info.ipv6_available &&
           g_cellular_health.info.ipv4_failed_rounds >= CELLULAR_HEALTH_FAILED_ROUNDS &&
           g_cellular_health.info.ipv6_failed_rounds >= CELLULAR_HEALTH_FAILED_ROUNDS &&
           g_cellular_health.info.updated_ms != 0U && now_ms >= g_cellular_health.info.updated_ms &&
           now_ms - g_cellular_health.info.updated_ms <= CELLULAR_HEALTH_NORMAL_INTERVAL_MS;
}

/**
 * @brief 验证双栈公网都持续不可达，不接受单地址族失败。
 */
bool cellular_health_dual_unreachable(void)
{
    bool unavailable;

    pthread_mutex_lock(&g_cellular_health.lock);
    unavailable = _cellular_health_dual_unreachable_locked(linkg_time_elapsed_ms());
    pthread_mutex_unlock(&g_cellular_health.lock);

    return unavailable;
}

/**
 * @brief 确定当前允许执行的下一恢复级别。
 */
static cellular_health_recovery_t _cellular_health_get_recovery(uint64_t now_ms)
{
    cellular_health_recovery_t recovery;

    pthread_mutex_lock(&g_cellular_health.lock);
    recovery = CELLULAR_HEALTH_RECOVERY_NONE;

    if (_cellular_health_dual_unreachable_locked(now_ms) && now_ms >= g_cellular_health.next_recovery_ms)
    {
        if (g_cellular_health.recovery_stage == 0U)
        {
            recovery = CELLULAR_HEALTH_RECOVERY_HOST;
        }
        else if (g_cellular_health.recovery_stage == 1U)
        {
            recovery = CELLULAR_HEALTH_RECOVERY_NETDEV;
        }
        else
        {
            if (g_cellular_health.pdp_window_started_ms == 0U ||
                now_ms < g_cellular_health.pdp_window_started_ms ||
                now_ms - g_cellular_health.pdp_window_started_ms >= CELLULAR_HEALTH_PDP_WINDOW_MS)
            {
                g_cellular_health.pdp_window_started_ms = now_ms;
                g_cellular_health.pdp_rebuild_count = 0U;
            }

            if (g_cellular_health.pdp_rebuild_count < CELLULAR_HEALTH_PDP_MAX_PER_WINDOW)
            {
                recovery = CELLULAR_HEALTH_RECOVERY_PDP;
            }
        }
    }

    pthread_mutex_unlock(&g_cellular_health.lock);
    return recovery;
}

/**
 * @brief 记录已决定的恢复级别并清空当前探测事实，等待恢复后重新验证。
 */
static void _cellular_health_note_recovery(cellular_health_recovery_t recovery, uint64_t now_ms)
{
    pthread_mutex_lock(&g_cellular_health.lock);

    if (recovery == CELLULAR_HEALTH_RECOVERY_HOST && g_cellular_health.recovery_stage < 1U)
    {
        g_cellular_health.recovery_stage = 1U;
    }
    else if (recovery == CELLULAR_HEALTH_RECOVERY_NETDEV && g_cellular_health.recovery_stage < 2U)
    {
        g_cellular_health.recovery_stage = 2U;
    }
    else if (recovery == CELLULAR_HEALTH_RECOVERY_PDP)
    {
        g_cellular_health.recovery_stage = 3U;
        g_cellular_health.pdp_rebuild_count++;
    }

    g_cellular_health.next_recovery_ms = _cellular_health_next_time(now_ms, recovery == CELLULAR_HEALTH_RECOVERY_PDP ? CELLULAR_HEALTH_PDP_COOLDOWN_MS : CELLULAR_HEALTH_RECOVERY_INTERVAL_MS);
    // 保留本轮双栈失败证据，供紧接着执行的FSM失败恢复入口再次验证。
    pthread_mutex_unlock(&g_cellular_health.lock);
}

/****************************** 本地连接诊断 ******************************/

/**
 * @brief 根据最新事实确认本地Host IPv6是否出现异常。
 */
static bool _cellular_health_ipv6_facts_current(const cellular_status_info_t *info)
{
    return info != NULL &&
           _cellular_fsm_meta_current(&info->host.interface_meta) &&
           _cellular_fsm_meta_current(&info->host.interface_up_meta) &&
           _cellular_fsm_meta_current(&info->host.ipv6_meta) &&
           _cellular_fsm_meta_current(&info->host.ipv6_route_meta);
}

/**
 * @brief ONLINE阶段优先恢复明确失效的本地连接，再考虑公网故障升级。
 */
cellular_fsm_step_t _cellular_fsm_state_online(cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms)
{
    cellular_health_recovery_t recovery;
    uint64_t                   grace_ms;
    uint8_t                    pdp_cid;
    int                        ret;

    if (fsm == NULL)
    {
        return _cellular_fsm_step_fatal(-EINVAL);
    }

    if (info != NULL && _cellular_fsm_meta_current(&info->local.sim_meta) && info->local.sim_state != LINKG_CELLULAR_SIM_STATE_READY)
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_CHECK_SIM);
    }

    if (info != NULL && _cellular_fsm_meta_current(&info->network.registration_meta) &&
        info->network.registration != LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED)
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION);
    }

    if (info != NULL && _cellular_fsm_meta_current(&info->pdp.active_meta) && !info->pdp.active)
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_ACTIVATE_PDP);
    }

    if (info != NULL && _cellular_fsm_meta_current(&info->netdev.state.meta))
    {
        if (!info->netdev.state.connected)
        {
            return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_START_NETDEV);
        }

        if (!cellular_runtime_get_pdp_cid(&fsm->runtime, &pdp_cid))
        {
            return _cellular_fsm_step_fatal(-EPROTO);
        }

        if (info->netdev.state.cid != pdp_cid)
        {
            CELLULAR_WARN("ONLINE QNETDEV CID conflict, actual=%u, selected=%u", (unsigned int)info->netdev.state.cid, (unsigned int)pdp_cid);
            return _cellular_fsm_step_failed(-EBUSY);
        }
    }

    _cellular_fsm_host_ipv4_maintain(info, now_ms);

    if (!_cellular_fsm_host_ready(info))
    {
        grace_ms = _cellular_health_ipv6_facts_current(info) ? CELLULAR_HEALTH_HOST_GRACE_MS : CELLULAR_HEALTH_UNKNOWN_GRACE_MS;

        if (fsm->runtime.next_action_ms == 0U)
        {
            ret = cellular_runtime_schedule_action(&fsm->runtime, now_ms, grace_ms);
            if (ret != 0)
            {
                return _cellular_fsm_step_fatal(ret);
            }
        }
        else if (cellular_runtime_action_due(&fsm->runtime, now_ms))
        {
            CELLULAR_WARN("ONLINE Host IPv6 not ready, retrying Host stage only");
            return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_PREPARE_HOST);
        }

        return _cellular_fsm_step_wait();
    }

    cellular_runtime_clear_action(&fsm->runtime, now_ms);

    recovery = _cellular_health_get_recovery(now_ms);
    if (recovery == CELLULAR_HEALTH_RECOVERY_NONE)
    {
        return _cellular_fsm_step_wait();
    }

    _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_PDP | CELLULAR_STATUS_REFRESH_NETDEV |
                                         CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK | CELLULAR_STATUS_REFRESH_HOST);

    _cellular_health_note_recovery(recovery, now_ms);

    if (recovery == CELLULAR_HEALTH_RECOVERY_HOST)
    {
        CELLULAR_WARN("both IPv4 and IPv6 public probes failed, attempting Host recovery");
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_PREPARE_HOST);
    }

    if (recovery == CELLULAR_HEALTH_RECOVERY_NETDEV)
    {
        CELLULAR_WARN("dual-stack public outage persisted, requesting controlled QNETDEV restart");
        return _cellular_fsm_step_failed(-ENOLINK);
    }

    CELLULAR_WARN("dual-stack public outage persisted after Host and QNETDEV recovery, requesting limited PDP rebuild");
    return _cellular_fsm_step_failed(-ENETUNREACH);
}

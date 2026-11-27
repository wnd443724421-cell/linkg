/**
 * @file network_rf_test.c
 * @brief LinkG独立蜂窝射频测试任务
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-27
 */

#define _POSIX_C_SOURCE 200809L // POSIX接口

#include "linkg_rf_test.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "linkg_log.h"
#include "linkg_network_task.h"
#include "linkg_time.h"
#include "at_channel.h"
#include "rg255_cmd.h"
#include "rg255_rf.h"

/****************************** 模块常量 ******************************/

#define LINKG_RF_AT_DEVICE      "/dev/ttyUSB1"         // RG255 AT控制串口
#define LINKG_RF_AT_BAUDRATE    115200                 // AT波特率
#define LINKG_RF_READY_MS       30000U                 // AT就绪等待上限
#define LINKG_RF_RETRY_MS       500U                   // AT重试周期
#define LINKG_RF_TICK_MS        100U                   // 强发取消检查周期
#define LINKG_RF_MODE_TRIES     5U                     // 模式切换后最多查询次数
#define LINKG_RF_MODE_POLL_MS   200U                   // 模式切换后查询间隔
#define LINKG_RF_MARKER_DIR     "/app"                 // 应用持久状态目录
#define LINKG_RF_MARKER         LINKG_RF_MARKER_DIR "/.linkg-rf-pending" // 设备重启后仍保留
#define LINKG_RF_LEGACY_MARKER  "/tmp/linkg-rf-pending" // 兼容旧版本的遗留任务
#define LINKG_RF_BW_BIT(code)   (1U << (code))          // FTM带宽编码位图

/****************************** 频段配置 ******************************/

// 基于FTM强发信道表与上行频段范围；带宽集合为保守候选，需板端确认。
static const linkg_rf_band_t g_rf_bands[] =
{
    {1,  1, 1920000, 1980000,  18000,  18599, 1, 100, 0x3c, 1950000},
    {1,  3, 1710000, 1785000,  19200,  19949, 1, 100, 0x3c, 1747500},
    {1,  5,  824000,  849000,  20400,  20649, 1, 100, 0x0f,  836500},
    {1,  8,  880000,  915000,  21450,  21799, 1, 100, 0x0f,  897500},
    {1, 28,  703000,  748000,  27210,  27659, 1, 100, 0x3c,  725500},
    {1, 39, 1880000, 1920000,  38250,  38649, 1, 100, 0x3c, 1900000},
    {1, 40, 2300000, 2400000,  38650,  39649, 1, 100, 0x3c, 2350000},
    {1, 41, 2496000, 2690000,  39650,  41589, 1, 100, 0x3c, 2593000},
    {2,  1, 1920000, 1980000, 384000, 396000, 20,  5, 0x7c, 1950000},
    {2, 28,  703000,  748000, 140600, 149600, 20,  5, 0x3c,  725000},
    {2, 41, 2496000, 2690000, 499200, 537999, 3,   5, 0x578, 2593005},
    {2, 78, 3300000, 3800000, 620000, 653333, 1,  15, 0x578, 3500000},
    {2, 79, 4400000, 5000000, 693334, 733333, 1,  15, 0x540, 4800000}
};

/****************************** 全局上下文 ******************************/

static pthread_mutex_t   g_rf_lock = PTHREAD_MUTEX_INITIALIZER; // 状态快照锁
static linkg_rf_status_t  g_rf_status;                           // 最近一次诊断任务
static bool               g_rf_requires_online;                 // 需要验证Online后才能恢复原Owner
static at_channel_t     *g_rf_unreleased_channel;              // 持有尚未成功停止的AT通道
static uint32_t           g_rf_last_rat;                         // 上一次尝试FTM的制式，仅任务线程访问

/****************************** 参数校验 ******************************/

/**
 * @brief 返回候选频段表。
 */
const linkg_rf_band_t *linkg_rf_test_bands(size_t *count)
{
    if (count != NULL)
    {
        *count = sizeof(g_rf_bands) / sizeof(g_rf_bands[0]);
    }
    return g_rf_bands;
}

/**
 * @brief 根据文档FTM编码返回带宽，单位kHz。
 */
uint32_t linkg_rf_test_bandwidth_khz(uint32_t code)
{
    static const uint32_t bandwidths[] = {1400, 3000, 5000, 10000, 15000, 20000, 40000, 0, 80000, 0, 100000};

    return code < sizeof(bandwidths) / sizeof(bandwidths[0]) ? bandwidths[code] : 0U;
}

/**
 * @brief 将物理中心频率就近对齐至合法信道栅格并校验完整带宽。
 */
int linkg_rf_test_resolve(const linkg_rf_params_t *params, uint32_t *channel, uint32_t *actual_khz)
{
    const linkg_rf_band_t *band;
    uint32_t              bandwidth;
    uint32_t              origin;
    uint32_t              step;
    uint32_t              min_index;
    uint32_t              max_index;
    uint32_t              index;
    size_t                count;
    size_t                i;

    if (params == NULL || channel == NULL || actual_khz == NULL ||
        params->duration_sec < 10U || params->duration_sec > 120U)
    {
        return -EINVAL;
    }

    bandwidth = linkg_rf_test_bandwidth_khz(params->bandwidth_code);
    if (bandwidth == 0U)
    {
        return -EINVAL;
    }

    band = linkg_rf_test_bands(&count);
    for (i = 0U; i < count; i++)
    {
        if (band[i].rat == params->rat && band[i].band == params->band)
        {
            break;
        }
    }
    if (i == count)
    {
        return -EINVAL;
    }
    band += i;
    if ((band->bandwidth_mask & LINKG_RF_BW_BIT(params->bandwidth_code)) == 0U ||
        bandwidth > band->high_khz - band->low_khz ||
        params->frequency_khz < band->low_khz + bandwidth / 2U ||
        params->frequency_khz > band->high_khz - bandwidth / 2U)
    {
        return -EINVAL;
    }

    origin = band->rat == 1U ? band->low_khz :
             band->first_channel < 600000U ? band->first_channel * 5U :
             3000000U + (band->first_channel - 600000U) * 15U;
    step = band->channel_step * band->step_khz;
    min_index = (band->low_khz + bandwidth / 2U - origin + step - 1U) / step;
    max_index = (band->high_khz - bandwidth / 2U - origin) / step;
    if (max_index > (band->last_channel - band->first_channel) / band->channel_step)
    {
        max_index = (band->last_channel - band->first_channel) / band->channel_step;
    }
    if (min_index > max_index)
    {
        return -EINVAL;
    }

    index = (params->frequency_khz - origin + step / 2U) / step;
    if (index < min_index)
    {
        index = min_index;
    }
    if (index > max_index)
    {
        index = max_index;
    }
    *channel = band->first_channel + index * band->channel_step;
    *actual_khz = origin + index * step;
    return 0;
}

/****************************** AT资源管理 ******************************/

/**
 * @brief 创建独立的AT通道并等待模组就绪。
 */
static int _rf_open(linkg_thread_t *thread, at_channel_t **out)
{
    uart_config_t config;
    uint64_t      deadline;
    at_channel_t *channel;
    int           ret;
    int           last_error = -ENODEV;

    memset(&config, 0, sizeof(config));
    config.baudrate = LINKG_RF_AT_BAUDRATE;
    config.data_bits = 8;
    config.stop_bits = 1;
    config.parity = 'n';
    config.exclusive = true;
    deadline = linkg_time_elapsed_ms() + LINKG_RF_READY_MS;
    *out = NULL;
    do
    {
        if (thread != NULL && !linkg_thread_is_running(thread))
        {
            return -ECANCELED;
        }
        channel = at_channel_create(LINKG_RF_AT_DEVICE, &config);
        if (channel != NULL)
        {
            ret = at_channel_start(channel);
            last_error = ret;
            LINKG_LOG_INFO("RF-TEST: independent AT channel start device=%s error=%d", LINKG_RF_AT_DEVICE, ret);
            if (ret == 0)
            {
                ret = rg255_cmd_test(channel);
                last_error = ret;
                LINKG_LOG_INFO("RF-TEST: AT readiness error=%d", ret);
            }
            if (ret == 0)
            {
                *out = channel;
                return 0;
            }
            ret = at_channel_stop(channel);
            if (ret != 0)
            {
                *out = channel;
                return ret;
            }
            at_channel_destroy(channel);
        }
        (void)linkg_time_sleep_ms(LINKG_RF_RETRY_MS);
    }
    while (linkg_time_elapsed_ms() < deadline);
    LINKG_LOG_WARN("RF-TEST: AT readiness deadline, last_error=%d", last_error);
    return last_error;
}

/**
 * @brief 停止AT接收线程并销毁通道，失败时保留所有权。
 */
static int _rf_close(at_channel_t **channel)
{
    int ret;

    if (*channel == NULL)
    {
        return 0;
    }
    ret = at_channel_stop(*channel);
    LINKG_LOG_INFO("RF-TEST: AT channel stop error=%d", ret);
    if (ret == 0)
    {
        at_channel_destroy(*channel);
        *channel = NULL;
    }
    return ret;
}

/**
 * @brief 等待已应答的模式切换生效，仅重复查询当前模式。
 */
static int _rf_wait_mode(at_channel_t *channel, unsigned int expected)
{
    unsigned int mode;
    unsigned int attempt;
    int          ret;

    for (attempt = 0U; attempt < LINKG_RF_MODE_TRIES; attempt++)
    {
        ret = rg255_rf_get_mode(channel, &mode);
        if (ret != 0 || mode == expected)
        {
            return ret;
        }
        if (attempt + 1U < LINKG_RF_MODE_TRIES)
        {
            (void)linkg_time_sleep_ms(LINKG_RF_MODE_POLL_MS);
        }
    }
    return -EIO;
}

/**
 * @brief 重启模组，重新打开AT通道并确认Online。
 */
static int _rf_restart(at_channel_t **channel)
{
    int restart_ret;
    int ret;

    restart_ret = rg255_cmd_restart(*channel);
    LINKG_LOG_INFO("RF-TEST: AT cmd=AT+CFUN=1,1 error=%d, reopening modem", restart_ret);
    if (restart_ret != 0 && restart_ret != -ETIMEDOUT)
    {
        return restart_ret;
    }
    ret = _rf_close(channel);
    if (ret != 0)
    {
        return ret;
    }
    (void)linkg_time_sleep_ms(2000U);
    ret = _rf_open(NULL, channel);
    if (ret != 0)
    {
        return ret;
    }
    return _rf_wait_mode(*channel, 0U);
}

/**
 * @brief 退出FTM并确认Online模式；失败时尝试模组重启。
 */
static int _rf_exit(at_channel_t **channel)
{
    int ret;

    if (*channel == NULL)
    {
        return -ENODEV;
    }
    ret = rg255_rf_set_mode(*channel, false);
    if (ret != 0)
    {
        LINKG_LOG_WARN("RF-TEST: FTM disable error=%d, verifying modem state", ret);
    }
    ret = _rf_wait_mode(*channel, 0U);
    if (ret == 0)
    {
        return 0;
    }

    LINKG_LOG_WARN("RF-TEST: FTM exit not confirmed, restarting modem");
    return _rf_restart(channel);
}

/**
 * @brief 检查持久和旧版临时标记，返回1表示需恢复。
 */
static int _rf_pending(void)
{
    if (g_rf_requires_online || access(LINKG_RF_MARKER, F_OK) == 0)
    {
        return 1;
    }
    if (errno != ENOENT)
    {
        return -errno;
    }
    if (access(LINKG_RF_LEGACY_MARKER, F_OK) == 0)
    {
        return 1;
    }
    return errno == ENOENT ? 0 : -errno;
}

/**
 * @brief 同步持久目录的标记变更。
 */
static int _rf_sync_marker_dir(void)
{
    int fd;
    int ret;

    fd = open(LINKG_RF_MARKER_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
    {
        return -errno;
    }
    ret = fsync(fd) == 0 ? 0 : -errno;
    (void)close(fd);
    return ret;
}

/**
 * @brief 在改变模组状态前创建并同步恢复标记。
 */
static int _rf_mark(void)
{
    int fd;
    int ret;

    fd = open(LINKG_RF_MARKER, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
    {
        return -errno;
    }
    ret = fsync(fd) == 0 ? 0 : -errno;
    if (close(fd) != 0 && ret == 0)
    {
        ret = -errno;
    }
    return ret != 0 ? ret : _rf_sync_marker_dir();
}

/**
 * @brief 仅在已验证Online并释放串口后删除恢复标记。
 */
static int _rf_clear_marker(void)
{
    int ret = 0;

    if (unlink(LINKG_RF_LEGACY_MARKER) != 0 && errno != ENOENT)
    {
        return -errno;
    }
    if (unlink(LINKG_RF_MARKER) == 0)
    {
        ret = _rf_sync_marker_dir();
    }
    else if (errno != ENOENT)
    {
        ret = -errno;
    }
    if (ret == 0)
    {
        g_rf_requires_online = false;
    }
    return ret;
}

/**
 * @brief 启动正常Owner前恢复上一次应用异常退出遗留的FTM。
 */
static int _rf_recover_pending(void)
{
    at_channel_t *channel;
    int           ret;
    int           stop_ret;

    ret = _rf_close(&g_rf_unreleased_channel);
    if (ret != 0)
    {
        return ret;
    }
    ret = _rf_pending();
    if (ret <= 0)
    {
        return ret;
    }
    g_rf_requires_online = true;
    ret = _rf_open(NULL, &channel);
    if (ret == 0)
    {
        ret = _rf_exit(&channel);
    }
    stop_ret = _rf_close(&channel);
    if (stop_ret != 0)
    {
        g_rf_unreleased_channel = channel;
        return stop_ret;
    }
    return ret != 0 ? ret : _rf_clear_marker();
}

/**
 * @brief 发布独立恢复状态，同时保留上一次射频任务的错误。
 */
int linkg_rf_test_recover_pending(void)
{
    int ret;

    pthread_mutex_lock(&g_rf_lock);
    g_rf_status.recovery_active = true;
    pthread_mutex_unlock(&g_rf_lock);

    ret = _rf_recover_pending();

    pthread_mutex_lock(&g_rf_lock);
    g_rf_status.recovery_error = ret;
    g_rf_status.recovery_blocked = ret != 0;
    g_rf_status.recovery_active = false;
    pthread_mutex_unlock(&g_rf_lock);
    return ret;
}

/****************************** 独占任务 ******************************/

/**
 * @brief 发布任务阶段。
 */
static void _rf_state(linkg_rf_state_t state)
{
    pthread_mutex_lock(&g_rf_lock);
    g_rf_status.state = state;
    pthread_mutex_unlock(&g_rf_lock);
    LINKG_LOG_INFO("RF-TEST: phase=%d", state);
}

/**
 * @brief 在正常蜂窝Owner退出后独占AT通道，始终执行收尾。
 */
static int _rf_run(linkg_thread_t *thread, void *context, bool *released)
{
    linkg_rf_status_t status;
    at_channel_t     *channel;
    unsigned int      mode;
    linkg_rf_state_t   failure_phase;
    linkg_rf_state_t   cleanup_phase;
    uint64_t          deadline;
    int               ret;
    int               cleanup;
    int               stop_ret;
    bool              marked;
    bool              tx_attempted;

    (void)context;
    *released = true;
    marked = false;
    tx_attempted = false;
    cleanup = 0;
    failure_phase = LINKG_RF_OPENING;
    cleanup_phase = LINKG_RF_IDLE;
    linkg_rf_test_get(&status);
    ret = _rf_pending();
    if (ret != 0)
    {
        g_rf_requires_online = ret > 0;
        *released = false;
        channel = NULL;
        ret = ret < 0 ? ret : -EBUSY;
        goto done;
    }
    _rf_state(failure_phase);
    ret = _rf_open(thread, &channel);
    if (ret != 0)
    {
        goto done;
    }

    failure_phase = LINKG_RF_CHECKING;
    _rf_state(failure_phase);
    ret = rg255_rf_get_mode(channel, &mode);
    if (ret != 0)
    {
        // 当前没有遗留标记，也未发送改变模式的命令，保留查询失败结果。
        goto done;
    }
    if (mode != 0U)
    {
        // 已确认存在FTM时，记录待恢复状态后再执行恢复。
        g_rf_requires_online = true;
        stop_ret = _rf_mark();
        if (stop_ret == 0)
        {
            marked = true;
        }
        else
        {
            cleanup = stop_ret;
            cleanup_phase = LINKG_RF_CHECKING;
            *released = false;
        }
        ret = -EIO;
        goto done;
    }
    if (!linkg_thread_is_running(thread))
    {
        ret = -ECANCELED;
        goto done;
    }
    if (g_rf_last_rat != 0U && g_rf_last_rat != status.params.rat)
    {
        ret = _rf_mark();
        if (ret != 0)
        {
            *released = false;
            goto done;
        }
        marked = true;
        ret = _rf_restart(&channel);
        if (ret != 0 || !linkg_thread_is_running(thread))
        {
            ret = ret != 0 ? ret : -ECANCELED;
            goto done;
        }
    }
    else
    {
        ret = _rf_mark();
        if (ret != 0)
        {
            *released = _rf_pending() == 0;
            g_rf_requires_online = !*released;
            goto done;
        }
        marked = true;
    }
    g_rf_requires_online = true;
    g_rf_last_rat = status.params.rat;
    failure_phase = LINKG_RF_ENTERING;
    _rf_state(failure_phase);
    ret = rg255_rf_set_mode(channel, true);
    if (ret == 0)
    {
        ret = _rf_wait_mode(channel, 1U);
    }
    if (ret != 0 || !linkg_thread_is_running(thread))
    {
        if (ret == 0)
        {
            ret = -ECANCELED;
        }
        goto done;
    }
    failure_phase = LINKG_RF_STARTING_TX;
    _rf_state(failure_phase);
    tx_attempted = true;
    ret = rg255_rf_set_tx(channel, status.params.rat, status.params.band, status.params.bandwidth_code, status.channel, true);
    if (ret != 0)
    {
        goto done;
    }
    pthread_mutex_lock(&g_rf_lock);
    g_rf_status.tx_confirmed = true;
    g_rf_status.started_ms = linkg_time_elapsed_ms();
    g_rf_status.state = LINKG_RF_RUNNING;
    deadline = g_rf_status.started_ms + status.params.duration_sec * 1000ULL;
    pthread_mutex_unlock(&g_rf_lock);

    LINKG_LOG_INFO("RF-TEST: job=%" PRIu32 " TX enabled duration_s=%" PRIu32, status.job_id, status.params.duration_sec);
    while (linkg_thread_is_running(thread) && linkg_time_elapsed_ms() < deadline)
    {
        (void)linkg_time_sleep_ms(LINKG_RF_TICK_MS);
    }
    ret = linkg_thread_is_running(thread) ? 0 : -ECANCELED;
    LINKG_LOG_INFO("RF-TEST: job=%" PRIu32 " TX timer finished error=%d", status.job_id, ret);

done:
    pthread_mutex_lock(&g_rf_lock);
    if (g_rf_status.tx_confirmed)
    {
        g_rf_status.elapsed_ms = linkg_time_elapsed_ms() - g_rf_status.started_ms;
    }
    if (ret != 0 && ret != -ECANCELED)
    {
        g_rf_status.test_failed_at = failure_phase;
    }
    pthread_mutex_unlock(&g_rf_lock);
    if (marked)
    {
        if (tx_attempted)
        {
            _rf_state(LINKG_RF_STOPPING_TX);
            cleanup = rg255_rf_set_tx(channel, status.params.rat, status.params.band, status.params.bandwidth_code, status.channel, false);
            if (cleanup != 0)
            {
                cleanup_phase = LINKG_RF_STOPPING_TX;
            }
        }
        _rf_state(LINKG_RF_EXITING);
        stop_ret = _rf_exit(&channel);
        if (stop_ret != 0)
        {
            cleanup = stop_ret;
            cleanup_phase = LINKG_RF_EXITING;
            *released = false;
        }
        else
        {
            pthread_mutex_lock(&g_rf_lock);
            g_rf_status.mode_verified = true;
            pthread_mutex_unlock(&g_rf_lock);
        }
    }
    _rf_state(LINKG_RF_RELEASING);
    stop_ret = _rf_close(&channel);
    if (stop_ret != 0)
    {
        *released = false;
        cleanup = stop_ret;
        cleanup_phase = LINKG_RF_RELEASING;
        g_rf_unreleased_channel = channel;
    }
    if (marked && *released)
    {
        stop_ret = _rf_clear_marker();
        if (stop_ret != 0)
        {
            *released = false;
            if (cleanup == 0)
            {
                cleanup = stop_ret;
                cleanup_phase = LINKG_RF_RELEASING;
            }
        }
    }
    pthread_mutex_lock(&g_rf_lock);
    g_rf_status.cleanup_error = cleanup;
    g_rf_status.cleanup_failed_at = cleanup_phase;
    pthread_mutex_unlock(&g_rf_lock);
    if (*released)
    {
        g_rf_requires_online = false;
        _rf_state(LINKG_RF_RESTORING);
    }
    LINKG_LOG_INFO("RF-TEST: job=%" PRIu32 " test_error=%d cleanup_error=%d released=%d",
                   status.job_id, ret, cleanup, *released);
    return ret;
}

/**
 * @brief 独立记录测试与正常蜂窝恢复结果。
 */
static void _rf_finish(void *context, int task_error, int restore_error, bool released)
{
    (void)context;
    pthread_mutex_lock(&g_rf_lock);
    if (task_error != 0 && task_error != -ECANCELED && g_rf_status.test_failed_at == LINKG_RF_IDLE)
    {
        g_rf_status.test_failed_at = g_rf_status.state;
    }
    g_rf_status.test_error = task_error;
    g_rf_status.restore_error = restore_error;
    g_rf_status.active = false;
    g_rf_status.recovery_blocked = !released;
    g_rf_status.state = !released || restore_error != 0 || g_rf_status.cleanup_error != 0 ? LINKG_RF_RECOVERY_FAILED :
                        task_error == -ECANCELED ? LINKG_RF_CANCELLED :
                        task_error == 0 ? LINKG_RF_COMPLETED : LINKG_RF_FAILED;
    pthread_mutex_unlock(&g_rf_lock);
}

/**
 * @brief 提交一次有限时长的独占强发任务。
 */
int linkg_rf_test_start(const linkg_rf_params_t *params, uint32_t *job_id)
{
    linkg_rf_status_t previous;
    uint32_t          channel;
    uint32_t          actual;
    uint32_t          next;
    int               ret;

    if (job_id == NULL)
    {
        return -EINVAL;
    }
    ret = linkg_rf_test_resolve(params, &channel, &actual);
    if (ret != 0)
    {
        return ret;
    }
    pthread_mutex_lock(&g_rf_lock);
    if (g_rf_status.active)
    {
        pthread_mutex_unlock(&g_rf_lock);
        return -EBUSY;
    }
    previous = g_rf_status;
    next = previous.job_id + 1U;
    memset(&g_rf_status, 0, sizeof(g_rf_status));
    g_rf_status.job_id = next == 0U ? 1U : next;
    g_rf_status.params = *params;
    g_rf_status.channel = channel;
    g_rf_status.actual_khz = actual;
    g_rf_status.state = LINKG_RF_STARTING;
    g_rf_status.active = true;
    ret = linkg_network_cellular_task_start(_rf_run, _rf_finish, NULL);
    LINKG_LOG_INFO("RF-TEST: submit job=%" PRIu32 " channel=%" PRIu32 " error=%d", g_rf_status.job_id, channel, ret);
    if (ret != 0)
    {
        g_rf_status = previous;
    }
    else
    {
        *job_id = g_rf_status.job_id;
    }
    pthread_mutex_unlock(&g_rf_lock);
    return ret;
}

/**
 * @brief 仅取消指定任务。
 */
int linkg_rf_test_cancel(uint32_t job_id)
{
    int ret;

    pthread_mutex_lock(&g_rf_lock);
    ret = job_id != 0U && g_rf_status.job_id == job_id && g_rf_status.active ?
          linkg_network_cellular_task_cancel() : -ENOENT;
    LINKG_LOG_INFO("RF-TEST: cancel job=%" PRIu32 " error=%d", job_id, ret);
    pthread_mutex_unlock(&g_rf_lock);
    return ret;
}

/**
 * @brief 返回一致的任务快照及实时已发射时间。
 */
void linkg_rf_test_get(linkg_rf_status_t *status)
{
    if (status == NULL)
    {
        return;
    }
    pthread_mutex_lock(&g_rf_lock);
    *status = g_rf_status;
    if (status->state == LINKG_RF_RUNNING)
    {
        status->elapsed_ms = linkg_time_elapsed_ms() - status->started_ms;
    }
    pthread_mutex_unlock(&g_rf_lock);
}

/**
 * @brief 查询诊断是否正在占用蜂窝资源。
 */
bool linkg_rf_test_active(void)
{
    linkg_rf_status_t status;

    linkg_rf_test_get(&status);
    return status.active;
}

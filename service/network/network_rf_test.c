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

#define LINKG_RF_AT_DEVICE   "/dev/ttyUSB1"         // RG255 AT控制串口
#define LINKG_RF_AT_BAUDRATE 115200                 // AT波特率
#define LINKG_RF_READY_MS    30000U                 // AT就绪等待上限
#define LINKG_RF_RETRY_MS    500U                   // AT重试周期
#define LINKG_RF_TICK_MS     100U                   // 强发取消检查周期
#define LINKG_RF_MARKER      "/tmp/linkg-rf-pending" // 进程异常退出恢复标记
#define LINKG_RF_BW_BIT(code) (1U << (code))          // FTM带宽编码位图

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
            if (ret == 0)
            {
                ret = rg255_cmd_test(channel);
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
    return -ETIMEDOUT;
}

/**
 * @brief 退出FTM并确认Online模式；失败时尝试模组重启。
 */
static int _rf_exit(at_channel_t **channel)
{
    unsigned int mode;
    int          ret;

    if (*channel == NULL)
    {
        return -ENODEV;
    }
    (void)rg255_rf_set_mode(*channel, false);
    ret = rg255_rf_get_mode(*channel, &mode);
    if (ret == 0 && mode == 0U)
    {
        return 0;
    }

    LINKG_LOG_WARN("RF-TEST: FTM exit not confirmed, restarting modem");
    (void)rg255_cmd_restart(*channel);
    ret = at_channel_stop(*channel);
    if (ret != 0)
    {
        return ret;
    }
    at_channel_destroy(*channel);
    *channel = NULL;
    (void)linkg_time_sleep_ms(2000U);
    ret = _rf_open(NULL, channel);
    if (ret != 0)
    {
        return ret;
    }
    ret = rg255_rf_get_mode(*channel, &mode);
    return ret != 0 ? ret : mode == 0U ? 0 : -EIO;
}

/**
 * @brief 在进入FTM前创建进程级恢复标记。
 */
static int _rf_mark(void)
{
    int fd = open(LINKG_RF_MARKER, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);

    if (fd < 0)
    {
        return -errno;
    }
    if (close(fd) != 0)
    {
        return -errno;
    }
    return 0;
}

/**
 * @brief 启动正常Owner前恢复上一次应用异常退出遗留的FTM。
 */
int linkg_rf_test_recover_pending(void)
{
    at_channel_t *channel;
    int           ret;
    int           stop_ret;

    if (access(LINKG_RF_MARKER, F_OK) != 0)
    {
        return errno == ENOENT ? 0 : -errno;
    }
    ret = _rf_open(NULL, &channel);
    if (ret == 0)
    {
        ret = _rf_exit(&channel);
    }
    if (channel != NULL)
    {
        stop_ret = at_channel_stop(channel);
        if (stop_ret != 0)
        {
            return stop_ret;
        }
        at_channel_destroy(channel);
    }
    if (ret == 0 && unlink(LINKG_RF_MARKER) != 0)
    {
        ret = -errno;
    }
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
}

/**
 * @brief 在正常蜂窝Owner退出后独占AT通道，始终执行收尾。
 */
static int _rf_run(linkg_thread_t *thread, void *context, bool *released)
{
    linkg_rf_status_t status;
    at_channel_t     *channel;
    unsigned int      mode;
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
    linkg_rf_test_get(&status);
    ret = _rf_open(thread, &channel);
    if (ret != 0)
    {
        goto done;
    }

    ret = rg255_rf_get_mode(channel, &mode);
    if (ret != 0 || mode != 0U)
    {
        // 不继承未知FTM状态。
        cleanup = _rf_exit(&channel);
        if (cleanup != 0)
        {
            ret = cleanup;
            *released = false;
            (void)_rf_mark();
            goto done;
        }
        if (ret != 0)
        {
            goto done;
        }
    }
    if (!linkg_thread_is_running(thread))
    {
        ret = -ECANCELED;
        goto done;
    }
    if (g_rf_last_rat != 0U && g_rf_last_rat != status.params.rat)
    {
        (void)rg255_cmd_restart(channel);
        stop_ret = at_channel_stop(channel);
        if (stop_ret != 0)
        {
            ret = stop_ret;
            *released = false;
            goto done;
        }
        at_channel_destroy(channel);
        channel = NULL;
        (void)linkg_time_sleep_ms(2000U);
        ret = _rf_open(thread, &channel);
        if (ret != 0)
        {
            goto done;
        }
        ret = rg255_rf_get_mode(channel, &mode);
        if (ret != 0 || mode != 0U)
        {
            ret = ret != 0 ? ret : -EIO;
            *released = false;
            (void)_rf_mark();
            goto done;
        }
    }
    ret = _rf_mark();
    if (ret != 0)
    {
        // 已有标记意味着不能确认之前的测试资源安全释放。
        if (ret == -EEXIST)
        {
            *released = false;
        }
        goto done;
    }
    marked = true;
    g_rf_last_rat = status.params.rat;
    _rf_state(LINKG_RF_ENTERING);
    ret = rg255_rf_set_mode(channel, true);
    if (ret == 0)
    {
        ret = rg255_rf_get_mode(channel, &mode);
        if (ret == 0 && mode != 1U)
        {
            ret = -EIO;
        }
    }
    if (ret != 0 || !linkg_thread_is_running(thread))
    {
        if (ret == 0)
        {
            ret = -ECANCELED;
        }
        goto done;
    }
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

    while (linkg_thread_is_running(thread) && linkg_time_elapsed_ms() < deadline)
    {
        (void)linkg_time_sleep_ms(LINKG_RF_TICK_MS);
    }
    ret = linkg_thread_is_running(thread) ? 0 : -ECANCELED;

done:
    pthread_mutex_lock(&g_rf_lock);
    if (g_rf_status.tx_confirmed)
    {
        g_rf_status.elapsed_ms = linkg_time_elapsed_ms() - g_rf_status.started_ms;
    }
    g_rf_status.state = LINKG_RF_RESTORING;
    pthread_mutex_unlock(&g_rf_lock);
    if (marked)
    {
        if (tx_attempted)
        {
            cleanup = rg255_rf_set_tx(channel, status.params.rat, status.params.band, status.params.bandwidth_code, status.channel, false);
        }
        stop_ret = _rf_exit(&channel);
        if (stop_ret != 0)
        {
            cleanup = stop_ret;
            *released = false;
        }
        else
        {
            pthread_mutex_lock(&g_rf_lock);
            g_rf_status.mode_verified = true;
            pthread_mutex_unlock(&g_rf_lock);
        }
    }
    if (channel != NULL)
    {
        stop_ret = at_channel_stop(channel);
        if (stop_ret == 0)
        {
            at_channel_destroy(channel);
        }
        else
        {
            *released = false;
            cleanup = stop_ret;
        }
    }
    if (marked && *released && unlink(LINKG_RF_MARKER) != 0)
    {
        *released = false;
        if (cleanup == 0)
        {
            cleanup = -errno;
        }
    }
    pthread_mutex_lock(&g_rf_lock);
    g_rf_status.cleanup_error = cleanup;
    pthread_mutex_unlock(&g_rf_lock);
    return ret != 0 ? ret : cleanup;
}

/**
 * @brief 独立记录测试与正常蜂窝恢复结果。
 */
static void _rf_finish(void *context, int task_error, int restore_error)
{
    (void)context;
    pthread_mutex_lock(&g_rf_lock);
    g_rf_status.test_error = task_error;
    g_rf_status.restore_error = restore_error;
    g_rf_status.active = false;
    g_rf_status.state = restore_error != 0 || g_rf_status.cleanup_error != 0 ? LINKG_RF_FAILED :
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

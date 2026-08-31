/**
 * @file wifi_flowctrl.c
 * @brief LinkG Wi-Fi发送流控控制器实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-28
 */

#include "wifi_flowctrl.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "linkg_log.h"
#include "linkg_time.h"

#include "wal_tx_flowctrl_ioctl.h"

/****************************** 控制参数 ******************************/

#define LINKG_WIFI_FLOWCTRL_DEVICE_PATH          "/dev/hi1105_tx_flowctrl" // HI1105发送流控状态设备

#ifndef LINKG_WIFI_FLOWCTRL_BATCH_LIMIT_MIN
#define LINKG_WIFI_FLOWCTRL_BATCH_LIMIT_MIN       2U                      // 普通流量最小单次准入数量
#endif

#ifndef LINKG_WIFI_FLOWCTRL_BATCH_LIMIT_MAX
#define LINKG_WIFI_FLOWCTRL_BATCH_LIMIT_MAX       14U                     // 普通流量最大单次准入数量
#endif

#ifndef LINKG_WIFI_FLOWCTRL_RECOVERY_INTERVAL_US
#define LINKG_WIFI_FLOWCTRL_RECOVERY_INTERVAL_US  100000ULL              // 批次上限恢复间隔，单位微秒
#endif

#ifndef LINKG_WIFI_FLOWCTRL_OFF_HOLD_US
#define LINKG_WIFI_FLOWCTRL_OFF_HOLD_US           1000ULL               // 新增FLOWCTRL_OFF后的普通流量静默时间
#endif

#ifndef LINKG_WIFI_FLOWCTRL_RECOVERY_QUEUE_MAX
#define LINKG_WIFI_FLOWCTRL_RECOVERY_QUEUE_MAX    16U                     // 允许恢复批次上限的BE/VI队列最大长度
#endif

/****************************** 内部类型 ******************************/

struct linkg_wifi_flowctrl
{
    pthread_mutex_t lock;               // 控制状态和设备描述符保护锁
    int             fd;                 // HI1105发送流控状态设备描述符
    uint32_t        normal_batch_limit; // 当前VIDEO/DATA单次最大准入数量
    uint32_t        last_off_count;     // 最近已经处理的FLOWCTRL_OFF累计计数
    uint64_t        last_recovery_us;   // 最近一次退避或恢复时间
    uint64_t        off_hold_until_us;  // FLOWCTRL_OFF后普通流量静默截止时间
    bool            baseline_valid;     // FLOWCTRL_OFF累计计数基线是否有效
    bool            started;            // 控制器是否已经启动
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 读取HI1105当前发送流控状态。
 *
 * @note 调用方必须持有flowctrl状态锁。
 */
static int _linkg_wifi_flowctrl_read_locked(linkg_wifi_flowctrl_t *flowctrl, wal_tx_flowctrl_status_stru *status)
{
    ssize_t length;

    if (flowctrl == NULL || status == NULL)
    {
        return -EINVAL;
    }

    if (flowctrl->fd < 0)
    {
        return -ENODEV;
    }

    memset(status, 0, sizeof(*status));

    do
    {
        length = read(flowctrl->fd, status, sizeof(*status));
    }
    while (length < 0 && errno == EINTR);

    if (length < 0)
    {
        return -errno;
    }

    if ((size_t)length != sizeof(*status))
    {
        return -EIO;
    }

    if (status->version != WAL_TX_FLOWCTRL_ABI_VERSION)
    {
        return -EPROTO;
    }

    if (status->struct_size != (uint16_t)sizeof(*status))
    {
        return -EPROTO;
    }

    return 0;
}

/**
 * @brief 重置流控运行状态。
 *
 * @note 调用方必须持有flowctrl状态锁，或者控制器尚未对其他线程可见。
 */
static void _linkg_wifi_flowctrl_reset_locked(linkg_wifi_flowctrl_t *flowctrl)
{
    flowctrl->normal_batch_limit = LINKG_WIFI_FLOWCTRL_BATCH_LIMIT_MAX;
    flowctrl->last_off_count     = 0U;
    flowctrl->last_recovery_us   = 0U;
    flowctrl->off_hold_until_us  = 0U;
    flowctrl->baseline_valid     = false;
}

/**
 * @brief 根据FLOWCTRL_OFF累计计数更新普通流量批次退避状态。
 *
 * @note 调用方必须持有flowctrl状态锁。
 */
static bool _linkg_wifi_flowctrl_handle_off_locked(linkg_wifi_flowctrl_t *flowctrl, uint32_t current_off_count, uint64_t now_us)
{
    uint32_t old_limit;

    if (!flowctrl->baseline_valid)
    {
        flowctrl->last_off_count = current_off_count;
        flowctrl->baseline_valid = true;
        return false;
    }

    if (current_off_count < flowctrl->last_off_count)
    {
        flowctrl->last_off_count   = current_off_count;
        flowctrl->last_recovery_us = now_us;
        return false;
    }

    if (current_off_count == flowctrl->last_off_count)
    {
        return false;
    }

    flowctrl->last_off_count = current_off_count;
    old_limit                = flowctrl->normal_batch_limit;

    if (flowctrl->normal_batch_limit > LINKG_WIFI_FLOWCTRL_BATCH_LIMIT_MIN)
    {
        flowctrl->normal_batch_limit = (flowctrl->normal_batch_limit + 1U) / 2U;

        if (flowctrl->normal_batch_limit < LINKG_WIFI_FLOWCTRL_BATCH_LIMIT_MIN)
        {
            flowctrl->normal_batch_limit = LINKG_WIFI_FLOWCTRL_BATCH_LIMIT_MIN;
        }
    }

    flowctrl->last_recovery_us = now_us;

    LINKG_LOG_DEBUG("WIFI: flowctrl backoff, off=%u, batch=%u->%u",
                    current_off_count,
                    old_limit,
                    flowctrl->normal_batch_limit);

    return true;
}

/**
 * @brief 在驱动队列稳定后缓慢恢复普通流量批次上限。
 *
 * @note 调用方必须持有flowctrl状态锁。
 */
static void _linkg_wifi_flowctrl_recover_locked(linkg_wifi_flowctrl_t *flowctrl, uint64_t now_us)
{
    if (flowctrl->normal_batch_limit >= LINKG_WIFI_FLOWCTRL_BATCH_LIMIT_MAX)
    {
        return;
    }

    if (flowctrl->last_recovery_us == 0U)
    {
        flowctrl->last_recovery_us = now_us;
        return;
    }

    if (now_us < flowctrl->last_recovery_us)
    {
        flowctrl->last_recovery_us = now_us;
        return;
    }

    if (now_us - flowctrl->last_recovery_us < LINKG_WIFI_FLOWCTRL_RECOVERY_INTERVAL_US)
    {
        return;
    }

    flowctrl->normal_batch_limit++;
    flowctrl->last_recovery_us = now_us;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建Wi-Fi发送流控控制器。
 */
linkg_wifi_flowctrl_t *linkg_wifi_flowctrl_create(void)
{
    linkg_wifi_flowctrl_t *flowctrl;
    int                    ret;

    flowctrl = calloc(1, sizeof(*flowctrl));
    if (flowctrl == NULL)
    {
        return NULL;
    }

    flowctrl->fd = -1;

    _linkg_wifi_flowctrl_reset_locked(flowctrl);

    ret = pthread_mutex_init(&flowctrl->lock, NULL);
    if (ret != 0)
    {
        free(flowctrl);
        return NULL;
    }

    return flowctrl;
}

/**
 * @brief 销毁Wi-Fi发送流控控制器。
 *
 * @note 调用前不能再有其他线程访问flowctrl。
 */
void linkg_wifi_flowctrl_destroy(linkg_wifi_flowctrl_t *flowctrl)
{
    if (flowctrl == NULL)
    {
        return;
    }

    linkg_wifi_flowctrl_stop(flowctrl);

    pthread_mutex_destroy(&flowctrl->lock);

    free(flowctrl);
}

/**
 * @brief 启动Wi-Fi发送流控控制器。
 */
int linkg_wifi_flowctrl_start(linkg_wifi_flowctrl_t *flowctrl)
{
    int descriptor;
    int ret;

    if (flowctrl == NULL)
    {
        return -EINVAL;
    }

    descriptor = open(LINKG_WIFI_FLOWCTRL_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
    {
        return -errno;
    }

    ret = pthread_mutex_lock(&flowctrl->lock);
    if (ret != 0)
    {
        close(descriptor);
        return -ret;
    }

    if (flowctrl->started)
    {
        pthread_mutex_unlock(&flowctrl->lock);
        close(descriptor);
        return -EALREADY;
    }

    flowctrl->fd      = descriptor;
    flowctrl->started = true;

    _linkg_wifi_flowctrl_reset_locked(flowctrl);

    pthread_mutex_unlock(&flowctrl->lock);

    return 0;
}

/**
 * @brief 停止Wi-Fi发送流控控制器。
 */
void linkg_wifi_flowctrl_stop(linkg_wifi_flowctrl_t *flowctrl)
{
    int descriptor;
    int ret;

    if (flowctrl == NULL)
    {
        return;
    }

    ret = pthread_mutex_lock(&flowctrl->lock);
    if (ret != 0)
    {
        return;
    }

    descriptor = flowctrl->fd;

    flowctrl->fd      = -1;
    flowctrl->started = false;

    _linkg_wifi_flowctrl_reset_locked(flowctrl);

    pthread_mutex_unlock(&flowctrl->lock);

    if (descriptor >= 0)
    {
        close(descriptor);
    }
}

/****************************** 发送准入 ******************************/

/**
 * @brief 获取当前普通流量允许进入Wi-Fi底层的最大包数。
 *
 * @note 本接口仅用于VIDEO和DATA，REALTIME流量不得经过本流控控制器。
 */
int linkg_wifi_flowctrl_admit(linkg_wifi_flowctrl_t *flowctrl, uint32_t requested_count, uint32_t *allowed_count, linkg_wifi_flowctrl_sample_t *sample)
{
    wal_tx_flowctrl_status_stru status;
    uint64_t                    read_start_us;
    uint64_t                    now_us;
    uint32_t                    allowed;
    bool                        backed_off;
    int                         ret;

    if (flowctrl == NULL || allowed_count == NULL || sample == NULL || requested_count == 0U)
    {
        return -EINVAL;
    }

    *allowed_count = 0U;

    memset(sample, 0, sizeof(*sample));

    sample->requested_count = requested_count;

    ret = pthread_mutex_lock(&flowctrl->lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (!flowctrl->started || flowctrl->fd < 0)
    {
        pthread_mutex_unlock(&flowctrl->lock);
        return -ENODEV;
    }

    read_start_us = linkg_time_monotonic_us();

    ret = _linkg_wifi_flowctrl_read_locked(flowctrl, &status);

    now_us          = linkg_time_monotonic_us();
    sample->read_us = now_us >= read_start_us ? now_us - read_start_us : 0U;

    if (ret != 0)
    {
        pthread_mutex_unlock(&flowctrl->lock);
        return ret;
    }

    sample->status_valid       = true;
    sample->be_queue_length    = status.be_queue_length;
    sample->vi_queue_length    = status.vi_queue_length;
    sample->vo_queue_length    = status.vo_queue_length;
    sample->flowctrl_off_count = status.flowctrl_off_count;
    sample->tx_allowed         = status.tx_allowed;

    backed_off = _linkg_wifi_flowctrl_handle_off_locked(flowctrl, status.flowctrl_off_count, now_us);

    if (backed_off)
    {
        flowctrl->off_hold_until_us = now_us + LINKG_WIFI_FLOWCTRL_OFF_HOLD_US;
    }

    sample->backed_off        = backed_off;
    sample->waterline_blocked = false;

    /**
     * 新观察到FLOWCTRL_OFF后短暂静默普通流量。
     * REALTIME流量不经过本控制器；驱动明确禁止发送时继续保持硬阻塞。
     */
    if (status.tx_allowed == 0U || now_us < flowctrl->off_hold_until_us)
    {
        flowctrl->last_recovery_us = now_us;

        sample->normal_batch_limit = flowctrl->normal_batch_limit;

        *allowed_count = 0U;

        pthread_mutex_unlock(&flowctrl->lock);

        return 0;
    }

    /**
     * HCC队列水位只作为批次恢复条件。
     * BE和VI队列持续回落后才逐步增加普通流量批次，
     * 避免因瞬时亚毫秒队列峰值直接阻塞整个发送批次。
     */
    if (status.be_queue_length <= LINKG_WIFI_FLOWCTRL_RECOVERY_QUEUE_MAX &&
        status.vi_queue_length <= LINKG_WIFI_FLOWCTRL_RECOVERY_QUEUE_MAX)
    {
        _linkg_wifi_flowctrl_recover_locked(flowctrl, now_us);
    }

    allowed = requested_count;

    if (allowed > flowctrl->normal_batch_limit)
    {
        allowed = flowctrl->normal_batch_limit;
    }

    *allowed_count = allowed;

    sample->normal_batch_limit = flowctrl->normal_batch_limit;
    sample->allowed_count      = allowed;

    pthread_mutex_unlock(&flowctrl->lock);

    return 0;
}

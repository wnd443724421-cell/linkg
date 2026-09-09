/**
 * @file wifi_flowctrl.c
 * @brief LinkG Wi-Fi发送流控控制器实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-09-09
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

#include "linkg_time.h"

#include "wal_tx_flowctrl_ioctl.h"

/****************************** 模块常量 ******************************/

#define LINKG_WIFI_FLOWCTRL_DEVICE_PATH "/dev/hi1105_tx_flowctrl"

/****************************** 内部类型 ******************************/

struct linkg_wifi_flowctrl
{
    pthread_mutex_t lock;           // 生命周期和设备访问保护锁
    int             fd;             // HI1105发送流控状态设备描述符
    uint32_t        last_off_count; // 上次观察到的FLOWCTRL_OFF累计次数
    bool            baseline_valid; // OFF计数基线是否有效
    bool            started;        // 控制器是否已经启动
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 读取HI1105当前发送流控状态。
 *
 * 调用方必须持有flowctrl状态锁。
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

    if (status->version != WAL_TX_FLOWCTRL_ABI_VERSION ||
        status->struct_size != (uint16_t)sizeof(*status))
    {
        return -EPROTO;
    }

    return 0;
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

    pthread_mutex_unlock(&flowctrl->lock);

    if (descriptor >= 0)
    {
        close(descriptor);
    }
}

/****************************** 发送准入 ******************************/

/**
 * @brief 检查Wi-Fi当前是否允许提交新的发送批次。
 */
int linkg_wifi_flowctrl_check(linkg_wifi_flowctrl_t *flowctrl, bool *submit_allowed, linkg_wifi_flowctrl_sample_t *sample)
{
    wal_tx_flowctrl_status_stru status;
    uint64_t                    read_start_us;
    uint64_t                    read_end_us;
    bool                        new_off;
    int                         ret;

    if (flowctrl == NULL || submit_allowed == NULL || sample == NULL)
    {
        return -EINVAL;
    }

    *submit_allowed = false;

    memset(sample, 0, sizeof(*sample));

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

    read_end_us = linkg_time_monotonic_us();

    sample->read_us =
        read_end_us >= read_start_us ?
        read_end_us - read_start_us : 0U;

    if (ret != 0)
    {
        pthread_mutex_unlock(&flowctrl->lock);
        return ret;
    }

    new_off = false;

    if (!flowctrl->baseline_valid)
    {
        flowctrl->last_off_count = status.flowctrl_off_count;
        flowctrl->baseline_valid = true;
    }
    else if (status.flowctrl_off_count > flowctrl->last_off_count)
    {
        flowctrl->last_off_count = status.flowctrl_off_count;
        new_off = true;
    }
    else if (status.flowctrl_off_count < flowctrl->last_off_count)
    {
        flowctrl->last_off_count = status.flowctrl_off_count;
    }

    sample->be_queue_length    = status.be_queue_length;
    sample->vi_queue_length    = status.vi_queue_length;
    sample->vo_queue_length    = status.vo_queue_length;
    sample->flowctrl_off_count = status.flowctrl_off_count;
    sample->driver_tx_allowed  = status.tx_allowed != 0U;
    sample->new_off_detected   = new_off;
    sample->status_valid       = true;

    sample->submit_allowed = sample->driver_tx_allowed && !sample->new_off_detected;

    *submit_allowed = sample->submit_allowed;

    pthread_mutex_unlock(&flowctrl->lock);

    return 0;
}

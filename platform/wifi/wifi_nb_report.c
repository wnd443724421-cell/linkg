/**
 * @file wifi_nb_report.c
 * @brief LinkG HI1105窄带附加状态实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-26
 */

#include "wifi_nb_report.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "wifi_nb_report_netlink.h"
#include "wifi_platform_internal.h"

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t lock;        // 模块操作互斥锁
    int             netlink_fd;  // HI1105窄带Netlink套接字
    bool            initialized; // 模块是否已经初始化
    bool            started;     // 模块是否已经启动
} wifi_nb_report_context_t;

/****************************** 全局上下文 ******************************/

static wifi_nb_report_context_t g_wifi_nb_report =
{
    .lock        = PTHREAD_MUTEX_INITIALIZER, // 模块操作互斥锁
    .netlink_fd  = -1,                        // 尚未创建Netlink套接字
    .initialized = false,                     // 尚未初始化
    .started     = false                      // 尚未启动
};

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化HI1105窄带附加状态模块。
 */
int wifi_nb_report_init(void)
{
    pthread_mutex_lock(&g_wifi_nb_report.lock);

    if (g_wifi_nb_report.initialized)
    {
        pthread_mutex_unlock(&g_wifi_nb_report.lock);
        return -EALREADY;
    }

    g_wifi_nb_report.netlink_fd  = -1;
    g_wifi_nb_report.started     = false;
    g_wifi_nb_report.initialized = true;

    pthread_mutex_unlock(&g_wifi_nb_report.lock);

    WIFI_DRIVER_DEBUG("narrow report module initialized");

    return 0;
}

/**
 * @brief 启动HI1105窄带附加状态模块。
 *
 * @note 必须在驱动加载且窄带无线参数生效后调用。
 */
int wifi_nb_report_start(void)
{
    int ret;

    pthread_mutex_lock(&g_wifi_nb_report.lock);

    if (!g_wifi_nb_report.initialized)
    {
        pthread_mutex_unlock(&g_wifi_nb_report.lock);
        return -ENODEV;
    }

    if (g_wifi_nb_report.started)
    {
        pthread_mutex_unlock(&g_wifi_nb_report.lock);
        return -EALREADY;
    }

    ret = wifi_nb_report_netlink_open();
    if (ret < 0)
    {
        pthread_mutex_unlock(&g_wifi_nb_report.lock);
        WIFI_DRIVER_ERROR("open narrow report netlink failed, error=%d", ret);
        return ret;
    }

    g_wifi_nb_report.netlink_fd = ret;
    g_wifi_nb_report.started    = true;

    pthread_mutex_unlock(&g_wifi_nb_report.lock);

    WIFI_DRIVER_DEBUG("narrow report module started");

    return 0;
}

/**
 * @brief 停止HI1105窄带附加状态模块。
 */
int wifi_nb_report_stop(void)
{
    int saved_errno;
    int ret;

    pthread_mutex_lock(&g_wifi_nb_report.lock);

    if (!g_wifi_nb_report.initialized || !g_wifi_nb_report.started)
    {
        pthread_mutex_unlock(&g_wifi_nb_report.lock);
        return 0;
    }

    ret = 0;

    if (g_wifi_nb_report.netlink_fd >= 0)
    {
        if (close(g_wifi_nb_report.netlink_fd) != 0)
        {
            saved_errno = errno;
            ret         = -saved_errno;
        }

        g_wifi_nb_report.netlink_fd = -1;
    }

    g_wifi_nb_report.started = false;

    pthread_mutex_unlock(&g_wifi_nb_report.lock);

    if (ret != 0)
    {
        WIFI_DRIVER_WARN("close narrow report netlink failed, error=%d", ret);
        return ret;
    }

    WIFI_DRIVER_DEBUG("narrow report module stopped");

    return 0;
}

/**
 * @brief 反初始化HI1105窄带附加状态模块。
 */
void wifi_nb_report_deinit(void)
{
    int ret;

    ret = wifi_nb_report_stop();
    if (ret != 0)
    {
        return;
    }

    pthread_mutex_lock(&g_wifi_nb_report.lock);

    if (!g_wifi_nb_report.initialized)
    {
        pthread_mutex_unlock(&g_wifi_nb_report.lock);
        return;
    }

    g_wifi_nb_report.initialized = false;

    pthread_mutex_unlock(&g_wifi_nb_report.lock);

    WIFI_DRIVER_DEBUG("narrow report module deinitialized");
}

/****************************** 状态读取 ******************************/

/**
 * @brief 同步获取当前窄带速率档位和HI1105芯片温度。
 *
 * @note 查询期间持有模块操作锁，保证stop不会并发关闭Netlink套接字；
 *       驱动响应等待时间由Netlink层限制在固定上限内。
 */
int wifi_nb_report_get_status(wifi_nb_report_status_t *status)
{
    int ret;

    if (status == NULL)
    {
        return -EINVAL;
    }

    memset(status, 0, sizeof(*status));

    pthread_mutex_lock(&g_wifi_nb_report.lock);

    if (!g_wifi_nb_report.initialized || !g_wifi_nb_report.started)
    {
        pthread_mutex_unlock(&g_wifi_nb_report.lock);
        return -ENODEV;
    }

    ret = wifi_nb_report_netlink_query(g_wifi_nb_report.netlink_fd, status);

    pthread_mutex_unlock(&g_wifi_nb_report.lock);

    if (ret != 0)
    {
        memset(status, 0, sizeof(*status));
    }

    return ret;
}

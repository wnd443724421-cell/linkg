/**
 * @file linkg_link_manager.c
 * @brief LinkG本地链路管理实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-09-10
 */

#include "linkg_link_manager.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_cellular_link.h"
#include "linkg_config.h"
#include "linkg_log.h"
#include "linkg_system_resources.h"
#include "linkg_wifi_link.h"

/****************************** 模块常量 ******************************/

#define LINKG_LINK_MANAGER_WIFI_REALTIME_SNDBUF_SIZE (32U   * 1024U) // 实时UDP发送缓冲请求值，32KiB
#define LINKG_LINK_MANAGER_WIFI_VIDEO_SNDBUF_SIZE    (64U   * 1024U) // 视频UDP发送缓冲请求值，64KiB
#define LINKG_LINK_MANAGER_WIFI_DATA_SNDBUF_SIZE     (128U  * 1024U) // 普通数据UDP发送缓冲请求值，128KiB
#define LINKG_LINK_MANAGER_WIFI_REALTIME_RCVBUF_SIZE (256U  * 1024U) // 实时UDP接收缓冲请求值，256KiB
#define LINKG_LINK_MANAGER_WIFI_VIDEO_RCVBUF_SIZE    (512U  * 1024U) // 视频UDP接收缓冲请求值，512KiB
#define LINKG_LINK_MANAGER_WIFI_DATA_RCVBUF_SIZE     (1024U * 1024U) // 普通数据UDP接收缓冲请求值，1MiB

#define LINKG_LINK_MANAGER_CELLULAR_NAME             "cellular"      // 蜂窝业务链路名称
#define LINKG_LINK_MANAGER_WIFI_NAME                 "wifi"          // Wi-Fi业务链路名称
#define LINKG_LINK_MANAGER_MAX_LINKS                 2U              // 最大业务链路数量，Wi-Fi和蜂窝各一条

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_link_t                    *links[LINKG_LINK_MANAGER_MAX_LINKS]; // 本地业务链路对象
    linkg_link_receive_batch_func_t  receive;                             // 上层统一接收处理函数
    void                            *receive_user_data;                   // 接收处理私有数据
    uint32_t                         count;                               // 当前已创建业务链路数量
    bool                             initialized;                         // 模块是否已经初始化
} linkg_link_manager_context_t;

typedef struct
{
    linkg_link_t *links[LINKG_LINK_MANAGER_MAX_LINKS]; // 初始化阶段创建的业务链路
    uint32_t      count;                               // 已创建业务链路数量
} linkg_link_manager_build_t;

/****************************** 全局上下文 ******************************/

/**
 * @brief Link Manager全局运行上下文。
 *
 * @note 生命周期由网络管理线程串行调用。初始化阶段根据Path配置一次性创建
 *       全部启用的业务Link，运行期间不动态增加、删除或单独启停Link。
 */
static linkg_link_manager_context_t g_link_manager;

/****************************** 内部辅助 ******************************/

/**
 * @brief 根据运行实例标识查找业务链路。
 */
static linkg_link_t *_linkg_link_manager_find(uint32_t link_id)
{
    uint32_t index;

    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return NULL;
    }

    for (index = 0U; index < g_link_manager.count; index++)
    {
        if (g_link_manager.links[index] != NULL &&
            linkg_link_get_id(g_link_manager.links[index]) == link_id)
        {
            return g_link_manager.links[index];
        }
    }

    return NULL;
}

/**
 * @brief 判断全部业务链路是否已经停止。
 */
static bool _linkg_link_manager_all_stopped(void)
{
    uint32_t index;

    for (index = 0U; index < g_link_manager.count; index++)
    {
        if (g_link_manager.links[index] == NULL)
        {
            return false;
        }

        if (linkg_link_get_state(g_link_manager.links[index]) != LINKG_LINK_STATE_STOPPED)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 将具体链路接收数据转发给上层统一处理函数。
 *
 * @note items中的Packet仅在当前回调期间借用，基础引用由Link RX在回调返回后统一释放。
 */
static void _linkg_link_manager_receive(linkg_link_t *link, linkg_link_rx_item_t *items, uint32_t count, void *user_data)
{
    (void)user_data;

    if (link == NULL || items == NULL || count == 0U)
    {
        return;
    }

    if (g_link_manager.receive == NULL)
    {
        LINKG_LOG_ERROR("link receive handler unavailable, link=%s", linkg_link_get_name(link));
        return;
    }

    g_link_manager.receive(link, items, count, g_link_manager.receive_user_data);
}

/****************************** 链路构建 ******************************/

/**
 * @brief 将业务链路加入初始化构建结果并分配运行实例标识。
 */
static int _linkg_link_manager_build_append(linkg_link_manager_build_t *build, linkg_link_t *link)
{
    uint32_t index;

    if (build == NULL || link == NULL)
    {
        return -EINVAL;
    }

    if (link->id != LINKG_LINK_ID_INVALID)
    {
        return -EALREADY;
    }

    if (build->count >= LINKG_LINK_MANAGER_MAX_LINKS)
    {
        return -ENOSPC;
    }

    for (index = 0U; index < build->count; index++)
    {
        if (build->links[index] != NULL &&
            linkg_link_get_access(build->links[index]) == linkg_link_get_access(link))
        {
            return -EEXIST;
        }
    }

    link->id = build->count + 1U;

    build->links[build->count] = link;
    build->count++;

    return 0;
}

/**
 * @brief 销毁初始化阶段已经创建的全部业务链路。
 */
static void _linkg_link_manager_build_destroy(linkg_link_manager_build_t *build)
{
    linkg_link_t *link;

    if (build == NULL)
    {
        return;
    }

    while (build->count > 0U)
    {
        build->count--;

        link = build->links[build->count];
        build->links[build->count] = NULL;

        if (link != NULL)
        {
            linkg_link_destroy(link);
        }
    }
}

/**
 * @brief 初始化具体业务链路共用的基类配置。
 */
static void _linkg_link_manager_build_config(linkg_link_config_t *link_config, const linkg_link_manager_config_t *manager_config, const char *name, linkg_link_access_t access)
{
    if (link_config == NULL || manager_config == NULL)
    {
        return;
    }

    memset(link_config, 0, sizeof(*link_config));

    link_config->name              = name;
    link_config->access            = access;
    link_config->tx_batch_size     = LINKG_LINK_TX_BATCH_SIZE_DEFAULT;
    link_config->rx_batch_size     = LINKG_LINK_RX_BATCH_SIZE_DEFAULT;
    link_config->packet_pool       = manager_config->packet_pool;
    link_config->receive           = _linkg_link_manager_receive;
    link_config->receive_user_data = NULL;
}

/**
 * @brief 创建Wi-Fi业务链路。
 *
 * @note 本函数只创建LinkG业务数据使用的Wi-Fi Link，不负责Wi-Fi接入模块生命周期。
 */
static int _linkg_link_manager_create_wifi(const linkg_config_t *config, const linkg_link_manager_config_t *manager_config, linkg_link_manager_build_t *build)
{
    linkg_wifi_link_config_t wifi_config;
    linkg_link_config_t      link_config;
    linkg_link_t            *link;
    int                      ret;

    if (config == NULL || manager_config == NULL || build == NULL)
    {
        return -EINVAL;
    }

    if (!config->paths.wifi.enabled)
    {
        return -EINVAL;
    }

    if (!config->links.wifi.enabled)
    {
        LINKG_LOG_ERROR("Wi-Fi path enabled but Wi-Fi access module disabled");
        return -EINVAL;
    }

    _linkg_link_manager_build_config(&link_config, manager_config, LINKG_LINK_MANAGER_WIFI_NAME, LINKG_LINK_ACCESS_WIFI);

    memset(&wifi_config, 0, sizeof(wifi_config));

    wifi_config.data_port                    = LINKG_RESOURCE_UDP_PORT_WIFI_DATA;
    wifi_config.realtime_port                = LINKG_RESOURCE_UDP_PORT_WIFI_REALTIME;
    wifi_config.video_port                   = LINKG_RESOURCE_UDP_PORT_WIFI_VIDEO;
    wifi_config.data_send_buffer_size        = LINKG_LINK_MANAGER_WIFI_DATA_SNDBUF_SIZE;
    wifi_config.realtime_send_buffer_size    = LINKG_LINK_MANAGER_WIFI_REALTIME_SNDBUF_SIZE;
    wifi_config.video_send_buffer_size       = LINKG_LINK_MANAGER_WIFI_VIDEO_SNDBUF_SIZE;
    wifi_config.data_receive_buffer_size     = LINKG_LINK_MANAGER_WIFI_DATA_RCVBUF_SIZE;
    wifi_config.realtime_receive_buffer_size = LINKG_LINK_MANAGER_WIFI_REALTIME_RCVBUF_SIZE;
    wifi_config.video_receive_buffer_size    = LINKG_LINK_MANAGER_WIFI_VIDEO_RCVBUF_SIZE;

    link = NULL;

    ret = linkg_wifi_link_create(&link_config, &wifi_config, &link);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("create Wi-Fi link failed, error=%d", ret);
        return ret;
    }

    ret = _linkg_link_manager_build_append(build, link);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("register Wi-Fi link failed, error=%d", ret);
        linkg_link_destroy(link);
        return ret;
    }

    LINKG_LOG_INFO("Wi-Fi link created, id=%u", linkg_link_get_id(link));

    return 0;
}

/**
 * @brief 创建蜂窝业务链路。
 *
 * @note 本函数只创建LinkG业务数据使用的蜂窝Link，不负责蜂窝接入模块生命周期。
 */
static int _linkg_link_manager_create_cellular(const linkg_config_t *config, const linkg_link_manager_config_t *manager_config, linkg_link_manager_build_t *build)
{
    linkg_cellular_link_config_t cellular_config;
    linkg_link_config_t          link_config;
    linkg_link_t                *link;
    int                          ret;

    if (config == NULL || manager_config == NULL || build == NULL)
    {
        return -EINVAL;
    }

    if (!config->paths.cellular.enabled)
    {
        return -EINVAL;
    }

    if (!config->links.cellular.enabled)
    {
        LINKG_LOG_ERROR("cellular path enabled but cellular access module disabled");
        return -EINVAL;
    }

    _linkg_link_manager_build_config(&link_config, manager_config, LINKG_LINK_MANAGER_CELLULAR_NAME, LINKG_LINK_ACCESS_CELLULAR);

    memset(&cellular_config, 0, sizeof(cellular_config));

    cellular_config.data_port     = LINKG_RESOURCE_UDP_PORT_CELLULAR_DATA;
    cellular_config.realtime_port = LINKG_RESOURCE_UDP_PORT_CELLULAR_REALTIME;
    cellular_config.video_port    = LINKG_RESOURCE_UDP_PORT_CELLULAR_VIDEO;

    link = NULL;

    ret = linkg_cellular_link_create(&link_config, &cellular_config, &link);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("create cellular link failed, error=%d", ret);
        return ret;
    }

    ret = _linkg_link_manager_build_append(build, link);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("register cellular link failed, error=%d", ret);
        linkg_link_destroy(link);
        return ret;
    }

    LINKG_LOG_INFO("cellular link created, id=%u", linkg_link_get_id(link));

    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化本地业务链路管理模块。
 *
 * @note Access Module生命周期由links配置和Network Service管理。
 *       Link Manager只根据paths配置创建参与LinkG业务传输的本地Link。
 */
int linkg_link_manager_init(const linkg_link_manager_config_t *manager_config)
{
    linkg_link_manager_build_t build;
    linkg_config_t             config;
    uint32_t                   index;
    int                        ret;

    if (manager_config == NULL || manager_config->packet_pool == NULL)
    {
        return -EINVAL;
    }

    if (g_link_manager.initialized)
    {
        return -EALREADY;
    }

    memset(&build, 0, sizeof(build));
    memset(&config, 0, sizeof(config));

    ret = linkg_config_create_snapshot(&config);
    if (ret != 0)
    {
        return ret;
    }

    if (config.paths.wifi.enabled)
    {
        ret = _linkg_link_manager_create_wifi(&config, manager_config, &build);
        if (ret != 0)
        {
            goto fail;
        }
    }

    if (config.paths.cellular.enabled)
    {
        ret = _linkg_link_manager_create_cellular(&config, manager_config, &build);
        if (ret != 0)
        {
            goto fail;
        }
    }

    if (build.count == 0U)
    {
        ret = -ENODEV;
        goto fail;
    }

    memset(&g_link_manager, 0, sizeof(g_link_manager));

    for (index = 0U; index < build.count; index++)
    {
        g_link_manager.links[index] = build.links[index];
        build.links[index] = NULL;
    }

    g_link_manager.count       = build.count;
    g_link_manager.initialized = true;

    LINKG_LOG_INFO("link manager initialized, count=%u", g_link_manager.count);

    return 0;

fail:
    _linkg_link_manager_build_destroy(&build);

    return ret;
}

/**
 * @brief 启动全部已创建的业务链路。
 */
int linkg_link_manager_start(void)
{
    linkg_link_state_t state;
    linkg_link_t      *link;
    uint32_t           index;
    int                first_error;
    int                ret;

    if (!g_link_manager.initialized)
    {
        return -ENODEV;
    }

    if (g_link_manager.receive == NULL)
    {
        return -ENODEV;
    }

    first_error = 0;

    for (index = 0U; index < g_link_manager.count; index++)
    {
        link = g_link_manager.links[index];
        if (link == NULL)
        {
            if (first_error == 0)
            {
                first_error = -EFAULT;
            }

            continue;
        }

        state = linkg_link_get_state(link);

        if (state == LINKG_LINK_STATE_RUNNING)
        {
            continue;
        }

        if (state != LINKG_LINK_STATE_STOPPED)
        {
            LINKG_LOG_ERROR("start link rejected, link=%s, state=%d",
                            linkg_link_get_name(link),
                            (int)state);

            if (first_error == 0)
            {
                first_error = -EBUSY;
            }

            continue;
        }

        ret = linkg_link_start(link);
        if (ret != 0 && ret != -EALREADY)
        {
            LINKG_LOG_ERROR("start managed link failed, link=%s, id=%u, error=%d",
                            linkg_link_get_name(link),
                            linkg_link_get_id(link),
                            ret);

            if (first_error == 0)
            {
                first_error = ret;
            }

            continue;
        }

        LINKG_LOG_INFO("managed link started, link=%s, id=%u",
                       linkg_link_get_name(link),
                       linkg_link_get_id(link));
    }

    if (first_error != 0)
    {
        return first_error;
    }

    LINKG_LOG_INFO("all managed links started, count=%u", g_link_manager.count);

    return 0;
}

/**
 * @brief 停止全部已创建的业务链路。
 */
int linkg_link_manager_stop(void)
{
    linkg_link_t *link;
    uint32_t      index;
    int           first_error;
    int           ret;

    if (!g_link_manager.initialized)
    {
        return -ENODEV;
    }

    first_error = 0;

    for (index = g_link_manager.count; index > 0U; index--)
    {
        link = g_link_manager.links[index - 1U];
        if (link == NULL)
        {
            if (first_error == 0)
            {
                first_error = -EFAULT;
            }

            continue;
        }

        if (linkg_link_get_state(link) == LINKG_LINK_STATE_STOPPED)
        {
            continue;
        }

        ret = linkg_link_stop(link);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("stop managed link failed, link=%s, id=%u, error=%d",
                            linkg_link_get_name(link),
                            linkg_link_get_id(link),
                            ret);

            if (first_error == 0)
            {
                first_error = ret;
            }

            continue;
        }

        LINKG_LOG_INFO("managed link stopped, link=%s, id=%u",
                       linkg_link_get_name(link),
                       linkg_link_get_id(link));
    }

    if (first_error != 0)
    {
        return first_error;
    }

    LINKG_LOG_INFO("all managed links stopped, count=%u", g_link_manager.count);

    return 0;
}

/**
 * @brief 反初始化本地业务链路管理模块。
 *
 * @note 调用前全部业务Link必须已经进入STOPPED状态。
 */
int linkg_link_manager_deinit(void)
{
    linkg_link_t *link;
    uint32_t      index;

    if (!g_link_manager.initialized)
    {
        return 0;
    }

    if (!_linkg_link_manager_all_stopped())
    {
        return -EBUSY;
    }

    for (index = g_link_manager.count; index > 0U; index--)
    {
        link = g_link_manager.links[index - 1U];
        g_link_manager.links[index - 1U] = NULL;

        if (link != NULL)
        {
            linkg_link_destroy(link);
        }
    }

    memset(&g_link_manager, 0, sizeof(g_link_manager));

    LINKG_LOG_INFO("link manager deinitialized");

    return 0;
}

/****************************** 接收处理 ******************************/

/**
 * @brief 注册业务链路统一接收处理函数。
 *
 * @note 必须在全部业务Link启动前调用，运行期间不得修改。
 */
int linkg_link_manager_register_receive_handler(linkg_link_receive_batch_func_t receive, void *user_data)
{
    if (!g_link_manager.initialized)
    {
        return -ENODEV;
    }

    if (receive == NULL)
    {
        return -EINVAL;
    }

    if (!_linkg_link_manager_all_stopped())
    {
        return -EBUSY;
    }

    if (g_link_manager.receive != NULL)
    {
        return -EALREADY;
    }

    g_link_manager.receive           = receive;
    g_link_manager.receive_user_data = user_data;

    return 0;
}

/**
 * @brief 注销业务链路统一接收处理函数。
 *
 * @note 必须在全部业务Link停止后调用。
 */
int linkg_link_manager_unregister_receive_handler(void)
{
    if (!g_link_manager.initialized)
    {
        return -ENODEV;
    }

    if (!_linkg_link_manager_all_stopped())
    {
        return -EBUSY;
    }

    g_link_manager.receive           = NULL;
    g_link_manager.receive_user_data = NULL;

    return 0;
}

/****************************** 发送清理 ******************************/

/**
 * @brief 清理目标Path所属链路发送队列中引用该Path的待发送Packet。
 *
 * @note 目标Path必须已经退出ACTIVE状态。调用前缓存其link_id，只查找并清理
 *       唯一归属Link；下层返回后不得再次读取Path字段。
 */
int linkg_link_manager_purge_tx_path(linkg_path_t *path, uint32_t *purged_count)
{
    linkg_link_t *link;
    uint32_t      link_id;

    if (path == NULL || purged_count == NULL)
    {
        return -EINVAL;
    }

    *purged_count = 0U;

    if (!g_link_manager.initialized)
    {
        return -ENODEV;
    }

    if (linkg_path_is_active(path))
    {
        return -EBUSY;
    }

    link_id = path->link_id;
    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return -ENODEV;
    }

    link = _linkg_link_manager_find(link_id);
    if (link == NULL)
    {
        return -ENOENT;
    }

    return linkg_link_purge_tx_path(link, path, purged_count);
}

/****************************** 链路查询 ******************************/

/**
 * @brief 根据运行实例标识获取业务链路。
 */
linkg_link_t *linkg_link_manager_get(uint32_t link_id)
{
    if (!g_link_manager.initialized)
    {
        return NULL;
    }

    return _linkg_link_manager_find(link_id);
}

/**
 * @brief 获取指定接入类型对应的业务链路实例标识。
 */
uint32_t linkg_link_manager_get_id(linkg_link_access_t access)
{
    uint32_t index;

    if (!g_link_manager.initialized || access == LINKG_LINK_ACCESS_NONE)
    {
        return LINKG_LINK_ID_INVALID;
    }

    for (index = 0U; index < g_link_manager.count; index++)
    {
        if (g_link_manager.links[index] != NULL &&
            linkg_link_get_access(g_link_manager.links[index]) == access)
        {
            return linkg_link_get_id(g_link_manager.links[index]);
        }
    }

    return LINKG_LINK_ID_INVALID;
}

/**
 * @brief 获取当前已创建业务链路数量。
 */
uint32_t linkg_link_manager_count(void)
{
    return g_link_manager.initialized ? g_link_manager.count : 0U;
}

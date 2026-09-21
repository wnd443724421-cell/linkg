/**
 * @file linkg_link_manager.c
 * @brief LinkG本地链路管理实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-09-21
 */

#include "linkg_link_manager.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_log.h"

#include "link_internal.h"

/****************************** 模块常量 ******************************/

#define LINKG_LINK_MANAGER_MAX_LINKS 2U // 最大同时注册业务链路数量

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_link_t                    *links[LINKG_LINK_MANAGER_MAX_LINKS]; // 已注册本地业务链路，仅借用对象引用
    linkg_link_receive_batch_func_t  receive;                             // 上层统一接收处理函数
    void                            *receive_user_data;                   // 接收处理私有数据
    uint32_t                         count;                               // 当前已注册业务链路数量
    uint32_t                         next_id;                             // 下一运行实例标识
    bool                             initialized;                         // 模块是否已经初始化
} linkg_link_manager_context_t;

/****************************** 全局上下文 ******************************/

/**
 * @brief Link Manager全局运行上下文。
 *
 * Manager仅借用已注册Link对象，不负责具体Link的创建、启动、停止和销毁。
 * 注册和注销属于模块生命周期操作，不得与外部借用Link指针的运行逻辑并发执行。
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

    for (index = 0U; index < LINKG_LINK_MANAGER_MAX_LINKS; index++)
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
 * @brief 根据接入类型查找业务链路。
 */
static linkg_link_t *_linkg_link_manager_find_access(linkg_link_access_t access)
{
    uint32_t index;

    if (access == LINKG_LINK_ACCESS_NONE)
    {
        return NULL;
    }

    for (index = 0U; index < LINKG_LINK_MANAGER_MAX_LINKS; index++)
    {
        if (g_link_manager.links[index] != NULL &&
            linkg_link_get_access(g_link_manager.links[index]) == access)
        {
            return g_link_manager.links[index];
        }
    }

    return NULL;
}

/**
 * @brief 查找业务链路当前注册槽位。
 */
static int _linkg_link_manager_find_slot(const linkg_link_t *link, uint32_t *slot)
{
    uint32_t index;

    if (link == NULL || slot == NULL)
    {
        return -EINVAL;
    }

    for (index = 0U; index < LINKG_LINK_MANAGER_MAX_LINKS; index++)
    {
        if (g_link_manager.links[index] == link)
        {
            *slot = index;
            return 0;
        }
    }

    return -ENOENT;
}

/**
 * @brief 查找空闲业务链路注册槽位。
 */
static int _linkg_link_manager_find_free_slot(uint32_t *slot)
{
    uint32_t index;

    if (slot == NULL)
    {
        return -EINVAL;
    }

    for (index = 0U; index < LINKG_LINK_MANAGER_MAX_LINKS; index++)
    {
        if (g_link_manager.links[index] == NULL)
        {
            *slot = index;
            return 0;
        }
    }

    return -ENOSPC;
}

/**
 * @brief 分配当前Manager内唯一的运行实例标识。
 */
static int _linkg_link_manager_allocate_id(uint32_t *link_id)
{
    uint32_t candidate;
    uint32_t attempt;

    if (link_id == NULL)
    {
        return -EINVAL;
    }

    candidate = g_link_manager.next_id;

    for (attempt = 0U; attempt <= LINKG_LINK_MANAGER_MAX_LINKS; attempt++)
    {
        if (candidate == LINKG_LINK_ID_INVALID)
        {
            candidate = 1U;
        }

        if (_linkg_link_manager_find(candidate) == NULL)
        {
            *link_id = candidate;

            g_link_manager.next_id = candidate + 1U;
            if (g_link_manager.next_id == LINKG_LINK_ID_INVALID)
            {
                g_link_manager.next_id = 1U;
            }

            return 0;
        }

        candidate++;
    }

    return -ENOSPC;
}

/**
 * @brief 判断全部已注册业务链路是否已经停止。
 */
static bool _linkg_link_manager_all_stopped(void)
{
    uint32_t index;

    for (index = 0U; index < LINKG_LINK_MANAGER_MAX_LINKS; index++)
    {
        if (g_link_manager.links[index] == NULL)
        {
            continue;
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
 * items中的Packet仅在当前回调期间借用，基础引用由Link RX在回调返回后统一释放。
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

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化本地业务链路管理模块。
 */
int linkg_link_manager_init(void)
{
    if (g_link_manager.initialized)
    {
        return -EALREADY;
    }

    memset(&g_link_manager, 0, sizeof(g_link_manager));

    g_link_manager.next_id     = 1U;
    g_link_manager.initialized = true;

    LINKG_LOG_INFO("link manager initialized");

    return 0;
}

/**
 * @brief 反初始化本地业务链路管理模块。
 *
 * 调用前所有具体业务Link和统一接收处理函数必须已经注销。
 */
int linkg_link_manager_deinit(void)
{
    if (!g_link_manager.initialized)
    {
        return 0;
    }

    if (g_link_manager.count != 0U || g_link_manager.receive != NULL)
    {
        return -EBUSY;
    }

    memset(&g_link_manager, 0, sizeof(g_link_manager));

    LINKG_LOG_INFO("link manager deinitialized");

    return 0;
}

/****************************** 链路注册 ******************************/

/**
 * @brief 注册已经创建完成的业务链路。
 *
 * Link Manager仅借用Link对象，不获取对象所有权。注册时Link必须处于STOPPED状态，
 * 且基类接收出口尚未绑定。成功后Manager分配运行实例ID并绑定统一接收转发入口。
 */
int linkg_link_manager_register(linkg_link_t *link)
{
    linkg_link_runtime_t *runtime;
    uint32_t              link_id;
    uint32_t              slot;
    int                   ret;

    if (!g_link_manager.initialized)
    {
        return -ENODEV;
    }

    if (link == NULL || link->runtime == NULL || link->ops == NULL)
    {
        return -EINVAL;
    }

    if (linkg_link_get_access(link) == LINKG_LINK_ACCESS_NONE)
    {
        return -EINVAL;
    }

    if (linkg_link_get_state(link) != LINKG_LINK_STATE_STOPPED)
    {
        return -EBUSY;
    }

    if (linkg_link_get_id(link) != LINKG_LINK_ID_INVALID)
    {
        return -EALREADY;
    }

    ret = _linkg_link_manager_find_slot(link, &slot);
    if (ret == 0)
    {
        return -EALREADY;
    }

    if (ret != -ENOENT)
    {
        return ret;
    }

    if (_linkg_link_manager_find_access(linkg_link_get_access(link)) != NULL)
    {
        return -EEXIST;
    }

    if (g_link_manager.count >= LINKG_LINK_MANAGER_MAX_LINKS)
    {
        return -ENOSPC;
    }

    runtime = link->runtime;

    if (runtime->receive != NULL || runtime->receive_user_data != NULL)
    {
        return -EBUSY;
    }

    ret = _linkg_link_manager_find_free_slot(&slot);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_link_manager_allocate_id(&link_id);
    if (ret != 0)
    {
        return ret;
    }

    link->id = link_id;

    runtime->receive           = _linkg_link_manager_receive;
    runtime->receive_user_data = NULL;

    g_link_manager.links[slot] = link;
    g_link_manager.count++;

    LINKG_LOG_INFO("link registered, link=%s, id=%u, access=%d",
                   linkg_link_get_name(link),
                   linkg_link_get_id(link),
                   (int)linkg_link_get_access(link));

    return 0;
}

/**
 * @brief 注销已经停止的业务链路。
 *
 * 本接口只解除Manager对Link的注册关系，不销毁Link对象。调用方必须保证与该Link
 * 关联的Path和外部借用引用已经完成退役，返回后由具体链路模块负责销毁对象。
 */
int linkg_link_manager_unregister(linkg_link_t *link)
{
    linkg_link_runtime_t *runtime;
    uint32_t              link_id;
    uint32_t              slot;
    int                   ret;

    if (!g_link_manager.initialized)
    {
        return -ENODEV;
    }

    if (link == NULL || link->runtime == NULL)
    {
        return -EINVAL;
    }

    if (linkg_link_get_state(link) != LINKG_LINK_STATE_STOPPED)
    {
        return -EBUSY;
    }

    ret = _linkg_link_manager_find_slot(link, &slot);
    if (ret != 0)
    {
        return ret;
    }

    runtime = link->runtime;
    link_id = linkg_link_get_id(link);

    g_link_manager.links[slot] = NULL;
    g_link_manager.count--;

    runtime->receive           = NULL;
    runtime->receive_user_data = NULL;

    link->id = LINKG_LINK_ID_INVALID;

    LINKG_LOG_INFO("link unregistered, link=%s, id=%u",
                   linkg_link_get_name(link),
                   link_id);

    return 0;
}

/****************************** 接收处理 ******************************/

/**
 * @brief 注册业务链路统一接收处理函数。
 *
 * 必须在全部已注册业务Link停止时修改，避免与Link RX线程并发更新回调。
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
 * 必须在全部已注册业务Link停止后调用。
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
 * 目标Path必须已经退出ACTIVE状态。调用前缓存其link_id，只查找并清理唯一归属Link；
 * 下层返回后不得再次读取Path字段。
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
 * @brief 根据接入类型获取业务链路。
 */
linkg_link_t *linkg_link_manager_get_by_access(linkg_link_access_t access)
{
    if (!g_link_manager.initialized || access == LINKG_LINK_ACCESS_NONE)
    {
        return NULL;
    }

    return _linkg_link_manager_find_access(access);
}

/**
 * @brief 获取指定接入类型对应的业务链路实例标识。
 */
uint32_t linkg_link_manager_get_id(linkg_link_access_t access)
{
    linkg_link_t *link;

    link = linkg_link_manager_get_by_access(access);
    if (link == NULL)
    {
        return LINKG_LINK_ID_INVALID;
    }

    return linkg_link_get_id(link);
}

/**
 * @brief 获取当前已注册业务链路数量。
 */
uint32_t linkg_link_manager_count(void)
{
    return g_link_manager.initialized ? g_link_manager.count : 0U;
}

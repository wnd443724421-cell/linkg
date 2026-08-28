/**
 * @file linkg_switch.c
 * @brief LinkG链路切换状态实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-28
 */

#include "linkg_switch.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#include "linkg_link.h"
#include "linkg_system_resources.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_PEER_MAX LINKG_RESOURCE_NETWORK_STA_MAX // 最大直接对端发送计划数量

/****************************** 内部类型 ******************************/

typedef struct
{
    uint8_t           peer_node_id; // 直接对端节点编号
    linkg_send_plan_t plan;         // 当前发送计划
    bool              valid;        // 当前槽位是否有效
} linkg_switch_peer_t;

typedef struct
{
    pthread_mutex_t     lock;                         // 发送计划保护锁
    linkg_switch_peer_t peers[LINKG_SWITCH_PEER_MAX]; // 直接对端发送计划
    bool                initialized;                  // 模块是否已经初始化
} linkg_switch_context_t;

/****************************** 全局上下文 ******************************/

static linkg_switch_context_t g_switch;

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取Switch状态锁。
 */
static int _linkg_switch_lock(void)
{
    int ret;

    ret = pthread_mutex_lock(&g_switch.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 释放Switch状态锁并保留原操作结果。
 */
static int _linkg_switch_unlock(int result)
{
    int ret;

    ret = pthread_mutex_unlock(&g_switch.lock);
    if (ret != 0)
    {
        return -ret;
    }

    return result;
}

/**
 * @brief 校验直接对端节点编号。
 */
static bool _linkg_switch_node_id_valid(uint8_t peer_node_id)
{
    return peer_node_id >= LINKG_RESOURCE_NODE_ID_MIN &&
           peer_node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 校验发送计划。
 */
static bool _linkg_switch_plan_valid(const linkg_send_plan_t *plan)
{
    if (plan == NULL)
    {
        return false;
    }

    if (plan->mode == LINKG_SEND_MODE_NONE)
    {
        return plan->primary_link_id == LINKG_LINK_ID_INVALID &&
               plan->secondary_link_id == LINKG_LINK_ID_INVALID;
    }

    if (plan->mode == LINKG_SEND_MODE_SINGLE)
    {
        if (plan->primary_link_id == LINKG_LINK_ID_INVALID)
        {
            return false;
        }

        return plan->secondary_link_id == LINKG_LINK_ID_INVALID ||
               plan->secondary_link_id != plan->primary_link_id;
    }

    if (plan->mode == LINKG_SEND_MODE_REDUNDANT)
    {
        return plan->primary_link_id != LINKG_LINK_ID_INVALID &&
               plan->secondary_link_id != LINKG_LINK_ID_INVALID &&
               plan->primary_link_id != plan->secondary_link_id;
    }

    return false;
}

/**
 * @brief 查找指定直接对端发送计划。
 *
 * @note 调用方必须持有Switch状态锁。
 */
static int _linkg_switch_find_locked(uint8_t peer_node_id)
{
    uint32_t index;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        if (!g_switch.peers[index].valid)
        {
            continue;
        }

        if (g_switch.peers[index].peer_node_id == peer_node_id)
        {
            return (int)index;
        }
    }

    return -1;
}

/**
 * @brief 查找空闲发送计划槽位。
 *
 * @note 调用方必须持有Switch状态锁。
 */
static int _linkg_switch_find_unused_locked(void)
{
    uint32_t index;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        if (!g_switch.peers[index].valid)
        {
            return (int)index;
        }
    }

    return -1;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化链路切换状态模块。
 */
int linkg_switch_init(void)
{
    int ret;

    if (g_switch.initialized)
    {
        return -EALREADY;
    }

    memset(&g_switch, 0, sizeof(g_switch));

    ret = pthread_mutex_init(&g_switch.lock, NULL);
    if (ret != 0)
    {
        memset(&g_switch, 0, sizeof(g_switch));
        return -ret;
    }

    g_switch.initialized = true;

    return 0;
}

/**
 * @brief 反初始化链路切换状态模块。
 *
 * @note 调用前必须停止所有可能访问Switch模块的线程。
 */
int linkg_switch_deinit(void)
{
    int ret;

    if (!g_switch.initialized)
    {
        return 0;
    }

    ret = _linkg_switch_lock();
    if (ret != 0)
    {
        return ret;
    }

    g_switch.initialized = false;

    ret = _linkg_switch_unlock(0);
    if (ret != 0)
    {
        g_switch.initialized = true;
        return ret;
    }

    ret = pthread_mutex_destroy(&g_switch.lock);
    if (ret != 0)
    {
        g_switch.initialized = true;
        return -ret;
    }

    memset(&g_switch, 0, sizeof(g_switch));

    return 0;
}

/****************************** 计划管理 ******************************/

/**
 * @brief 设置或更新指定直接对端的发送计划。
 */
int linkg_switch_set_plan(uint8_t peer_node_id, const linkg_send_plan_t *plan)
{
    linkg_switch_peer_t *peer;
    int                  index;
    int                  ret;

    if (!g_switch.initialized)
    {
        return -ENODEV;
    }

    if (!_linkg_switch_node_id_valid(peer_node_id) || !_linkg_switch_plan_valid(plan))
    {
        return -EINVAL;
    }

    ret = _linkg_switch_lock();
    if (ret != 0)
    {
        return ret;
    }

    index = _linkg_switch_find_locked(peer_node_id);
    if (index < 0)
    {
        index = _linkg_switch_find_unused_locked();
        if (index < 0)
        {
            return _linkg_switch_unlock(-ENOSPC);
        }
    }

    peer = &g_switch.peers[index];

    peer->peer_node_id = peer_node_id;
    peer->plan         = *plan;
    peer->valid        = true;

    return _linkg_switch_unlock(0);
}

/**
 * @brief 获取指定直接对端当前发送计划。
 */
int linkg_switch_get_plan(uint8_t peer_node_id, linkg_send_plan_t *plan)
{
    int index;
    int ret;

    if (!g_switch.initialized)
    {
        return -ENODEV;
    }

    if (!_linkg_switch_node_id_valid(peer_node_id) || plan == NULL)
    {
        return -EINVAL;
    }

    memset(plan, 0, sizeof(*plan));

    ret = _linkg_switch_lock();
    if (ret != 0)
    {
        return ret;
    }

    index = _linkg_switch_find_locked(peer_node_id);
    if (index < 0)
    {
        return _linkg_switch_unlock(-ENOENT);
    }

    *plan = g_switch.peers[index].plan;

    return _linkg_switch_unlock(0);
}

/**
 * @brief 删除指定直接对端发送计划。
 */
int linkg_switch_remove_plan(uint8_t peer_node_id)
{
    int index;
    int ret;

    if (!g_switch.initialized)
    {
        return -ENODEV;
    }

    if (!_linkg_switch_node_id_valid(peer_node_id))
    {
        return -EINVAL;
    }

    ret = _linkg_switch_lock();
    if (ret != 0)
    {
        return ret;
    }

    index = _linkg_switch_find_locked(peer_node_id);
    if (index < 0)
    {
        return _linkg_switch_unlock(-ENOENT);
    }

    memset(&g_switch.peers[index], 0, sizeof(g_switch.peers[index]));

    return _linkg_switch_unlock(0);
}

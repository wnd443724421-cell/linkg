/**
 * @file linkg_scheduler.c
 * @brief LinkG发送调度实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-28
 */

#include "linkg_scheduler.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "linkg_link.h"
#include "linkg_link_manager.h"
#include "linkg_node.h"
#include "linkg_path.h"
#include "linkg_switch.h"
#include "linkg_system_resources.h"

/****************************** 模块常量 ******************************/

#define LINKG_SCHEDULER_BATCH_CHUNK_SIZE 32U                 // 单次调度批处理最大包数
#define LINKG_SCHEDULER_PEER_GROUP_MAX   LINKG_NODE_PEER_MAX // 单批最大直接Peer分组数

/****************************** 内部类型 ******************************/

typedef struct
{
    uint32_t item_indices[LINKG_SCHEDULER_BATCH_CHUNK_SIZE]; // 属于该下一跳Peer的批次索引
    uint32_t count;                                          // 当前Peer数据包数量
    uint8_t  next_hop_node_id;                               // 当前物理下一跳节点编号
} linkg_scheduler_peer_group_t;

/****************************** 模块状态 ******************************/

static _Atomic bool g_scheduler_initialized = false;

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验节点编号。
 */
static bool _linkg_scheduler_node_id_valid(uint8_t node_id)
{
    return node_id >= LINKG_RESOURCE_NODE_ID_MIN &&
           node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 校验链路标识。
 */
static bool _linkg_scheduler_link_id_valid(uint32_t link_id)
{
    return link_id != LINKG_LINK_ID_INVALID;
}

/**
 * @brief 设置Peer分组中全部数据包的调度结果。
 */
static void _linkg_scheduler_set_group_result(linkg_scheduler_tx_item_t *items, const linkg_scheduler_peer_group_t *group, int result)
{
    uint32_t index;

    if (items == NULL || group == NULL)
    {
        return;
    }

    for (index = 0U; index < group->count; index++)
    {
        items[group->item_indices[index]].result = result;
    }
}

/**
 * @brief 统计Peer分组中的成功数据包数量。
 */
static uint32_t _linkg_scheduler_group_success_count(const linkg_scheduler_tx_item_t *items, const linkg_scheduler_peer_group_t *group)
{
    uint32_t success_count;
    uint32_t index;

    if (items == NULL || group == NULL)
    {
        return 0U;
    }

    success_count = 0U;

    for (index = 0U; index < group->count; index++)
    {
        if (items[group->item_indices[index]].result == 0)
        {
            success_count++;
        }
    }

    return success_count;
}

/**
 * @brief 向指定链路批量提交同一直接Peer的数据包。
 *
 * @note 整个同步batch只获取一个Path引用和一份Endpoint快照。
 *       Packet仅借用，不转移调用方原始引用所有权。
 *
 * @return <0表示本函数参数或内部调用异常，>=0表示成功提交的数据包数量。
 */
static int _linkg_scheduler_submit_link_batch(linkg_scheduler_tx_item_t *items, const linkg_scheduler_peer_group_t *group, uint32_t link_id, int *results)
{
    linkg_packet_t        *packets[LINKG_SCHEDULER_BATCH_CHUNK_SIZE];
    linkg_path_endpoint_t  destination;
    linkg_path_t          *path;
    linkg_link_t          *link;
    uint32_t               item_index;
    uint32_t               index;
    int                    ret;

    if (items == NULL ||
        group == NULL ||
        results == NULL ||
        group->count == 0U ||
        group->count > LINKG_SCHEDULER_BATCH_CHUNK_SIZE ||
        !_linkg_scheduler_node_id_valid(group->next_hop_node_id) ||
        !_linkg_scheduler_link_id_valid(link_id))
    {
        return -EINVAL;
    }

    for (index = 0U; index < group->count; index++)
    {
        results[index] = -EINPROGRESS;
    }

    link = linkg_link_manager_get(link_id);
    if (link == NULL)
    {
        for (index = 0U; index < group->count; index++)
        {
            results[index] = -ENOENT;
        }

        return 0;
    }

    if (!linkg_link_is_running(link))
    {
        for (index = 0U; index < group->count; index++)
        {
            results[index] = -ENETDOWN;
        }

        return 0;
    }

    path = NULL;
    memset(&destination, 0, sizeof(destination));

    ret = linkg_node_acquire_path(group->next_hop_node_id, link_id, &path, &destination);
    if (ret != 0)
    {
        for (index = 0U; index < group->count; index++)
        {
            results[index] = ret;
        }

        return 0;
    }

    for (index = 0U; index < group->count; index++)
    {
        item_index     = group->item_indices[index];
        packets[index] = items[item_index].packet;
    }

    ret = linkg_link_submit_batch(link,
                              path,
                              LINKG_LINK_TX_CLASS_DATA,
                              &destination,
                              packets,
                              group->count,
                              results);

    // Link同步提交返回后释放Scheduler持有的Path引用。
    linkg_path_release(path);

    return ret;
}

/**
 * @brief 调度同一直接Peer的一批数据包。
 *
 * @return <0表示本函数参数或内部调用异常，>=0表示当前Peer成功提交的数据包数量。
 */
static int _linkg_scheduler_submit_group(linkg_scheduler_tx_item_t *items, const linkg_scheduler_peer_group_t *group, linkg_scheduler_policy_t policy, uint32_t specified_link_id)
{
    int               primary_results[LINKG_SCHEDULER_BATCH_CHUNK_SIZE];
    int               secondary_results[LINKG_SCHEDULER_BATCH_CHUNK_SIZE];
    linkg_send_plan_t plan;
    uint32_t          success_count;
    uint32_t          item_index;
    uint32_t          index;
    bool              redundant;
    int               primary_ret;
    int               secondary_ret;

    if (items == NULL ||
        group == NULL ||
        group->count == 0U ||
        group->count > LINKG_SCHEDULER_BATCH_CHUNK_SIZE)
    {
        return -EINVAL;
    }

    // SPECIFIED策略不读取Switch发送计划。
    if (policy == LINKG_SCHEDULER_POLICY_SPECIFIED)
    {
        primary_ret = _linkg_scheduler_submit_link_batch(items, group, specified_link_id, primary_results);
        if (primary_ret < 0)
        {
            _linkg_scheduler_set_group_result(items, group, primary_ret);
            return primary_ret;
        }

        for (index = 0U; index < group->count; index++)
        {
            items[group->item_indices[index]].result = primary_results[index];
        }

        return primary_ret;
    }

    memset(&plan, 0, sizeof(plan));

    // 同一个直接下一跳Peer的整个batch只读取一次发送计划。
    primary_ret = linkg_switch_get_plan(group->next_hop_node_id, &plan);
    if (primary_ret != 0)
    {
        _linkg_scheduler_set_group_result(items, group, primary_ret);
        return 0;
    }

    if (plan.mode == LINKG_SEND_MODE_NONE)
    {
        _linkg_scheduler_set_group_result(items, group, -ENETDOWN);
        return 0;
    }

    if (plan.mode != LINKG_SEND_MODE_SINGLE &&
        plan.mode != LINKG_SEND_MODE_REDUNDANT)
    {
        _linkg_scheduler_set_group_result(items, group, -EINVAL);
        return 0;
    }

    if (!_linkg_scheduler_link_id_valid(plan.primary_link_id))
    {
        _linkg_scheduler_set_group_result(items, group, -ENODEV);
        return 0;
    }

    primary_ret = _linkg_scheduler_submit_link_batch(items, group, plan.primary_link_id, primary_results);
    if (primary_ret < 0)
    {
        _linkg_scheduler_set_group_result(items, group, primary_ret);
        return primary_ret;
    }

    for (index = 0U; index < group->count; index++)
    {
        item_index               = group->item_indices[index];
        items[item_index].result = primary_results[index];
    }

    redundant = policy == LINKG_SCHEDULER_POLICY_REDUNDANT ||
                plan.mode == LINKG_SEND_MODE_REDUNDANT;

    if (!redundant)
    {
        return primary_ret;
    }

    if (!_linkg_scheduler_link_id_valid(plan.secondary_link_id) ||
        plan.secondary_link_id == plan.primary_link_id)
    {
        return primary_ret;
    }

    secondary_ret = _linkg_scheduler_submit_link_batch(items, group, plan.secondary_link_id, secondary_results);
    if (secondary_ret < 0)
    {
        for (index = 0U; index < group->count; index++)
        {
            secondary_results[index] = secondary_ret;
        }
    }

    /**
     * REDUNDANT策略下任意一条目标链路成功则当前Packet整体成功。
     * 两条链路全部失败时保留主链路返回的具体错误。
     */
    for (index = 0U; index < group->count; index++)
    {
        item_index = group->item_indices[index];

        if (primary_results[index] == 0 || secondary_results[index] == 0)
        {
            items[item_index].result = 0;
            continue;
        }

        items[item_index].result = primary_results[index];
    }

    success_count = _linkg_scheduler_group_success_count(items, group);

    return (int)success_count;
}

/**
 * @brief 将一个内部批次按照直接下一跳Peer分组并完成调度。
 *
 * @return <0表示本函数参数异常，>=0表示当前chunk成功提交的数据包数量。
 */
static int _linkg_scheduler_submit_chunk(linkg_scheduler_tx_item_t *items, uint32_t count, linkg_scheduler_policy_t policy, uint32_t specified_link_id)
{
    linkg_scheduler_peer_group_t groups[LINKG_SCHEDULER_PEER_GROUP_MAX];
    uint32_t                     success_count;
    uint32_t                     group_count;
    uint32_t                     group_index;
    uint32_t                     index;
    int                          ret;

    if (items == NULL || count == 0U || count > LINKG_SCHEDULER_BATCH_CHUNK_SIZE)
    {
        return -EINVAL;
    }

    group_count = 0U;

    // 按当前物理下一跳节点建立批次内分组。
    for (index = 0U; index < count; index++)
    {
        items[index].result = -EINPROGRESS;

        if (!_linkg_scheduler_node_id_valid(items[index].next_hop_node_id) ||
            items[index].packet == NULL)
        {
            items[index].result = -EINVAL;
            continue;
        }

        for (group_index = 0U; group_index < group_count; group_index++)
        {
            if (groups[group_index].next_hop_node_id == items[index].next_hop_node_id)
            {
                break;
            }
        }

        if (group_index == group_count)
        {
            if (group_count >= LINKG_SCHEDULER_PEER_GROUP_MAX)
            {
                items[index].result = -ENOSPC;
                continue;
            }

            groups[group_index].next_hop_node_id = items[index].next_hop_node_id;
            groups[group_index].count            = 0U;
            group_count++;
        }

        groups[group_index].item_indices[groups[group_index].count] = index;
        groups[group_index].count++;
    }

    success_count = 0U;

    for (group_index = 0U; group_index < group_count; group_index++)
    {
        ret = _linkg_scheduler_submit_group(items, &groups[group_index], policy, specified_link_id);
        if (ret < 0)
        {
            _linkg_scheduler_set_group_result(items, &groups[group_index], ret);
            continue;
        }

        success_count += (uint32_t)ret;
    }

    return (int)success_count;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化发送调度模块。
 */
int linkg_scheduler_init(void)
{
    bool expected;

    expected = false;

    if (!atomic_compare_exchange_strong(&g_scheduler_initialized, &expected, true))
    {
        return -EALREADY;
    }

    return 0;
}

/**
 * @brief 反初始化发送调度模块。
 *
 * @note 调用前必须停止所有可能进入调度发送路径的数据面线程。
 */
int linkg_scheduler_deinit(void)
{
    atomic_store(&g_scheduler_initialized, false);

    return 0;
}

/****************************** 数据发送 ******************************/

/**
 * @brief 批量调度数据包。
 *
 * @note Scheduler不接管调用方持有的Packet原始引用。
 *       每条实际发送链路独立获取Path引用；具体Link需要异步保存Packet或Path时，
 *       由对应Link实现自行增加引用。
 *
 * @return <0表示整个batch未进入正常调度流程，>=0表示成功提交的数据包数量。
 */
int linkg_scheduler_submit_batch(linkg_scheduler_tx_item_t *items, uint32_t count, linkg_scheduler_policy_t policy, uint32_t specified_link_id)
{
    uint32_t success_count;
    uint32_t chunk_count;
    uint32_t offset;
    uint32_t index;
    int      first_error;
    int      ret;
    bool     processed;

    if (!atomic_load(&g_scheduler_initialized))
    {
        return -ENODEV;
    }

    if (items == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (policy != LINKG_SCHEDULER_POLICY_DEFAULT &&
        policy != LINKG_SCHEDULER_POLICY_REDUNDANT &&
        policy != LINKG_SCHEDULER_POLICY_SPECIFIED)
    {
        return -EINVAL;
    }

    if (policy == LINKG_SCHEDULER_POLICY_SPECIFIED &&
        !_linkg_scheduler_link_id_valid(specified_link_id))
    {
        return -EINVAL;
    }

    success_count = 0U;
    first_error   = 0;
    processed     = false;
    offset        = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;

        if (chunk_count > LINKG_SCHEDULER_BATCH_CHUNK_SIZE)
        {
            chunk_count = LINKG_SCHEDULER_BATCH_CHUNK_SIZE;
        }

        ret = _linkg_scheduler_submit_chunk(&items[offset], chunk_count, policy, specified_link_id);
        if (ret < 0)
        {
            for (index = 0U; index < chunk_count; index++)
            {
                items[offset + index].result = ret;
            }

            if (first_error == 0)
            {
                first_error = ret;
            }
        }
        else
        {
            processed = true;
            success_count += (uint32_t)ret;
        }

        offset += chunk_count;
    }

    // 所有chunk都发生系统级错误时，整个batch视为未正常执行。
    if (!processed)
    {
        return first_error;
    }

    return (int)success_count;
}

/**
 * @brief 向当前物理下一跳节点调度单个数据包。
 *
 * @note next_hop_node_id表示当前这一跳的直接Peer，而不是Packet最终目的节点。
 *       SPECIFIED策略不读取Switch计划，但仍使用该节点编号获取指定链路Path。
 */
int linkg_scheduler_submit(uint8_t next_hop_node_id, linkg_packet_t *packet, linkg_scheduler_policy_t policy, uint32_t specified_link_id)
{
    linkg_scheduler_tx_item_t item;
    int                       ret;

    if (!_linkg_scheduler_node_id_valid(next_hop_node_id) || packet == NULL)
    {
        return -EINVAL;
    }

    item.packet           = packet;
    item.next_hop_node_id = next_hop_node_id;
    item.result           = -EINPROGRESS;

    ret = linkg_scheduler_submit_batch(&item, 1U, policy, specified_link_id);
    if (ret < 0)
    {
        return ret;
    }

    return item.result;
}

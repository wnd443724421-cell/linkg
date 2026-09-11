/**
 * @file linkg_scheduler.c
 * @brief LinkG发送调度实现
 * @author Dawn
 * @version 1.2.2
 * @date 2026-09-11
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
#include "linkg_transport.h"

/****************************** 模块常量 ******************************/

#define LINKG_SCHEDULER_TX_CHUNK_MAX   32U                 // Scheduler内部单次工作Chunk最大Packet数量
#define LINKG_SCHEDULER_PEER_GROUP_MAX  LINKG_NODE_PEER_MAX // 单个Chunk最大直接Peer分组数量

/****************************** 内部类型 ******************************/

typedef struct
{
    uint32_t item_indices[LINKG_SCHEDULER_TX_CHUNK_MAX]; // 属于当前直接Peer的原批次索引
    uint32_t count;                                      // 当前Peer数据包数量
    uint8_t  peer_node_id;                               // 当前物理下一跳直接Peer节点编号
} linkg_scheduler_peer_group_t;

typedef struct
{
    const linkg_node_info_t *local;            // 当前同步Batch借用的本机Node信息
    uint8_t                  sta_peer_node_id; // STA当前唯一AP直接Peer节点编号
    int                      sta_peer_result;  // STA直接Peer解析结果，AP固定为0
} linkg_scheduler_route_context_t;

/****************************** 模块状态 ******************************/

static _Atomic bool    g_scheduler_initialized      = false;
static _Atomic bool    g_scheduler_sta_peer_cached  = false;
static _Atomic uint8_t g_scheduler_sta_peer_node_id = 0U;

/****************************** 参数校验 ******************************/

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
 * @brief 校验Transport业务类别。
 */
static bool _linkg_scheduler_class_valid(linkg_transport_class_t traffic_class)
{
    return traffic_class >= LINKG_TRANSPORT_CLASS_REALTIME &&
           traffic_class < LINKG_TRANSPORT_CLASS_COUNT;
}

/**
 * @brief 校验Transport帧类型。
 */
static bool _linkg_scheduler_type_valid(linkg_transport_type_t type)
{
    return type > LINKG_TRANSPORT_TYPE_NONE &&
           type < LINKG_TRANSPORT_TYPE_COUNT;
}

/**
 * @brief 校验Scheduler调度策略。
 */
static bool _linkg_scheduler_policy_valid(linkg_scheduler_policy_t policy)
{
    return policy == LINKG_SCHEDULER_POLICY_DEFAULT ||
           policy == LINKG_SCHEDULER_POLICY_REDUNDANT ||
           policy == LINKG_SCHEDULER_POLICY_SPECIFIED;
}

/**
 * @brief 校验Scheduler发送上下文。
 */
static int _linkg_scheduler_validate_context(const linkg_scheduler_tx_context_t *context)
{
    if (context == NULL)
    {
        return -EINVAL;
    }
    if (!_linkg_scheduler_class_valid(context->traffic_class) ||
        !_linkg_scheduler_policy_valid(context->policy))
    {
        return -EINVAL;
    }
    if (context->policy == LINKG_SCHEDULER_POLICY_SPECIFIED &&
        !_linkg_scheduler_link_id_valid(context->specified_link_id))
    {
        return -EINVAL;
    }

    return 0;
}

/****************************** 路由解析 ******************************/

/**
 * @brief 查找STA当前唯一注册的AP直接Peer。
 *
 * @note Node层已经保证STA最多只能存在一个直接Peer，Scheduler只通过公开快照接口读取当前结果。
 */
static int _linkg_scheduler_find_sta_peer(uint8_t *peer_node_id)
{
    linkg_node_peer_snapshot_t snapshot;
    uint32_t                   node_id;
    uint8_t                    cached_node_id;
    int                        ret;

    if (peer_node_id == NULL)
    {
        return -EINVAL;
    }

    if (atomic_load(&g_scheduler_sta_peer_cached))
    {
        cached_node_id = atomic_load(&g_scheduler_sta_peer_node_id);
        ret = linkg_node_get_peer_snapshot(cached_node_id, &snapshot);
        if (ret == 0)
        {
            if (snapshot.info.role != LINKG_DEVICE_ROLE_AP)
            {
                return -EINVAL;
            }

            *peer_node_id = snapshot.info.node_id;
            return 0;
        }
        if (ret != -ENOENT)
        {
            return ret;
        }

        atomic_store(&g_scheduler_sta_peer_cached, false);
    }

    for (node_id = LINKG_RESOURCE_NODE_ID_MIN; node_id <= LINKG_RESOURCE_NODE_ID_MAX; node_id++)
    {
        ret = linkg_node_get_peer_snapshot((uint8_t)node_id, &snapshot);
        if (ret == -ENOENT)
        {
            continue;
        }
        if (ret != 0)
        {
            return ret;
        }
        if (snapshot.info.role != LINKG_DEVICE_ROLE_AP)
        {
            return -EINVAL;
        }

        atomic_store(&g_scheduler_sta_peer_node_id, snapshot.info.node_id);
        atomic_store(&g_scheduler_sta_peer_cached, true);

        *peer_node_id = snapshot.info.node_id;
        return 0;
    }

    return -ENOENT;
}

/**
 * @brief 根据本机角色和最终目标解析当前物理下一跳直接Peer。
 *
 * @note 当前拓扑为AP-STA星型：AP直接发送目标STA；STA发送任意远端节点均先交给唯一AP Peer。
 */
static int _linkg_scheduler_resolve_peer(const linkg_node_info_t *local, uint8_t destination_node_id, uint8_t sta_peer_node_id, uint8_t *peer_node_id)
{
    if (local == NULL || peer_node_id == NULL || !_linkg_scheduler_node_id_valid(destination_node_id))
    {
        return -EINVAL;
    }
    if (destination_node_id == local->node_id)
    {
        return -EHOSTUNREACH;
    }

    if (local->role == LINKG_DEVICE_ROLE_AP)
    {
        *peer_node_id = destination_node_id;
        return 0;
    }
    if (local->role == LINKG_DEVICE_ROLE_STA)
    {
        if (!_linkg_scheduler_node_id_valid(sta_peer_node_id))
        {
            return -ENOENT;
        }

        *peer_node_id = sta_peer_node_id;
        return 0;
    }

    return -EINVAL;
}

/**
 * @brief 为一次普通发送大Batch准备固定路由上下文。
 *
 * @note 本机角色只在大Batch开始时读取一次；STA的唯一AP Peer也只解析一次。
 *       STA暂时没有可用AP Peer不视为上下文构造失败，错误保存在sta_peer_result中，
 *       后续由各Packet写入独立result，保持部分失败语义。
 */
static int _linkg_scheduler_prepare_route_context(linkg_scheduler_route_context_t *route_context)
{
    const linkg_node_info_t *local;

    if (route_context == NULL)
    {
        return -EINVAL;
    }

    memset(route_context, 0, sizeof(*route_context));

    local = linkg_node_get_local();
    if (local == NULL)
    {
        return -ENODEV;
    }
    if (local->role != LINKG_DEVICE_ROLE_AP &&
        local->role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    route_context->local           = local;
    route_context->sta_peer_result = 0;

    if (local->role == LINKG_DEVICE_ROLE_STA)
    {
        route_context->sta_peer_result = _linkg_scheduler_find_sta_peer(&route_context->sta_peer_node_id);
    }

    return 0;
}

/****************************** 结果处理 ******************************/

/**
 * @brief 设置Peer分组中全部普通发送元素的调度结果。
 */
static void _linkg_scheduler_set_tx_group_result(linkg_scheduler_tx_item_t *items, const linkg_scheduler_peer_group_t *group, int result)
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
 * @brief 设置Forward分组中全部元素的调度结果。
 */
static void _linkg_scheduler_set_forward_group_result(int *results, const linkg_scheduler_peer_group_t *group, int result)
{
    uint32_t index;

    if (results == NULL || group == NULL)
    {
        return;
    }

    for (index = 0U; index < group->count; index++)
    {
        results[group->item_indices[index]] = result;
    }
}

/****************************** Target管理 ******************************/

/**
 * @brief 获取一个Transport发送Target并持有对应Path引用。
 *
 * @note Link仅借用Link Manager生命周期内稳定对象；Path成功获取后由调用方负责释放一次引用。
 */
static int _linkg_scheduler_acquire_target(uint8_t peer_node_id, uint32_t link_id, linkg_transport_tx_target_t *target)
{
    linkg_path_endpoint_t destination;
    linkg_path_t          *path;
    linkg_link_t          *link;
    int                    ret;

    if (!_linkg_scheduler_node_id_valid(peer_node_id) ||
        !_linkg_scheduler_link_id_valid(link_id) ||
        target == NULL)
    {
        return -EINVAL;
    }

    memset(target, 0, sizeof(*target));

    link = linkg_link_manager_get(link_id);
    if (link == NULL)
    {
        return -ENOENT;
    }
    if (!linkg_link_is_running(link))
    {
        return -ENETDOWN;
    }

    path = NULL;
    memset(&destination, 0, sizeof(destination));

    ret = linkg_node_acquire_path(peer_node_id, link_id, &path, &destination);
    if (ret != 0)
    {
        return ret;
    }

    target->link        = link;
    target->path        = path;
    target->destination = destination;

    return 0;
}

/**
 * @brief 释放Scheduler为Transport Target持有的全部Path引用。
 */
static void _linkg_scheduler_release_targets(linkg_transport_tx_target_t *targets, uint32_t target_count)
{
    uint32_t index;

    if (targets == NULL)
    {
        return;
    }

    for (index = 0U; index < target_count; index++)
    {
        if (targets[index].path != NULL)
        {
            linkg_path_release(targets[index].path);
            targets[index].path = NULL;
        }
    }
}

/**
 * @brief 根据当前策略和Switch计划构造同一直接Peer的Transport Target数组。
 *
 * @note REDUNDANT模式仍只调用一次Transport；可用主备链路作为同一Context中的多个Target。
 *       主链路暂时不可用时允许备用链路单独承担当前发送。
 */
static int _linkg_scheduler_build_targets(uint8_t peer_node_id, linkg_scheduler_policy_t policy, uint32_t specified_link_id, linkg_transport_tx_target_t *targets, uint32_t *target_count)
{
    linkg_send_plan_t plan;
    uint32_t          count;
    bool              redundant;
    int               primary_error;
    int               secondary_error;
    int               ret;

    if (!_linkg_scheduler_node_id_valid(peer_node_id) ||
        !_linkg_scheduler_policy_valid(policy) ||
        targets == NULL ||
        target_count == NULL)
    {
        return -EINVAL;
    }

    memset(targets, 0, sizeof(*targets) * LINKG_TRANSPORT_TX_TARGET_MAX);
    *target_count = 0U;

    if (policy == LINKG_SCHEDULER_POLICY_SPECIFIED)
    {
        if (!_linkg_scheduler_link_id_valid(specified_link_id))
        {
            return -EINVAL;
        }

        ret = _linkg_scheduler_acquire_target(peer_node_id, specified_link_id, &targets[0]);
        if (ret != 0)
        {
            return ret;
        }

        *target_count = 1U;
        return 0;
    }

    memset(&plan, 0, sizeof(plan));

    ret = linkg_switch_get_plan(peer_node_id, &plan);
    if (ret != 0)
    {
        return ret;
    }
    if (plan.mode == LINKG_SEND_MODE_NONE)
    {
        return -ENETDOWN;
    }
    if (plan.mode != LINKG_SEND_MODE_SINGLE &&
        plan.mode != LINKG_SEND_MODE_REDUNDANT)
    {
        return -EINVAL;
    }
    if (!_linkg_scheduler_link_id_valid(plan.primary_link_id))
    {
        return -ENODEV;
    }

    count         = 0U;
    primary_error = _linkg_scheduler_acquire_target(peer_node_id, plan.primary_link_id, &targets[count]);
    if (primary_error == 0)
    {
        count++;
    }

    redundant = policy == LINKG_SCHEDULER_POLICY_REDUNDANT || plan.mode == LINKG_SEND_MODE_REDUNDANT;

    if (!redundant)
    {
        if (count == 0U)
        {
            return primary_error;
        }

        *target_count = count;
        return 0;
    }

    secondary_error = 0;

    if (_linkg_scheduler_link_id_valid(plan.secondary_link_id) &&
        plan.secondary_link_id != plan.primary_link_id &&
        count < LINKG_TRANSPORT_TX_TARGET_MAX)
    {
        secondary_error = _linkg_scheduler_acquire_target(peer_node_id, plan.secondary_link_id, &targets[count]);
        if (secondary_error == 0)
        {
            count++;
        }
    }

    if (count == 0U)
    {
        if (primary_error != 0)
        {
            return primary_error;
        }
        if (secondary_error != 0)
        {
            return secondary_error;
        }

        return -ENETDOWN;
    }

    *target_count = count;
    return 0;
}

/****************************** 普通发送 ******************************/

/**
 * @brief 调度同一直接Peer的一批普通Transport逻辑Packet。
 */
static int _linkg_scheduler_submit_tx_group(const linkg_scheduler_tx_context_t *scheduler_context, linkg_scheduler_tx_item_t *items, const linkg_scheduler_peer_group_t *group)
{
    int                              transport_results[LINKG_SCHEDULER_TX_CHUNK_MAX];
    linkg_transport_tx_target_t      targets[LINKG_TRANSPORT_TX_TARGET_MAX];
    linkg_transport_tx_context_t     transport_context;
    linkg_transport_tx_item_t        transport_items[LINKG_SCHEDULER_TX_CHUNK_MAX];
    uint32_t                         target_count;
    uint32_t                         success_count;
    uint32_t                         item_index;
    uint32_t                         index;
    int                              ret;

    if (scheduler_context == NULL ||
        items == NULL ||
        group == NULL ||
        group->count == 0U ||
        group->count > LINKG_SCHEDULER_TX_CHUNK_MAX)
    {
        return -EINVAL;
    }

    ret = _linkg_scheduler_build_targets(group->peer_node_id,
                                         scheduler_context->policy,
                                         scheduler_context->specified_link_id,
                                         targets,
                                         &target_count);
    if (ret != 0)
    {
        _linkg_scheduler_set_tx_group_result(items, group, ret);
        return 0;
    }

    memset(&transport_context, 0, sizeof(transport_context));
    memset(transport_items, 0, sizeof(transport_items));

    transport_context.targets       = targets;
    transport_context.target_count  = target_count;
    transport_context.traffic_class = scheduler_context->traffic_class;
    transport_context.peer_node_id  = group->peer_node_id;

    for (index = 0U; index < group->count; index++)
    {
        item_index = group->item_indices[index];

        transport_items[index].packet              = items[item_index].packet;
        transport_items[index].type                = items[item_index].type;
        transport_items[index].destination_node_id = items[item_index].destination_node_id;
        transport_results[index]                   = -EINPROGRESS;
    }

    ret = linkg_transport_send_batch(&transport_context, transport_items, group->count, transport_results);

    // Transport同步返回后释放Scheduler持有的全部Path引用。
    _linkg_scheduler_release_targets(targets, target_count);

    if (ret < 0)
    {
        _linkg_scheduler_set_tx_group_result(items, group, ret);
        return ret;
    }

    success_count = 0U;

    for (index = 0U; index < group->count; index++)
    {
        item_index               = group->item_indices[index];
        items[item_index].result = transport_results[index];

        if (transport_results[index] == 0)
        {
            success_count++;
        }
    }

    return (int)success_count;
}

/**
 * @brief 将一个最多32个Packet的内部Chunk按照当前直接Peer分组并完成调度。
 *
 * @note 本函数只消费Batch级已经准备好的本机角色和STA直接Peer信息，不重复查询Node状态。
 */
static int _linkg_scheduler_submit_tx_chunk(const linkg_scheduler_tx_context_t *context, const linkg_scheduler_route_context_t *route_context, linkg_scheduler_tx_item_t *items, uint32_t count)
{
    linkg_scheduler_peer_group_t groups[LINKG_SCHEDULER_PEER_GROUP_MAX];
    uint32_t                     success_count;
    uint32_t                     group_count;
    uint32_t                     group_index;
    uint32_t                     index;
    uint8_t                      peer_node_id;
    int                          ret;

    if (context == NULL ||
        route_context == NULL ||
        route_context->local == NULL ||
        items == NULL ||
        count == 0U ||
        count > LINKG_SCHEDULER_TX_CHUNK_MAX)
    {
        return -EINVAL;
    }

    memset(groups, 0, sizeof(groups));

    group_count = 0U;

    for (index = 0U; index < count; index++)
    {
        items[index].result = -EINPROGRESS;

        if (items[index].packet == NULL ||
            !_linkg_scheduler_type_valid(items[index].type) ||
            !_linkg_scheduler_node_id_valid(items[index].destination_node_id))
        {
            items[index].result = -EINVAL;
            continue;
        }
        if (route_context->local->role == LINKG_DEVICE_ROLE_STA &&
            route_context->sta_peer_result != 0)
        {
            items[index].result = route_context->sta_peer_result;
            continue;
        }

        ret = _linkg_scheduler_resolve_peer(route_context->local,
                                            items[index].destination_node_id,
                                            route_context->sta_peer_node_id,
                                            &peer_node_id);
        if (ret != 0)
        {
            items[index].result = ret;
            continue;
        }

        for (group_index = 0U; group_index < group_count; group_index++)
        {
            if (groups[group_index].peer_node_id == peer_node_id)
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

            groups[group_index].peer_node_id = peer_node_id;
            groups[group_index].count        = 0U;
            group_count++;
        }

        groups[group_index].item_indices[groups[group_index].count] = index;
        groups[group_index].count++;
    }

    success_count = 0U;

    for (group_index = 0U; group_index < group_count; group_index++)
    {
        ret = _linkg_scheduler_submit_tx_group(context, items, &groups[group_index]);
        if (ret < 0)
        {
            _linkg_scheduler_set_tx_group_result(items, &groups[group_index], ret);
            continue;
        }

        success_count += (uint32_t)ret;
    }

    return (int)success_count;
}

/****************************** 中继发送 ******************************/

/**
 * @brief 调度同一直接Peer的一批Transport中继Wire Frame。
 */
static int _linkg_scheduler_submit_forward_group(linkg_transport_class_t traffic_class, const linkg_transport_forward_item_t *items, int *results, const linkg_scheduler_peer_group_t *group)
{
    int                              transport_results[LINKG_SCHEDULER_TX_CHUNK_MAX];
    linkg_transport_tx_target_t      targets[LINKG_TRANSPORT_TX_TARGET_MAX];
    linkg_transport_tx_context_t     transport_context;
    linkg_transport_forward_item_t   transport_items[LINKG_SCHEDULER_TX_CHUNK_MAX];
    uint32_t                         target_count;
    uint32_t                         success_count;
    uint32_t                         item_index;
    uint32_t                         index;
    int                              ret;

    if (!_linkg_scheduler_class_valid(traffic_class) ||
        items == NULL ||
        results == NULL ||
        group == NULL ||
        group->count == 0U ||
        group->count > LINKG_SCHEDULER_TX_CHUNK_MAX)
    {
        return -EINVAL;
    }

    ret = _linkg_scheduler_build_targets(group->peer_node_id,
                                         LINKG_SCHEDULER_POLICY_DEFAULT,
                                         LINKG_LINK_ID_INVALID,
                                         targets,
                                         &target_count);
    if (ret != 0)
    {
        _linkg_scheduler_set_forward_group_result(results, group, ret);
        return 0;
    }

    memset(&transport_context, 0, sizeof(transport_context));
    memset(transport_items, 0, sizeof(transport_items));

    transport_context.targets       = targets;
    transport_context.target_count  = target_count;
    transport_context.traffic_class = traffic_class;
    transport_context.peer_node_id  = group->peer_node_id;

    for (index = 0U; index < group->count; index++)
    {
        item_index = group->item_indices[index];

        transport_items[index]   = items[item_index];
        transport_results[index] = -EINPROGRESS;
    }

    ret = linkg_transport_forward_batch(&transport_context, transport_items, group->count, transport_results);

    // Transport同步返回后释放Scheduler持有的全部Path引用。
    _linkg_scheduler_release_targets(targets, target_count);

    if (ret < 0)
    {
        _linkg_scheduler_set_forward_group_result(results, group, ret);
        return ret;
    }

    success_count = 0U;

    for (index = 0U; index < group->count; index++)
    {
        item_index          = group->item_indices[index];
        results[item_index] = transport_results[index];

        if (transport_results[index] == 0)
        {
            success_count++;
        }
    }

    return (int)success_count;
}

/**
 * @brief 处理Transport提交的中继批次并按照最终目的重新确定下一跳发送计划。
 *
 * @note Transport仅在当前同步回调期间借用items中的Packet引用，Scheduler不得异步保存这些指针。
 */
static int _linkg_scheduler_transport_forward(linkg_transport_class_t traffic_class, const linkg_transport_forward_item_t *items, uint32_t count, int *results, void *user_data)
{
    linkg_scheduler_peer_group_t groups[LINKG_SCHEDULER_PEER_GROUP_MAX];
    const linkg_node_info_t     *local;
    uint32_t                     success_count;
    uint32_t                     group_count;
    uint32_t                     group_index;
    uint32_t                     index;
    uint8_t                      peer_node_id;
    int                          ret;

    (void)user_data;

    if (!atomic_load(&g_scheduler_initialized))
    {
        return -ESHUTDOWN;
    }
    if (!_linkg_scheduler_class_valid(traffic_class) ||
        items == NULL ||
        results == NULL ||
        count == 0U)
    {
        return -EINVAL;
    }
    if (count > LINKG_SCHEDULER_TX_CHUNK_MAX)
    {
        for (index = 0U; index < count; index++)
        {
            results[index] = -EOVERFLOW;
        }

        return -EOVERFLOW;
    }

    local = linkg_node_get_local();
    if (local == NULL)
    {
        for (index = 0U; index < count; index++)
        {
            results[index] = -ENODEV;
        }

        return -ENODEV;
    }
    if (local->role != LINKG_DEVICE_ROLE_AP)
    {
        for (index = 0U; index < count; index++)
        {
            results[index] = -EOPNOTSUPP;
        }

        return -EOPNOTSUPP;
    }

    memset(groups, 0, sizeof(groups));

    group_count = 0U;

    for (index = 0U; index < count; index++)
    {
        results[index] = -EINPROGRESS;

        if (items[index].packet == NULL ||
            items[index].payload_length == 0U ||
            !_linkg_scheduler_node_id_valid(items[index].destination_node_id) ||
            !_linkg_scheduler_node_id_valid(items[index].peer_node_id))
        {
            results[index] = -EINVAL;
            continue;
        }

        ret = _linkg_scheduler_resolve_peer(local,
                                            items[index].destination_node_id,
                                            0U,
                                            &peer_node_id);
        if (ret != 0)
        {
            results[index] = ret;
            continue;
        }

        // 禁止中继Packet原路回送到当前物理上一跳，避免异常目的地址形成U-turn。
        if (peer_node_id == items[index].peer_node_id)
        {
            results[index] = -ELOOP;
            continue;
        }

        for (group_index = 0U; group_index < group_count; group_index++)
        {
            if (groups[group_index].peer_node_id == peer_node_id)
            {
                break;
            }
        }

        if (group_index == group_count)
        {
            if (group_count >= LINKG_SCHEDULER_PEER_GROUP_MAX)
            {
                results[index] = -ENOSPC;
                continue;
            }

            groups[group_index].peer_node_id = peer_node_id;
            groups[group_index].count        = 0U;
            group_count++;
        }

        groups[group_index].item_indices[groups[group_index].count] = index;
        groups[group_index].count++;
    }

    success_count = 0U;

    for (group_index = 0U; group_index < group_count; group_index++)
    {
        ret = _linkg_scheduler_submit_forward_group(traffic_class,
                                                    items,
                                                    results,
                                                    &groups[group_index]);
        if (ret < 0)
        {
            _linkg_scheduler_set_forward_group_result(results, &groups[group_index], ret);
            continue;
        }

        success_count += (uint32_t)ret;
    }

    return (int)success_count;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化发送调度模块并注册Transport中继回调。
 *
 * @note Transport必须已经完成初始化。
 */
int linkg_scheduler_init(void)
{
    bool expected;
    int  ret;

    expected = false;

    if (!atomic_compare_exchange_strong(&g_scheduler_initialized, &expected, true))
    {
        return -EALREADY;
    }

    atomic_store(&g_scheduler_sta_peer_cached, false);
    atomic_store(&g_scheduler_sta_peer_node_id, 0U);

    ret = linkg_transport_register_forward_handler(_linkg_scheduler_transport_forward, NULL);
    if (ret != 0)
    {
        atomic_store(&g_scheduler_initialized, false);
        return ret;
    }

    return 0;
}

/**
 * @brief 反初始化发送调度模块并注销Transport中继回调。
 *
 * @note 调用前必须停止所有可能进入Scheduler和Transport RX中继路径的数据面线程。
 */
int linkg_scheduler_deinit(void)
{
    int ret;

    if (!atomic_load(&g_scheduler_initialized))
    {
        return 0;
    }

    atomic_store(&g_scheduler_initialized, false);

    ret = linkg_transport_unregister_forward_handler();
    if (ret != 0 && ret != -ENOENT)
    {
        atomic_store(&g_scheduler_initialized, true);
        return ret;
    }

    atomic_store(&g_scheduler_sta_peer_cached, false);
    atomic_store(&g_scheduler_sta_peer_node_id, 0U);

    return 0;
}

/****************************** 数据发送 ******************************/

/**
 * @brief 批量调度上层已经确定业务类别的Transport逻辑Packet。
 *
 * @note Scheduler不接管调用方持有的Packet原始引用；一个调用只对应一个traffic_class。
 *       本机角色和STA唯一AP Peer只在大Batch开始时解析一次。
 *       调用方可以提交任意数量Packet，Scheduler固定按最多32个Packet拆分内部Chunk。
 *       每个Chunk再根据最终destination_node_id解析直接Peer并按Peer分组，同一Peer组同步调用一次Transport。
 *       Scheduler不在内部缓存Packet，所有Chunk均在本次调用返回前完成同步调度。
 *
 * @return 小于0表示全部Chunk均未正常执行；
 *         大于等于0表示成功完整提交的原始逻辑Packet总数量。
 */
int linkg_scheduler_submit_batch(const linkg_scheduler_tx_context_t *context, linkg_scheduler_tx_item_t *items, uint32_t count)
{
    linkg_scheduler_route_context_t route_context;
    uint32_t                        success_count;
    uint32_t                        chunk_count;
    uint32_t                        offset;
    uint32_t                        index;
    int                             first_error;
    int                             ret;
    bool                            processed;

    if (!atomic_load(&g_scheduler_initialized))
    {
        return -ENODEV;
    }

    ret = _linkg_scheduler_validate_context(context);
    if (ret != 0)
    {
        return ret;
    }
    if (items == NULL || count == 0U)
    {
        return -EINVAL;
    }

    ret = _linkg_scheduler_prepare_route_context(&route_context);
    if (ret != 0)
    {
        for (index = 0U; index < count; index++)
        {
            items[index].result = ret;
        }

        return ret;
    }

    success_count = 0U;
    first_error   = 0;
    processed     = false;
    offset        = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;
        if (chunk_count > LINKG_SCHEDULER_TX_CHUNK_MAX)
        {
            chunk_count = LINKG_SCHEDULER_TX_CHUNK_MAX;
        }

        ret = _linkg_scheduler_submit_tx_chunk(context, &route_context, &items[offset], chunk_count);
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

    // 所有Chunk都发生系统级错误时，整个大批次视为未正常执行。
    if (!processed)
    {
        return first_error;
    }

    return (int)success_count;
}

/**
 * @brief 调度单个上层已经确定业务类别的Transport逻辑Packet。
 */
int linkg_scheduler_submit(const linkg_scheduler_tx_context_t *context, linkg_scheduler_tx_item_t *item)
{
    int ret;

    if (item == NULL)
    {
        return -EINVAL;
    }

    ret = linkg_scheduler_submit_batch(context, item, 1U);
    if (ret < 0)
    {
        return ret;
    }

    return item->result;
}

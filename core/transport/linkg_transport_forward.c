/**
 * @file linkg_transport_forward.c
 * @brief LinkG AP同步批量转发实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-29
 */

#include "linkg_transport_internal.h"

#include <errno.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_scheduler.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    uint32_t sequence;     // 新一跳Transport序列号
    int      result;       // 当前转发结果
    uint8_t  peer_node_id; // 当前直接Peer节点编号
    bool     prepared;     // Peer和Sequence是否准备完成
} linkg_transport_forward_state_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验节点编号。
 */
static bool _linkg_transport_forward_node_id_valid(uint8_t node_id)
{
    return node_id >= LINKG_RESOURCE_NODE_ID_MIN && node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 校验同步转发批次参数。
 */
static int _linkg_transport_forward_validate_batch(const linkg_transport_forward_item_t *items, uint32_t count)
{
    uint32_t index;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (items == NULL || count == 0U || count > LINKG_TRANSPORT_FORWARD_BATCH_MAX)
    {
        return -EINVAL;
    }

    if (g_transport.local_role != LINKG_DEVICE_ROLE_AP)
    {
        return -EPERM;
    }

    for (index = 0U; index < count; index++)
    {
        if (items[index].packet == NULL)
        {
            return -EINVAL;
        }

        if (!_linkg_transport_forward_node_id_valid(items[index].destination_node_id))
        {
            return -EINVAL;
        }

        if (items[index].destination_node_id == g_transport.local_node_id)
        {
            return -EINVAL;
        }

        if (!linkg_transport_type_valid(items[index].type))
        {
            return -EINVAL;
        }

        if (items[index].payload_length == 0U)
        {
            return -EINVAL;
        }
    }

    return 0;
}

/**
 * @brief 为AP转发批次确定直接Peer并分配新的逐跳序列号。
 *
 * 整个批次只获取一次g_transport.lock，释放状态锁后再进入Scheduler发送路径。
 */
static int _linkg_transport_forward_prepare_batch(const linkg_transport_forward_item_t *items, linkg_transport_forward_state_t *states, uint32_t count)
{
    linkg_transport_peer_t *cached_peer;
    linkg_transport_peer_t *peer;
    uint32_t                index;
    uint8_t                 cached_destination_node_id;
    bool                    cache_valid;
    int                     ret;

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    cached_peer                = NULL;
    cached_destination_node_id = LINKG_RESOURCE_NODE_ID_INVALID;
    cache_valid                = false;

    for (index = 0U; index < count; index++)
    {
        states[index].result   = -ENOENT;
        states[index].prepared = false;

        if (cache_valid && items[index].destination_node_id == cached_destination_node_id)
        {
            peer = cached_peer;
        }
        else
        {
            peer                       = linkg_transport_find_peer_locked(items[index].destination_node_id);
            cached_destination_node_id = items[index].destination_node_id;
            cached_peer                = peer;
            cache_valid                = true;
        }

        if (peer == NULL)
        {
            continue;
        }

        peer->tx_sequence++;

        states[index].sequence     = peer->tx_sequence;
        states[index].peer_node_id = peer->peer_node_id;
        states[index].result       = -EINPROGRESS;
        states[index].prepared     = true;
    }

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 批量记录AP同步转发TX统计。
 *
 * RX统计已经在Transport接收窗口阶段完成，此处只记录转发产生的新一跳TX结果。
 */
static void _linkg_transport_forward_record_batch(const linkg_transport_forward_item_t *items, const linkg_transport_forward_state_t *states, uint32_t count)
{
    linkg_transport_type_stats_t *type_stats;
    linkg_transport_peer_t       *cached_peer;
    linkg_transport_peer_t       *peer;
    uint64_t                      now_ms;
    uint32_t                      index;
    uint8_t                       cached_peer_node_id;
    bool                          cache_valid;
    bool                          now_valid;

    cached_peer         = NULL;
    cached_peer_node_id = LINKG_RESOURCE_NODE_ID_INVALID;
    cache_valid         = false;
    now_ms              = 0U;
    now_valid           = false;

    pthread_mutex_lock(&g_transport.lock);

    for (index = 0U; index < count; index++)
    {
        if (!states[index].prepared)
        {
            continue;
        }

        if (cache_valid && states[index].peer_node_id == cached_peer_node_id)
        {
            peer = cached_peer;
        }
        else
        {
            peer                = linkg_transport_find_peer_locked(states[index].peer_node_id);
            cached_peer_node_id = states[index].peer_node_id;
            cached_peer         = peer;
            cache_valid         = true;
        }

        if (peer == NULL)
        {
            continue;
        }

        type_stats = &peer->stats.types[items[index].type];

        if (states[index].result == 0)
        {
            type_stats->tx_packets++;
            type_stats->tx_bytes += items[index].payload_length;

            if (!now_valid)
            {
                now_ms    = linkg_time_elapsed_ms();
                now_valid = true;
            }

            peer->stats.last_tx_ms = now_ms;
        }
        else
        {
            type_stats->tx_failed_packets++;
            type_stats->tx_failed_bytes += items[index].payload_length;
        }
    }

    pthread_mutex_unlock(&g_transport.lock);
}

/****************************** 转发处理 ******************************/

/**
 * @brief 同步批量转发AP收到的非本机目标Transport帧。
 *
 * 当前函数同步借用Link RX持有的Packet引用，不retain且不跨线程保存。
 * 转发保持原始Transport端到端身份、packet_id和分片边界，只更新当前新一跳sequence。
 */
int linkg_transport_forward_batch(const linkg_transport_forward_item_t *items, uint32_t count)
{
    linkg_scheduler_tx_item_t        scheduler_items[LINKG_TRANSPORT_FORWARD_BATCH_MAX];
    linkg_transport_forward_state_t states[LINKG_TRANSPORT_FORWARD_BATCH_MAX];
    uint32_t                        scheduler_indices[LINKG_TRANSPORT_FORWARD_BATCH_MAX];
    uint32_t                        scheduler_count;
    uint32_t                        item_index;
    uint32_t                        index;
    int                             ret;

    ret = _linkg_transport_forward_validate_batch(items, count);
    if (ret != 0)
    {
        return ret;
    }

    memset(states, 0, sizeof(states));
    memset(scheduler_items, 0, sizeof(scheduler_items));

    ret = _linkg_transport_forward_prepare_batch(items, states, count);
    if (ret != 0)
    {
        return ret;
    }

    scheduler_count = 0U;

    for (index = 0U; index < count; index++)
    {
        if (!states[index].prepared)
        {
            continue;
        }

        ret = linkg_transport_wire_update_sequence(items[index].packet, states[index].sequence);
        if (ret != 0)
        {
            states[index].result = ret;
            continue;
        }

        scheduler_items[scheduler_count].packet           = items[index].packet;
        scheduler_items[scheduler_count].next_hop_node_id = states[index].peer_node_id;
        scheduler_items[scheduler_count].result           = -EINPROGRESS;

        scheduler_indices[scheduler_count] = index;
        scheduler_count++;
    }

    if (scheduler_count > 0U)
    {
        ret = linkg_scheduler_submit_batch(scheduler_items, scheduler_count, LINKG_SCHEDULER_POLICY_DEFAULT, LINKG_LINK_ID_INVALID);

        if (ret < 0)
        {
            for (index = 0U; index < scheduler_count; index++)
            {
                item_index = scheduler_indices[index];
                states[item_index].result = ret;
            }
        }
        else
        {
            for (index = 0U; index < scheduler_count; index++)
            {
                item_index = scheduler_indices[index];

                states[item_index].result = scheduler_items[index].result;

                if (states[item_index].result == -EINPROGRESS)
                {
                    LINKG_LOG_ERROR("transport forward scheduler result not completed, index=%u", index);
                    states[item_index].result = -EIO;
                }
            }
        }
    }

    _linkg_transport_forward_record_batch(items, states, count);

    return 0;
}

/**
 * @file path_probe_rx.c
 * @brief LinkG Path Probe Transport本机交付回调实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-16
 */

#include "path_probe_internal.h"

#include <errno.h>

#include "linkg_time.h"

/****************************** 接收校验 ******************************/

/**
 * @brief 仅接受可明确归属入站Link的直接Peer报文。
 */
static bool _linkg_path_probe_delivery_valid(const linkg_transport_delivery_t *item, uint8_t local_node_id)
{
    if (item == NULL || item->packet == NULL)
    {
        return false;
    }

    if (item->ingress_link_id == LINKG_LINK_ID_INVALID || item->peer_node_id == local_node_id || item->source_node_id != item->peer_node_id)
    {
        return false;
    }

    if (item->peer_node_id < LINKG_RESOURCE_NODE_ID_MIN || item->peer_node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return false;
    }

    return item->traffic_class >= LINKG_TRANSPORT_CLASS_REALTIME && item->traffic_class < LINKG_TRANSPORT_CLASS_COUNT;
}

/****************************** Transport接收 ******************************/

/**
 * @brief 处理Probe批次，REQUEST直接回复，RESPONSE仅完成本机在途记录。
 *
 * Transport同步借用所有Packet，本回调不修改、释放或长期保存它们。
 * 迟到、重复、错误来源和损坏Probe静默丢弃，避免异常小包引发日志风暴。
 */
int linkg_path_probe_transport_receive(const linkg_transport_delivery_t *items, uint32_t count, void *user_data)
{
    linkg_path_probe_wire_t  wire;
    linkg_packet_pool_t     *pool;
    linkg_device_role_t      role;
    uint64_t                 now_us;
    uint32_t                 index;
    uint8_t                  local_node_id;

    if (items == NULL && count != 0U)
    {
        return -EINVAL;
    }

    if (count == 0U)
    {
        return 0;
    }

    if (!linkg_path_probe_enter_receive(user_data, &pool, &role, &local_node_id))
    {
        return 0;
    }

    for (index = 0U; index < count; index++)
    {
        now_us = linkg_time_monotonic_us();

        if (!_linkg_path_probe_delivery_valid(&items[index], local_node_id))
        {
            continue;
        }

        if (linkg_path_probe_wire_decode(items[index].packet, &wire) != 0)
        {
            continue;
        }

        pthread_mutex_lock(&g_path_probe.lock);

        if (g_path_probe.running)
        {
            linkg_path_probe_record_rx_locked(items[index].peer_node_id, items[index].ingress_link_id);
        }

        pthread_mutex_unlock(&g_path_probe.lock);

        if (wire.type == LINKG_PATH_PROBE_WIRE_TYPE_REQUEST)
        {
            // 不持有Probe锁；只提交一个原路响应，不执行诊断或等待。
            (void)linkg_path_probe_send_response(pool, role, local_node_id, items[index].peer_node_id, items[index].ingress_link_id, items[index].traffic_class, wire.sequence);
        }
        else
        {
            pthread_mutex_lock(&g_path_probe.lock);

            if (g_path_probe.running)
            {
                linkg_path_probe_accept_reply_locked(items[index].peer_node_id, items[index].ingress_link_id, items[index].traffic_class, wire.sequence, now_us);
            }

            pthread_mutex_unlock(&g_path_probe.lock);
        }
    }

    linkg_path_probe_leave_receive();

    return 0;
}

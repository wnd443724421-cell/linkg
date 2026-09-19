/**
 * @file switch_tx.c
 * @brief LinkG链路切换控制消息发送实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-09-19
 */

#include "switch_tx.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_packet_pool.h"
#include "linkg_scheduler.h"
#include "linkg_system_resources.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断目标Peer节点编号是否合法。
 */
static bool _linkg_switch_tx_peer_valid(uint8_t peer_node_id)
{
    return peer_node_id >= LINKG_RESOURCE_NODE_ID_MIN && peer_node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 从外部Packet Pool申请一个空Switch控制Packet。
 *
 * 成功后调用方持有Packet的一个基础引用，最终必须释放。
 */
static int _linkg_switch_tx_create_packet(linkg_packet_pool_t *pool, linkg_packet_t **out)
{
    linkg_packet_t *packet;

    if (out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    if (pool == NULL || !pool->initialized)
    {
        return -ENODEV;
    }

    packet = linkg_packet_pool_alloc(pool);
    if (packet == NULL)
    {
        return -ENOMEM;
    }

    if (linkg_packet_capacity(packet) < LINKG_SWITCH_WIRE_MAX_SIZE)
    {
        linkg_packet_release(packet);
        return -ENOSPC;
    }

    *out = packet;

    return 0;
}

/**
 * @brief 将已编码Switch Wire作为REALTIME控制Packet冗余提交给Scheduler。
 *
 * Scheduler同步借用Packet，不接管调用方持有的基础引用。
 */
static int _linkg_switch_tx_submit(uint8_t peer_node_id, linkg_packet_t *packet, size_t wire_length)
{
    linkg_scheduler_tx_context_t context;
    linkg_scheduler_tx_item_t    item;

    if (!_linkg_switch_tx_peer_valid(peer_node_id) || packet == NULL)
    {
        return -EINVAL;
    }

    if (wire_length == 0U || wire_length > linkg_packet_capacity(packet) || wire_length > UINT32_MAX)
    {
        return -EMSGSIZE;
    }

    packet->data_length = (uint32_t)wire_length;
    linkg_packet_set_realtime(packet, true);

    memset(&context, 0, sizeof(context));
    memset(&item, 0, sizeof(item));

    context.traffic_class = LINKG_TRANSPORT_CLASS_REALTIME;
    context.policy        = LINKG_SCHEDULER_POLICY_REDUNDANT;

    item.packet              = packet;
    item.type                = LINKG_TRANSPORT_TYPE_SWITCH;
    item.destination_node_id = peer_node_id;
    item.result              = -EINPROGRESS;

    // 单包submit返回0才表示当前Switch控制Packet提交成功。
    return linkg_scheduler_submit(&context, &item);
}

/****************************** 数据发送 ******************************/

/**
 * @brief 向指定STA发送Wi-Fi上行质量报告。
 */
int linkg_switch_tx_send_wifi_quality_report(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_wifi_quality_report_t *report)
{
    linkg_packet_t *packet;
    size_t          wire_length;
    int             ret;

    if (report == NULL)
    {
        return -EINVAL;
    }

    packet      = NULL;
    wire_length = 0U;

    ret = _linkg_switch_tx_create_packet(pool, &packet);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_switch_wire_encode_wifi_quality_report(message_id,
                                                       report,
                                                       linkg_packet_data(packet),
                                                       linkg_packet_capacity(packet),
                                                       &wire_length);
    if (ret == 0)
    {
        ret = _linkg_switch_tx_submit(peer_node_id, packet, wire_length);
    }

    linkg_packet_release(packet);

    return ret;
}

/**
 * @brief 向AP发送STA当前发送计划同步消息。
 */
int linkg_switch_tx_send_plan_sync(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_plan_sync_t *plan)
{
    linkg_packet_t *packet;
    size_t          wire_length;
    int             ret;

    if (plan == NULL)
    {
        return -EINVAL;
    }

    packet      = NULL;
    wire_length = 0U;

    ret = _linkg_switch_tx_create_packet(pool, &packet);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_switch_wire_encode_plan_sync(message_id,
                                             plan,
                                             linkg_packet_data(packet),
                                             linkg_packet_capacity(packet),
                                             &wire_length);
    if (ret == 0)
    {
        ret = _linkg_switch_tx_submit(peer_node_id, packet, wire_length);
    }

    linkg_packet_release(packet);

    return ret;
}

/**
 * @brief 向STA发送AP侧发送计划同步确认消息。
 */
int linkg_switch_tx_send_plan_ack(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_plan_ack_t *ack)
{
    linkg_packet_t *packet;
    size_t          wire_length;
    int             ret;

    if (ack == NULL)
    {
        return -EINVAL;
    }

    packet      = NULL;
    wire_length = 0U;

    ret = _linkg_switch_tx_create_packet(pool, &packet);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_switch_wire_encode_plan_ack(message_id,
                                            ack,
                                            linkg_packet_data(packet),
                                            linkg_packet_capacity(packet),
                                            &wire_length);
    if (ret == 0)
    {
        ret = _linkg_switch_tx_submit(peer_node_id, packet, wire_length);
    }

    linkg_packet_release(packet);

    return ret;
}

/**
 * @brief 向指定直接Peer发送Maintenance开始或结束通知。
 */
int linkg_switch_tx_send_maintenance(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_maintenance_t *maintenance)
{
    linkg_packet_t *packet;
    size_t          wire_length;
    int             ret;

    if (maintenance == NULL)
    {
        return -EINVAL;
    }

    packet      = NULL;
    wire_length = 0U;

    ret = _linkg_switch_tx_create_packet(pool, &packet);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_switch_wire_encode_maintenance(message_id,
                                               maintenance,
                                               linkg_packet_data(packet),
                                               linkg_packet_capacity(packet),
                                               &wire_length);
    if (ret == 0)
    {
        ret = _linkg_switch_tx_submit(peer_node_id, packet, wire_length);
    }

    linkg_packet_release(packet);

    return ret;
}

/**
 * @brief 向指定直接Peer发送Maintenance END处理确认。
 */
int linkg_switch_tx_send_maintenance_ack(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_maintenance_ack_t *ack)
{
    linkg_packet_t *packet;
    size_t          wire_length;
    int             ret;

    if (ack == NULL)
    {
        return -EINVAL;
    }

    packet      = NULL;
    wire_length = 0U;

    ret = _linkg_switch_tx_create_packet(pool, &packet);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_switch_wire_encode_maintenance_ack(message_id,
                                                   ack,
                                                   linkg_packet_data(packet),
                                                   linkg_packet_capacity(packet),
                                                   &wire_length);
    if (ret == 0)
    {
        ret = _linkg_switch_tx_submit(peer_node_id, packet, wire_length);
    }

    linkg_packet_release(packet);

    return ret;
}

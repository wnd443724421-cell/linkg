/**
 * @file switch_rx.c
 * @brief LinkG链路切换Transport接收实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-18
 */

#include "switch_rx.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"
#include "linkg_packet_pool.h"
#include "linkg_system_resources.h"

#include "switch_event.h"
#include "switch_internal.h"
#include "switch_report.h"
#include "switch_wire.h"

/****************************** 接收校验 ******************************/

/**
 * @brief 判断Switch接收处理中的状态变化错误是否属于正常并发情况。
 */
static bool _linkg_switch_rx_expected_state_error(int error)
{
    return error == -ENOENT ||
           error == -ENODEV ||
           error == -ESHUTDOWN;
}

/**
 * @brief 记录当前批次第一个非预期处理错误。
 */
static void _linkg_switch_rx_record_error(int error, int *first_error)
{
    if (first_error == NULL || error == 0)
    {
        return;
    }

    if (_linkg_switch_rx_expected_state_error(error))
    {
        return;
    }

    if (*first_error == 0)
    {
        *first_error = error;
    }
}

/**
 * @brief 校验Switch消息必须来自有效直接Peer并使用REALTIME业务类别。
 */
static bool _linkg_switch_rx_delivery_valid(const linkg_transport_delivery_t *item, uint8_t local_node_id)
{
    if (item == NULL || item->packet == NULL)
    {
        return false;
    }

    if (item->ingress_link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    if (item->peer_node_id == local_node_id)
    {
        return false;
    }

    if (item->source_node_id != item->peer_node_id)
    {
        return false;
    }

    if (item->peer_node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        item->peer_node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return false;
    }

    return item->traffic_class == LINKG_TRANSPORT_CLASS_REALTIME;
}

/****************************** 消息分发 ******************************/

/**
 * @brief 将AP Wi-Fi质量报告直接交给Report模块更新STA运行状态。
 */
static int _linkg_switch_rx_dispatch_wifi_quality(linkg_device_role_t role, uint8_t peer_node_id, const linkg_switch_wire_message_t *message)
{
    if (role != LINKG_DEVICE_ROLE_STA)
    {
        return 0;
    }

    return linkg_switch_report_receive_wifi_quality(peer_node_id, message->header.message_id, &message->payload.wifi_quality_report);
}

/**
 * @brief 将STA发送计划同步转换为AP Worker内部事件。
 */
static int _linkg_switch_rx_dispatch_plan_sync(linkg_device_role_t role, uint8_t peer_node_id, const linkg_switch_wire_message_t *message)
{
    linkg_switch_event_t event;

    if (role != LINKG_DEVICE_ROLE_AP)
    {
        return 0;
    }

    memset(&event, 0, sizeof(event));

    event.type              = LINKG_SWITCH_EVENT_PLAN_SYNC_RX;
    event.peer_node_id      = peer_node_id;
    event.message_id        = message->header.message_id;
    event.payload.plan_sync = message->payload.plan_sync;

    return linkg_switch_event_post(&event);
}

/**
 * @brief 将AP发送计划确认转换为STA Worker内部事件。
 */
static int _linkg_switch_rx_dispatch_plan_ack(linkg_device_role_t role, uint8_t peer_node_id, const linkg_switch_wire_message_t *message)
{
    linkg_switch_event_t event;

    if (role != LINKG_DEVICE_ROLE_STA)
    {
        return 0;
    }

    memset(&event, 0, sizeof(event));

    event.type             = LINKG_SWITCH_EVENT_PLAN_ACK_RX;
    event.peer_node_id     = peer_node_id;
    event.message_id       = message->header.message_id;
    event.payload.plan_ack = message->payload.plan_ack;

    return linkg_switch_event_post(&event);
}

/**
 * @brief 按Switch Wire消息类型分发到对应内部模块。
 */
static int _linkg_switch_rx_dispatch(linkg_device_role_t role, uint8_t peer_node_id, const linkg_switch_wire_message_t *message)
{
    if (message == NULL)
    {
        return -EINVAL;
    }

    switch (message->header.type)
    {
        case LINKG_SWITCH_WIRE_TYPE_WIFI_QUALITY_REPORT:
        {
            return _linkg_switch_rx_dispatch_wifi_quality(role, peer_node_id, message);
        }

        case LINKG_SWITCH_WIRE_TYPE_PLAN_SYNC:
        {
            return _linkg_switch_rx_dispatch_plan_sync(role, peer_node_id, message);
        }

        case LINKG_SWITCH_WIRE_TYPE_PLAN_ACK:
        {
            return _linkg_switch_rx_dispatch_plan_ack(role, peer_node_id, message);
        }

        default:
        {
            return -EPROTONOSUPPORT;
        }
    }
}

/****************************** Transport接收 ******************************/

/**
 * @brief 处理Transport交付的Switch控制消息批次。
 *
 * Transport同步借用所有Packet，本回调不修改、释放或长期保存Packet。
 * Quality Report直接更新轻量运行状态，Plan Sync和Plan Ack仅复制为内部事件，
 * 后续发送计划事务统一由Switch Worker处理。
 */
int linkg_switch_transport_receive(const linkg_transport_delivery_t *items, uint32_t count, void *user_data)
{
    linkg_switch_wire_message_t message;
    linkg_device_role_t         role;
    const uint8_t              *data;
    uint32_t                    index;
    uint8_t                     local_node_id;
    int                         first_error;
    int                         ret;

    if (items == NULL && count != 0U)
    {
        return -EINVAL;
    }

    if (count == 0U)
    {
        return 0;
    }

    if (!linkg_switch_enter_receive(user_data, &role, &local_node_id))
    {
        return 0;
    }

    first_error = 0;

    for (index = 0U; index < count; index++)
    {
        if (!_linkg_switch_rx_delivery_valid(&items[index], local_node_id))
        {
            continue;
        }

        data = linkg_packet_const_data(items[index].packet);
        if (data == NULL)
        {
            continue;
        }

        ret = linkg_switch_wire_decode(data, items[index].packet->data_length, &message);
        if (ret != 0)
        {
            // 损坏、旧版本及非法Switch控制消息静默丢弃。
            continue;
        }

        ret = _linkg_switch_rx_dispatch(role, items[index].peer_node_id, &message);

        _linkg_switch_rx_record_error(ret, &first_error);
    }

    linkg_switch_leave_receive();

    return first_error;
}

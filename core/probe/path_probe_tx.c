/**
 * @file path_probe_tx.c
 * @brief LinkG Path Probe指定链路发送实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-16
 */

#include "path_probe_internal.h"

#include <errno.h>
#include <string.h>

#include "linkg_scheduler.h"
#include "linkg_time.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 申请新的Probe Packet并按真实业务类别设置标志。
 */
static int _linkg_path_probe_create_packet(linkg_packet_pool_t *pool, linkg_transport_class_t traffic_class, linkg_path_probe_wire_type_t type, uint32_t sequence, linkg_packet_t **out)
{
    linkg_packet_t *packet;
    int             ret;

    if (out == NULL || pool == NULL || traffic_class < LINKG_TRANSPORT_CLASS_REALTIME || traffic_class >= LINKG_TRANSPORT_CLASS_COUNT)
    {
        return -EINVAL;
    }

    *out = NULL;
    packet = linkg_packet_pool_alloc(pool);
    if (packet == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_path_probe_wire_encode(packet, type, sequence);
    if (ret != 0)
    {
        linkg_packet_release(packet);
        return ret;
    }

    if (traffic_class == LINKG_TRANSPORT_CLASS_REALTIME)
    {
        linkg_packet_set_realtime(packet, true);
    }
    else if (traffic_class == LINKG_TRANSPORT_CLASS_VIDEO)
    {
        linkg_packet_set_video(packet, true);
    }
    else
    {
        linkg_packet_set_data(packet);
    }

    *out = packet;

    return 0;
}

/**
 * @brief 同步借用Packet交给Scheduler，强制指定Link且不读取默认发送计划。
 */
static int _linkg_path_probe_submit(uint8_t peer_node_id, uint32_t link_id, linkg_transport_class_t traffic_class, linkg_packet_t *packet)
{
    linkg_scheduler_tx_context_t context;
    linkg_scheduler_tx_item_t    item;

    memset(&context, 0, sizeof(context));
    memset(&item, 0, sizeof(item));

    context.traffic_class     = traffic_class;
    context.policy            = LINKG_SCHEDULER_POLICY_SPECIFIED;
    context.specified_link_id = link_id;

    item.packet              = packet;
    item.type                = LINKG_TRANSPORT_TYPE_PATH_PROBE;
    item.destination_node_id = peer_node_id;
    item.result              = -EINPROGRESS;

    // 单包submit返回0才表示接受成功，不能套用batch返回成功数量的语义。
    return linkg_scheduler_submit(&context, &item);
}

/****************************** 请求发送 ******************************/

/**
 * @brief 发送已预留Pending的请求，Probe锁不跨越任何Scheduler调用。
 *
 * Pending在提交前发布；提交期间收到的响应由RX暂存，提交返回后统一完成。
 * Scheduler借用Packet，返回后本函数总是释放自己持有的基础引用。
 */
int linkg_path_probe_send_request(linkg_packet_pool_t *pool, linkg_device_role_t role, uint8_t local_node_id, const linkg_path_probe_tx_task_t *task)
{
    linkg_path_endpoint_t  endpoint;
    linkg_packet_t        *packet;
    int                    ret;

    if (task == NULL)
    {
        return -EINVAL;
    }

    packet = NULL;

    ret = linkg_path_probe_read_target(role, local_node_id, task->peer_node_id, task->link_id, &endpoint);
    if (ret != 0)
    {
        goto out;
    }

    if (!linkg_path_probe_endpoint_equal(&endpoint, &task->endpoint))
    {
        ret = -ESTALE;
        goto out;
    }

    ret = _linkg_path_probe_create_packet(pool, task->traffic_class, LINKG_PATH_PROBE_WIRE_TYPE_REQUEST, task->sequence, &packet);
    if (ret != 0)
    {
        goto out;
    }

    pthread_mutex_lock(&g_path_probe.lock);
    ret = linkg_path_probe_mark_sending_locked(task, linkg_time_monotonic_us());
    pthread_mutex_unlock(&g_path_probe.lock);

    if (ret == 0)
    {
        ret = _linkg_path_probe_submit(task->peer_node_id, task->link_id, task->traffic_class, packet);
    }

out:
    if (packet != NULL)
    {
        linkg_packet_release(packet);
    }

    pthread_mutex_lock(&g_path_probe.lock);
    linkg_path_probe_finish_send_locked(task, ret, linkg_time_monotonic_us());
    pthread_mutex_unlock(&g_path_probe.lock);

    return ret;
}

/****************************** 响应发送 ******************************/

/**
 * @brief 收包线程直接原Link原Class回复，不创建Pending也不等待响应。
 *
 * 调用者通过rx_users或Worker生命周期保证pool有效；不修改借用的RX Packet。
 */
int linkg_path_probe_send_response(linkg_packet_pool_t *pool, linkg_device_role_t role, uint8_t local_node_id, uint8_t peer_node_id, uint32_t link_id, linkg_transport_class_t traffic_class, uint32_t sequence)
{
    linkg_path_endpoint_t  endpoint;
    linkg_packet_t        *packet;
    int                    ret;

    ret = linkg_path_probe_read_target(role, local_node_id, peer_node_id, link_id, &endpoint);
    if (ret != 0)
    {
        return ret;
    }

    packet = NULL;
    ret    = _linkg_path_probe_create_packet(pool, traffic_class, LINKG_PATH_PROBE_WIRE_TYPE_RESPONSE, sequence, &packet);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_path_probe_submit(peer_node_id, link_id, traffic_class, packet);

    if (ret == 0)
    {
        pthread_mutex_lock(&g_path_probe.lock);

        if (g_path_probe.running)
        {
            linkg_path_probe_record_tx_locked(peer_node_id, link_id);
        }

        pthread_mutex_unlock(&g_path_probe.lock);
    }

    linkg_packet_release(packet);

    return ret;
}

# 这是针对当前main的最小生命周期补丁说明，不覆盖你已经完成的tx_order_lock同步实现。
# 修改 core/transport/linkg_transport.c：

@@ 在内部辅助区域增加：
/**
 * @brief 同步清理指定Peer残留的本机重组和AP中继分片缓存。
 *
 * @note 调用本函数时不得持有g_transport.lock，forward pair清理可能在内部归并全局统计。
 */
static int _linkg_transport_reset_peer_caches(uint8_t peer_node_id)
{
    int ret;

    ret = linkg_transport_reassembly_reset_peer(peer_node_id);
    if (ret != 0)
    {
        return ret;
    }

    if (g_transport.local_role == LINKG_DEVICE_ROLE_AP)
    {
        ret = linkg_transport_forward_pair_reset_peer(peer_node_id);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

@@ linkg_transport_reset_peer()：
# 保留当前g_transport.lock + tx_order_lock同步逻辑不变。
# 在全部Peer/Class状态重置完成并释放g_transport.lock之后、return之前加入：

    ret = _linkg_transport_reset_peer_caches(peer_node_id);
    if (ret != 0)
    {
        return ret;
    }

    return 0;

@@ linkg_transport_unregister_peer()：
# 保留当前g_transport.lock + tx_order_lock同步逻辑不变。
# 必须先完成valid=false / peer_count--并释放g_transport.lock，随后再执行：

    ret = _linkg_transport_reset_peer_caches(peer_node_id);
    if (ret != 0)
    {
        return ret;
    }

    return 0;

# 生命周期约束：同一个peer_node_id的register/reset/unregister必须由Node控制面串行化；
# unregister返回前已同步清空旧Peer的reassembly/forward-pair长期Packet引用。

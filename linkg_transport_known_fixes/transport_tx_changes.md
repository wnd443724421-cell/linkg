# 针对GitHub main当前Transport TX的最小修改清单。
# 不替换现有固定32逻辑Packet chunk模型，只修已确认问题。

1. Wire数组全部使用64容量：

- linkg_packet_t *packets[LINKG_TRANSPORT_FRAME_BATCH_MAX];
- uint32_t        payload_lengths[LINKG_TRANSPORT_FRAME_BATCH_MAX];
+ linkg_packet_t *packets[LINKG_TRANSPORT_TX_WIRE_BATCH_MAX];
+ uint32_t        payload_lengths[LINKG_TRANSPORT_TX_WIRE_BATCH_MAX];

- int frame_results[LINKG_TRANSPORT_FRAME_BATCH_MAX];
+ int frame_results[LINKG_TRANSPORT_TX_WIRE_BATCH_MAX];

- int target_results[LINKG_TRANSPORT_FRAME_BATCH_MAX];
+ int target_results[LINKG_TRANSPORT_TX_WIRE_BATCH_MAX];

# 任何对submit Wire batch的count上限也必须是LINKG_TRANSPORT_TX_WIRE_BATCH_MAX。
# linkg_transport_tx_state_t states[]仍保持LINKG_TRANSPORT_TX_BATCH_MAX(32)。

2. public send_batch入口在指针基础检查后、任何_prepare_states/_send_chunk之前调用：

    ret = _linkg_transport_tx_validate_batch(context, items, count);
    if (ret != 0)
    {
        return ret;
    }

3. 删除完全未使用的：

    static uint32_t _linkg_transport_tx_get_chunk_count(...)

# public send_batch现有固定32逻辑Packet切分保持：
# chunk_count = min(count - offset, LINKG_TRANSPORT_TX_BATCH_MAX)。

4. 内部Wire提交函数收回文件内：

- int linkg_transport_submit_wire_batch(...)
+ static int _linkg_transport_tx_submit_wire_batch(...)

# 同文件所有调用同步改名。

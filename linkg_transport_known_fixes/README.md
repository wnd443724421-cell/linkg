# LinkG Transport known-fixes patchset

这组文件针对 2026-09-11 对 GitHub main Transport 的已知问题收口，不改 Scheduler。

## 可直接替换

- `transport_internal.h`
  - RX Window增加 `tracked_count`。
  - 保留旧 `linkg_transport_window_accept()`。
  - 新增 `linkg_transport_window_accept_ex(..., confirmed_lost)` 内部接口。
- `transport_window.c`
  - 实现最终确认丢包：Gap出现时不计loss，只有缺失sequence永久滑出512 Window时才计。
  - 已验证迟到补齐、512边界、大跨度跳号、uint32回绕。

## 针对当前main精确应用

- `transport_rx.patch`
  - RX取得本次 `confirmed_lost` 并累计到 `peer_class->stats.rx_lost_packets`。
  - 最新RX已有source group容量/重复赋值/overflow debug修正，不再重复改。
- `transport_tx_changes.md`
  - 32逻辑Packet -> 最大64 Wire Frame容量闭环。
  - 恢复public send_batch输入校验。
  - 删除旧 `_get_chunk_count()`。
  - Wire submit helper改static。
- `transport_forward_changes.md`
  - Forward public API补 `count/context/packet` 边界。
  - 保留新架构 Scheduler选Target -> Transport Forward -> Link，不覆盖旧版forward文件。
- `linkg_transport_peer_cache_changes.md`
  - reset/unregister在释放 `g_transport.lock` 后同步清理 reassembly/forward-pair 缓存。
  - 不破坏你已经实现的 `tx_order_lock` 同步。
- `linkg_transport_public_header_changes.md`
  - 移除过时的公开 `LINKG_TRANSPORT_FRAME_BATCH_MAX 32` 语义。

## 重要生命周期约束

1. Peer reset/unregister前，Node控制面应先隔离/注销该Peer的数据Path，避免旧RX在cache purge后重新写入旧epoch分片。
2. 同一peer_node_id的register/reset/unregister必须由控制面串行化。
3. Handler unregister前必须停止对应数据入口，再释放user_data。
4. Path切换（Wi-Fi <-> 5G）不能reset Transport Peer；Peer/Class sequence必须连续。

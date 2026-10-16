# LinkG Cellular 独立路径丢包统计 V1

本分支基于 main 的 2026-10-16 版本，提供完整源码，而不是补丁或替换脚本。

## 数据面变更

- Cellular IPv6/UDP 增加 8 字节私有头：session_id (4B) + sequence (4B)。
- Wi-Fi 和 Cellular 的链路丢包分别由其 Socket RX 层的 64 包序号窗口记录。
- Cellular 在 `sendmmsg()` 返回成功前缀后，才推进每个 IPv6 目的地址 / 业务类别的 Sequence。
- TX 等待队列先于 Socket 发生的队列满淘汰不消耗 Cellular Sequence。
- Cellular RX 使用两个 iovec 分离私有头和原有 Transport 帧，不污染共享 Packet。
- Cellular RX 在入队前记录各 IPv6 来源的传输序号；Transport 跨链路去重依旧由原有流程负责。

## 对外接口

```c
int linkg_cellular_link_get_rx_stats(
    linkg_link_t            *link,
    uint8_t                  peer_node_id,
    linkg_cellular_rx_stats_t *stats);
```

获取当前注册 Cellular Path 对端 IPv6 来源的三类别累计成功接收帧数及确认丢失帧数。
Wi-Fi 的 `linkg_wifi_link_get_rx_stats()` 保持不变。
AP 后续在 Switch Report 阶段调用 Cellular 查询接口即可上报 STA 上行路径质量。

## 队列溢出统计

`linkg_path_stats_t` 增加：
- `tx_queue_overflow_bytes`
- `tx_queue_overflow_packets`

`tx_queue_overflow_*` 是 `tx_dropped_*` 的子集，不能重复加入总失败分母。
仅 Wi-Fi/Cellular TX 因等待队列容量不足淘汰旧帧时更新该专用计数。
Path 退役导致的主动丢弃继续使用原来的 `tx_dropped_*` 口径。

## MTU / TCP

当前 main 已把 `LINKG_TRANSPORT_WIRE_FRAME_MAX_SIZE` 改为 1444B，本分支修正遗漏的两个编译期断言：
- 非分片有效载荷：1428B。
- 首片有效载荷：1420B。

当前 main 的 Fast NAT TCP MSS 已经是 1388B，直接沿用。

前提：Cellular 路径有效 IPv6 MTU 至少 1500B。两个端都要同步升级，旧 Cellular UDP 格式无法混用。

## 测试建议

1. 首先构建用户态 LinkG 和 `linkg_fast_nat.ko`，检查 MTU 编译期断言。
2. 两端同时部署新版本，单独验证 Cellular IPv6 UDP 正常双向收发。
3. 依次测试 1400B / 1401B 内层 IPv4 UDP Payload，确认 Transport 单帧/两片分包边界。
4. 使用固定发包和可控丢包验证 64 位接收窗口，注意缺失帧需滑出窗口才确认。
5. 人为使 TX 等待队列溢出，验证队列淘汰统计增加，但 Cell Wire Sequence 不出现缺口。
6. 在发送端重启、Path 重建及 IPv6 地址变化后验证 Session/Peer 的统计状态。
7. 使用冗余模式同时发送 Wi-Fi 与 Cellular，验证各物理路径各自记账。

**状态：静态结构与接口审核已完成；尚未交叉编译或在 T113/RG255 实板运行。**

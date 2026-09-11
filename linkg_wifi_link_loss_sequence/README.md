# LinkG Wi-Fi 链路序号与丢包累计统计实现

## 目标

本实现只增加 Wi-Fi 私有链路级丢包统计能力，不修改 `linkg_path_t`，不修改 Transport Wire Sequence，不引入切换算法。

Wi-Fi Wire：

```
[ 4B Wi-Fi sequence ][ 原 Transport Packet ]
```

Cellular Wire 保持不变。

## 替换 / 新增文件

- `modules/wifi/wifi_wire.h`（新增）
- `modules/wifi/wifi_tx.c`（完整替换）
- `modules/wifi/wifi_rx.c`（完整替换）
- `modules/wifi/wifi_rx.h`（完整替换）
- `modules/wifi/linkg_wifi_link.c`（完整替换）
- `include/linkg/modules/wifi/linkg_wifi_link.h`（完整替换）

## TX 语义

- 每个 Wi-Fi 对端 IPv4 独立维护一个 `uint32_t next_sequence`。
- REALTIME / VIDEO / DATA 共用该对端同一个 Wi-Fi sequence space。
- Queue 中的 Packet 不占用 sequence。
- 真正构造 `sendmmsg()` 时临时 reserve sequence。
- `sendmmsg()` 返回的连续成功前缀才 commit sequence。
- 未发送尾部仍留 Queue，下轮重新获得未消耗的 sequence。
- Wire Header 使用第二个 scatter/gather iovec，不修改原 Packet 内存。

## RX 语义

- 每个来源 Wi-Fi IPv4 独立维护 64-bit sequence window。
- `received_packets`：首次成功收到的唯一 Wi-Fi Wire Packet 累计数。
- `confirmed_lost_packets`：missing sequence 被 64-window 推出时仍未到达的累计确认丢包数。
- 窗口内晚到包会补洞，不计 loss。
- duplicate 不增加 received。
- 已经推出窗口后才到达的超迟包不再修改公开累计值。

## 公共接口

```c
typedef struct
{
    uint64_t received_packets;
    uint64_t confirmed_lost_packets;
} linkg_wifi_rx_stats_t;

int linkg_wifi_link_get_rx_stats(linkg_link_t *link,
                                 const struct in_addr *peer_address,
                                 linkg_wifi_rx_stats_t *stats);
```

统计模块后续自行增加 `report_id / sample_time / generation`，并根据两次累计值计算 delta、samples 和 loss rate。

## MTU

Wi-Fi 私有头增加 4 Byte，因此 Wi-Fi 单 UDP Datagram 中可承载的原 Transport Packet 上限为：

```
1500 - IPv4(20) - UDP(8) - WiFiHeader(4) = 1468 Byte
```

代码会拒绝超过该上限的 Wi-Fi Transport Packet。当前 LinkG 如果已经按 Cellular IPv6 最坏开销限制 Transport Packet，则该 4 Byte 应处于 Wi-Fi 原有余量内；集成后仍建议用最大 Transport Packet 做一次边界测试。

## 注意事项

当前 sequence/window 以进程内 Wi-Fi 对端 IPv4 为生命周期 Key。正常 Path 临时注销/重建不会清 TX sequence。若未来需要支持“远端 LinkG 进程重启但本端进程不重启”后立即继续统计，应在 Peer Session/Generation 接入时增加 RX window reset 通知；这不影响当前 Wire Header 和累计统计接口设计。

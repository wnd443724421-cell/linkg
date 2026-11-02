# 5G 开启后的双链路补充测试报告

## 1. 结论

用户开启 AP 侧 5G Path 后，原“Cellular Path 未建立”阻塞已经解除：

- AP 与 STA 均为 `path_enabled=true`；
- AP 对 Node 2、STA 对 Node 1 的 Cellular peer 均为 `active=true`；
- AP/STA Switch 状态均报告 Cellular Path available；
- REALTIME、VIDEO、DATA 三类 Send Plan 均为 Wi-Fi single，Cellular secondary；
- 两个方向的三类 Cellular Probe 均能形成真实 ACK 和 RTT；
- 后续低频与高频同窗复测中，双向 Cellular IPv6 ICMP 均为 0% 丢包。

双链路前置条件现在为 **PASS**。但测试过程中观察到一段间歇性 Cellular Probe 超时，必须单独保留为 **FAIL（短时稳定性）**，不能被后续通过结果覆盖：

- 60 秒 VIDEO 双链路基线中，74 轮×2 板侧快照里有 2 个板侧快照不满足“三类全部可达”；
- 随后的 180 秒、250 ms 采样中，715 轮共出现 86 个失败板侧快照，按板侧连续状态修正合并为 30 个事件组；
- 最长事件观测跨度 1.5 秒，所有事件均自行恢复；
- 715 轮中 AP/STA Send Plan 全程保持 Wi-Fi 主、Cellular 备，没有误切换；
- 后续 120 秒低频同窗和 60 秒高频同窗均未复现，且同窗 ICMP 均为 0% 丢包。

因此可确认“Path 建立、当前可达、状态能恢复、没有误切换”；不能确认早期超时的根因。证据不足以归因于 Web 高频查询、运营商链路或 LinkG Probe 调度中的某一个单点。

## 2. 5G 开启后的前置状态

2026-11-02 16:16（Asia/Shanghai）重新采集：

- AP：5G SA，`path_enabled=true`，Node 2 peer active；Cellular link ID 2。
- STA：LTE，`path_enabled=true`，Node 1 peer active；Cellular link ID 1。
- 两侧三类计划均为：

```text
mode=single
primary=wifi
secondary=cellular
```

刚开启后的第一组 Cellular Probe 尚为 `reachable=false`；约 30 秒后进入稳定可达。16:16:48～16:17:54 的 60 秒复查中，两侧三类 Probe 在起止快照均可达：

- AP→Node 2：64～97 ms；
- STA→Node 1：62～116 ms。

这证明 Path active 不是单纯配置标志，已经存在实际 LinkG Cellular Probe 往返。

## 3. 双链路正常 VIDEO 基线

时间：16:20:09～16:21:25；VIDEO 实际发送 60 秒。

### 3.1 业务与计划

- VIDEO 数据面通过：PASS。
- AP/STA 74 轮 Web 采样全部成功。
- 所有 Send Plan 始终为 Wi-Fi 主、Cellular 备。
- 未进入 REDUNDANT，未迁移到 Cellular。
- qdisc 结构前后不变，全部接口错误为 0。

实际主要业务仍走 Wi-Fi：

| 位置 | 字节增量 |
|---|---:|
| STA `eth0` RX | 46,992,607 |
| STA `linkg0` TX | 46,392,738 |
| STA `wlan0` TX | 49,168,695 |
| AP `wlan0` RX | 49,168,349 |
| AP `linkg0` RX | 46,392,661 |
| AP `eth0` TX | 47,322,809 |

备用 Cellular 仍有 Probe/Discovery 业务：

| 位置 | RX 增量 | TX 增量 |
|---|---:|---:|
| AP `usb0` | 48,171 | 126,365 |
| STA `usb0` | 116,521 | 53,543 |

### 3.2 短时 Probe 超时

严格的“所有快照三类均可达”判据失败：

- 16:21:04，AP 三类 Cellular Probe 同时不可达；
- 16:21:05，STA DATA Probe 不可达；REALTIME RTT 一度为 254.574 ms；
- 其余采样恢复正常；VIDEO 业务和 Send Plan 未受影响。

本项结论：

- 双链路正常业务路径及计划安全性：**PASS**；
- Cellular Probe 全程稳定性：**FAIL**。

## 4. 180 秒高频只读监测

时间：16:23:10～16:26:10；250 ms 请求间隔；无额外 VIDEO 压力。

### 4.1 样本与事件

- 715 轮，每轮采集 AP/STA 的 Cellular 与 Switch 状态；
- Web 响应失败：0；
- Send Plan 失败/变化：0；
- 失败板侧快照：86/1430；
- 三类总快照失败约 165/4290；
- 按板侧连续状态合并：30 个事件组；
- AP 10 组，STA 20 组；
- 最长观测跨度 1.5 秒；所有事件自行恢复。

事件组主要分布：

- AP 三类同时不可达：7 组；
- STA REALTIME 单类不可达：5 组；
- STA DATA 单类不可达：4 组；
- STA VIDEO 单类不可达：4 组；
- 其余为双类或三类组合。

有效 RTT 快照范围：

| 方向 | 类别 | 最小 | 均值 | 最大 |
|---|---|---:|---:|---:|
| AP→STA | REALTIME | 34.496 ms | 116.298 ms | 289.403 ms |
| AP→STA | VIDEO | 39.512 ms | 120.567 ms | 275.176 ms |
| AP→STA | DATA | 40.002 ms | 122.319 ms | 274.987 ms |
| STA→AP | REALTIME | 25.074 ms | 88.848 ms | 294.739 ms |
| STA→AP | VIDEO | 31.482 ms | 94.251 ms | 290.062 ms |
| STA→AP | DATA | 32.920 ms | 86.590 ms | 235.457 ms |

源码的周期 Probe 默认超时为 300 ms；有效 RTT 最大值已经接近该边界，与短时 TIMEOUT 现象一致。

### 4.2 事件分组修正

初版汇总把同一序号的 AP/STA 失败交错写入一个列表，错误拆成了 61 组。原始 715 轮数据未变；修正为“每个板侧独立按连续 sequence 和失败类别组合分组”后，真实结果为 30 组。修正后的脚本和 JSON 均保留 `analysis_revision`。

## 5. Cellular IPv6 与 LinkG Probe 对照

### 5.1 独立 ICMP 对照

16:30:36～16:31:35：

- AP→STA：60/60，0% 丢包，RTT 24.209/46.325/107.606 ms；
- STA→AP：60/60，0% 丢包，RTT 24.993/33.971/84.756 ms。

这轮与前面的失败窗口不是同一时间窗，因此只能证明后续底层 Cellular IPv6 正常，不能单独反证先前 TIMEOUT。

### 5.2 120 秒低频同窗对照

16:32:48～16:34:50；Web 每秒采样一次，同时双向各发 1 个 ICMP/秒：

- LinkG：120 轮、两板三类 Probe 全部可达；
- Send Plan：120 轮全部稳定；
- AP→STA ICMP：120/120，0% 丢包，最大 66.038 ms；
- STA→AP ICMP：120/120，0% 丢包，最大 48.370 ms。

结论：**PASS**。

### 5.3 60 秒高频同窗 A/B

16:36:23～16:37:24；Web 250 ms 采样，同时双向各发 1 个 ICMP/秒：

- LinkG：239 轮、两板三类 Probe 全部可达；
- Send Plan：239 轮全部稳定；
- AP→STA ICMP：60/60，0% 丢包，最大 127.247 ms；
- STA→AP ICMP：60/60，0% 丢包，最大 48.090 ms。

结论：**PASS**。这说明 250 ms Web 查询不是稳定可复现的 TIMEOUT 触发条件，不能把前一轮异常直接归因于观测负载。

## 6. 能确认与不能确认的边界

已确认：

- Cellular Path 已正确建立，当前双向可达；
- 三类业务计划正确携带 Cellular secondary；
- 周期 Probe 的请求/响应能真实通过 Cellular；
- 短时 Probe timeout 后状态会恢复，没有旧不可达状态残留；
- timeout 期间没有错误迁移或计划覆盖；
- Wi-Fi 主链路 VIDEO 运行时，备用 Cellular 保持 Discovery/Probe 流量；
- 后续 120 秒和 60 秒复测均完全通过。

仍未确认：

- 用户业务实际切换到 Cellular 后的吞吐和中断时间；
- WIFI_LOSS_BAD、REDUNDANT、Verify、分业务迁移和恢复回切；
- 早期 Cellular Probe timeout 的唯一根因；
- Probe sent/received/timeout 累计，因为当前 Web 不暴露这些计数。

切换主体仍被 TC 安全注入条件阻塞，不再被 Cellular Path 缺失阻塞。


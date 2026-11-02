# LinkG V1 链路切换算法与指标统计专项测试报告

## 1. 结论摘要

本轮已完成源码阈值核对、60 秒 Wi-Fi T0、用户开启 5G 后的双链路前置复验、Cellular Probe 多轮稳定性复测，以及用户追加授权后的 T1～T5 渐进丢包、三业务迁移、5G 实际承载和恢复观察。没有修改定版产品源码或算法阈值，没有执行 Git Commit/Push。TC 阶段依据用户明确授权临时覆盖 `wlan0` root QoS，并在每级结束恢复原有队列语义；这一偏离与原任务书边界已单独记录。

结论不是“专项全部通过”：

- **PASS**：Wi-Fi Path、Cellular Path、正常期计划稳定、T1 轻微丢包无误切换、T2～T5 分业务迁移外部行为、DATA→VIDEO→REALTIME 优先级、5G 实际用户业务承载，以及每级 TC/trace/进程清理。
- **FAIL（间歇性质量）**：首轮双链路 VIDEO 中出现 2 个 Cellular Probe 异常板侧快照；随后 180 秒监测中出现 30 个短时事件组，最长观测跨度 1.5 秒。状态均自行恢复，计划未误切换；后续 120 秒低频和 60 秒高频同窗复测均未再出现。根因尚不能唯一归到蜂窝网络、Probe 调度或 Web 观测负载。
- **FAIL（切换与恢复）**：T3/T4 在丢包仍持续时出现 Cellular→Wi-Fi→Cellular 的计划振荡；T2～T4 清除 TC 后带业务观察 60 秒均未自动回切；T5 虽最终恢复，但三类业务最大中断为 1.360～5.898 秒，超过 500 ms 参考目标，且第一次恢复验证回退到 5G。
- **FAIL（可观测性，不代表算法判决失败）**：Web `cmd=1003` 未提供任务书要求的 Probe 发送/接收/超时累计、平均 RTT/窗口、HCC、LOSS_SCORE、LOAD_SCORE、Operation ID/phase、Verify 状态/结果。诊断 trace 只提供 HCC/OVF/LOSS 文本，且每行没有事件时间戳；成功切换生命周期也没有对应公共日志字段。当前接口不足以按任务书核对完整状态机时间线。
- **NOT RUN**：实际业务负载/压力场景和真实 `ifdown`/关 Wi-Fi 硬件故障。T5 是双向 100% 网络层丢包，不能冒充无线接口物理关闭。

## 2. 被测版本与边界

| 项目 | 实际值 |
|---|---|
| 源码主机 | `dawn@192.168.80.132:/home/dawn/project/linkg_v1` |
| 分支 / Commit | `main` / `a1e74eda5fee1b3ab5fb62460ac3324c609f6c52` |
| AP | `192.168.1.1`，Node 1，`/app/current -> /app/releases/v2` |
| STA | `192.168.3.1`，Node 2，`/app/current -> /app/releases/v1` |
| 板端内核 | 两板均为 TinaLinux `5.4.61 #43 SMP PREEMPT` |
| 板端 LinkG SHA-256 | 两板均为 `325ab0c7196e0e4e9f94173fb2ab6a522debe17e9727d9505c25ffd1d18c28d1` |
| 仓库现有二进制 SHA-256 | `upgrade/app/bin/linkg` 同为上述 SHA-256 |
| RK 摄像头板 | `172.28.2.3`，`/dev/video51`，MPP H.264 |
| 测试时间 | 2026-11-02 15:18～17:20（Asia/Shanghai） |

板端二进制与仓库中既有 `upgrade/app/bin/linkg` 哈希一致，但该二进制是测试开始前已经存在的工作区修改项。本报告只能把板测结果追溯到该 SHA-256，不能宣称它由当前 Commit 的干净工作树可重复构建。

仓库测试前已有大量 `test/` 删除项，以及 `upgrade/app/bin/linkg`、升级包修改；这些均保留未动。本轮仅在 `test/integration/` 增加板测脚本并在 `test/reports/20261102_switch/` 归档结果。产品目录 `core/ infra/ include/ modules/ platform/ service/ supervisor/ third_party/ linkg-mcu/` 的 `git diff --stat` 为空。

## 3. 双链路前置条件

### 3.1 Wi-Fi

- AP/STA 均报告 Wi-Fi `available=true`、连接正常。
- STA 到 AP 三类周期 Probe 均为 `valid=true, reachable=true`。
- T0 期间三类计划在 AP 与 STA 两侧始终是 Wi-Fi `single`，74 次采样无计划变化。
- Wi-Fi Path 前置条件：**PASS**。

### 3.2 5G / Cellular

初始快照中 AP `path_enabled=false`，所以当时双链路确实 BLOCKED。用户随后开启 5G Path；16:16 重新取证后：

- AP、STA 均为 `path_enabled=true`；
- AP Node 2 peer 与 STA Node 1 peer 均 `active=true`；
- AP `cellular_path_available=true`，STA Cellular `available=true`；
- 三类 Send Plan 均为 Wi-Fi single、Cellular secondary；
- 60 秒复查中两侧三类 Cellular Probe 均能形成 ACK，RTT 约 62～116 ms。

双链路前置条件现为 **PASS**。后续运行中观察到间歇性、可自恢复的 Cellular Probe TIMEOUT，详细数据见 `DUAL_PATH_FOLLOWUP.md`；该问题标为质量稳定性 FAIL，但不再把 Cellular Path 缺失列为 BLOCKED。

## 4. TC 安全性评估

两板 `wlan0` 的结构相同：

```text
qdisc mq 0: root
qdisc fq_codel 0: parent :8
...
qdisc fq_codel 0: parent :1
```

`sch_netem` 模块已经加载，但这并不代表存在安全挂载点。当前设备上：

- `tc filter show dev wlan0 root` → `RTNETLINK answers: Operation not supported`
- `tc filter show dev wlan0 ingress` → 同上
- `tc filter show dev wlan0 egress` → 同上
- `tc action ls action gact` → 同上

因此无法在保留 QoS 的前提下实施随机丢包。随后用户明确授权：测试期间允许 `netem` 替换整个 `wlan0` root，不要求保留 QoS，除产品源码外所有测试开关可用。

执行前先做两类恢复演练：

- `tc qdisc replace dev wlan0 root mq` 会重新分配动态 handle（例如 `8001:`），但能自动重建 8 个参数相同的 `fq_codel` leaf；恢复判断因此比较队列语义而非 handle 数字。
- 板载 iproute2 5.11 的 `tc -d` 会把 `delay 0ms` 错误显示为 `delay 275s 275s`；1% 微探针中 50 个直连 ICMP 收到 49 个，RTT 1.3～2.7 ms，netem 统计丢 1 包，证明这是显示换算问题，不是实际 275 秒延迟。

正式命令采用 `netem delay 0ms loss random X%`，每次均在 `finally` 恢复 root `mq`。T1～T4 只作用于 STA 出口；T5 同时作用于 AP/STA。结论：在用户授权的新边界下，TC 注入与恢复前置为 **PASS（带任务书偏离说明）**。

## 5. T0 Wi-Fi 正常基线

### 5.1 场景

- 业务：RK `/dev/video51` → H.264/RTP/UDP 5600 → `172.28.1.100`。
- 分类：现网规则 UDP 5600～5610 为 VIDEO。
- 发送时长：60 秒。
- Web 采样：AP/STA 每秒读取 `cmd=1003/1006/1009`，共 74 组；额外时间来自 GStreamer 建链及清理。
- HCC/LOSS：仅在 STA 临时创建 `/tmp/linkg_switch_trace.enable`，采集完成后删除 marker 和输出文件；原始 trace 已保存到报告目录。

### 5.2 实际数据路径

| 位置 | 方向 | 字节增量 |
|---|---|---:|
| STA `eth0` | RX | 47,073,442 |
| STA `linkg0` | TX | 46,500,597 |
| STA `wlan0` | TX | 49,226,248 |
| AP `wlan0` | RX | 49,226,046 |
| AP `linkg0` | RX | 46,500,649 |
| AP `eth0` | TX | 47,358,949 |

所有上述接口 `rx_errors/tx_errors` 均为 0。STA 与 AP 的 Wi-Fi 物理字节增量仅差 202 字节，数据方向与预期一致，能够证明本场景实际经过 Wi-Fi Path，而不是只看策略字段。

### 5.3 Web 与驱动计数

- STA Web driver `tx_bytes`：69,843,326 → 118,879,412，增量 49,036,086。
- STA sysfs `wlan0 tx_bytes` 场景增量：49,226,248。两者采样边界并非完全相同，差异约 0.39%。
- AP Web driver `rx_bytes`：67,584,203 → 116,326,117，增量 48,741,914。
- AP sysfs `wlan0 rx_bytes` 场景增量：49,226,046；边界差异约 0.98%。
- STA Web LinkG traffic 在全部 74 个快照中有效；VIDEO 稳态期间 `tx_bps` 中位观测值为 7,720,332 bit/s。由于 Web 是瞬时窗口，而 sysfs 是包含建链/收尾的累计窗口，不能用该中位数直接乘 60 秒作为精确字节对账。

Web/驱动累计字节方向一致，边界差异在 1% 内：**PASS（T0 计数对照）**。

### 5.4 RTT、LOSS、HCC

以下 RTT 是 74 次 Web“当前值”快照的分布，不是内部诊断 API 的独立样本平均：

| 类别 | 有效快照 | 最小 / 快照均值 / 最大 RTT |
|---|---:|---:|
| REALTIME | 74 | 1,664 / 2,115.0 / 7,434 us |
| VIDEO | 74 | 1,668 / 2,249.3 / 6,521 us |
| DATA | 74 | 1,842 / 8,414.7 / 33,966 us |

- 上行 LOSS 有效快照 70/74，样本数范围 1～198，最大 `lost_packets=0`、`loss_permille=0`。
- 下行 LOSS 有效快照 74/74，样本数范围 1～14，最大 `lost_packets=0`、`loss_permille=0`。
- trace 共 592 条非空 UL/DL 记录，592 条全部解析成功，双方向 HCC 均有效。
- 采集期所有队列为 `0/0/0`，`FLOWCTRL_OFF` 新增总数 0、单窗最大 0；未出现 OVF 或非零 LOSS。
- AP/STA 发送计划全程一个版本，三类均保持 Wi-Fi 单发，没有误进入冗余或迁移。

T0 正常基线：**PASS**。该结论只覆盖单 Wi-Fi Path 的短期正常运行，不外推为双链路算法通过。

## 6. T1～T5 渐进丢包与实际切换

### 6.1 注入与业务模型

- T1～T4：STA `wlan0` 出口单向丢包 1%/3%/5%/10%，每级约 60 秒。
- T5：AP/STA `wlan0` 双向 100% 丢包，约 60 秒。
- 同窗业务：UDP/14550 REALTIME、RK 摄像头 H.264/RTP UDP/5600 VIDEO、UDP/6200 DATA。
- Web 计划采样约 250 ms；接收端逐秒记录三类包数与包间隙；AP `eth0` 保存 snaplen 96 的 PCAP。
- 脚本最后一次 `tc -s` 检查点约在注入后 55 秒，以下成功/丢弃数量不是完整 60 秒最终累计；比例可验证 netem 生效。

| 场景 | 检查点成功/丢弃 | 实测比例 | 首次 5G 主顺序 | 自动回切 | 结论 |
|---|---:|---:|---|---|---|
| T1 1% | 49,687/510 | 1.016% | 无迁移 | 不适用 | 轻微丢包无误切换，PASS |
| T2 3% | 22,572/684 | 2.941% | DATA→VIDEO | 否 | 迁移通过，恢复失败 |
| T3 5% | 18,796/1,033 | 5.210% | DATA→VIDEO→REALTIME | 否 | 发生 3 次丢包期回 Wi-Fi 振荡，FAIL |
| T4 10% | 6,037/674 | 10.043% | DATA→VIDEO→REALTIME | 否 | DATA 丢包期回 Wi-Fi 后再迁出，FAIL |
| T5 100% | STA 0/3,858；AP 0/104 | 100% | DATA→VIDEO→REALTIME | 是 | 功能恢复但业务中断超标，FAIL |

### 6.2 LinkG LOSS 与迁移时间

只统计 `sample_packets >= 20` 的有效 Web 窗口，T1～T4 最大上行 LOSS 分别为 29/102/140/205‰。这些是短 Wire 窗口，不等于 qdisc 累计比例。T5 完全黑洞后没有新 Wire 质量报文，最大有效窗口仍为故障起始前的 0‰；这证明“无有效新样本”不能被解释为 100% LOSS，后续依靠 Probe/Path 不可用触发。

| 场景 | DATA 冗余/5G主 | VIDEO 冗余/5G主 | REALTIME 冗余/5G主 |
|---|---:|---:|---:|
| T2 | 11.872/12.874 s | 21.898/22.899 s | 未迁移 |
| T3 | 2.242/3.493 s | 5.509/6.260 s | 48.929/51.999 s |
| T4 | 1.102/2.104 s | 3.856/6.872 s | 3.856/6.872 s |
| T5 | 0.982/1.983 s | 3.486/4.488 s | 5.991/6.992 s |

T2/T3/T5 明确观察到 DATA 优先、VIDEO 次之、REALTIME 最后；T4 在 DATA 后同时把 VIDEO/REALTIME 进入冗余，但仍没有优先于 DATA。AP 与 STA 的计划变化同步，PlanSync 最终状态为 confirmed。

这里不能直接把迁移归因于 WIFI_LOSS_BAD：T2～T4 trace 的有效 `OFF` 分别为 1056/1076/1078 条且全部为 0，有效 `OVF` 也全部为 0，没有直接 HCC 压力证据；但分级顺序与源码 LOAD 分支一致，而源码 LOSS 分支会让参与类一起进入冗余。由于 Web 不暴露 migration_reason，本轮只能报告“TC LOSS 条件下出现 LOAD 风格分级迁移”，不能直接断言内部产生了 WIFI_LOSS_BAD 或 WIFI_LOAD_BAD。这也是 LOSS/LOAD 区分验收的可观测性缺口。

### 6.3 5G 实际接管与业务连续性

| 场景 | STA `usb0` TX | AP `usb0` RX | DATA 最大间隙 | VIDEO 最大间隙 | REALTIME 最大间隙 |
|---|---:|---:|---:|---:|---:|
| T1 | 50,875 B | 45,425 B | 230.353 ms | 383.300 ms | 268.988 ms |
| T2 | 92,479,638 B | 91,095,355 B | 483.772 ms | 250.304 ms | 302.653 ms |
| T3 | 107,655,882 B | 106,053,480 B | 554.795 ms | 251.091 ms | 247.072 ms |
| T4 | 114,062,818 B | 112,265,487 B | 363.275 ms | 358.912 ms | 278.273 ms |
| T5 | 80,844,334 B | 79,821,256 B | 1,359.821 ms | 3,589.581 ms | 5,898.312 ms |

T2～T5 的 `usb0` 大字节增量与 AP 侧接收闭环证明 5G 实际承载用户业务，不是仅观察 Send Plan。T5 三类最大中断均超过 500 ms 参考目标，按验收口径为 **FAIL**。此外 AP `usb0 rx_errors` 在 T2/T3/T5 分别增加 4/3/1，T1/T4 为 0；无法仅凭 sysfs 唯一归因，原始值已保留。

### 6.4 恢复与振荡

T2～T4 清除 TC 后继续带业务观察 60 秒，Wi-Fi Probe 和 LOSS 已恢复，但均未自动回切。migration_reason 不可见；源码对 LOSS 迁出要求当前 RSSI 比基线至少高 3 dB，对 LOAD 迁出也要求 RSSI 不低于基线。T2 迁出时约 -38/-39 dBm，恢复期为 -47～-49 dBm，因此两种原因都不具备恢复候选。为了继续下一级，使用 Web 临时关闭并重新启用 STA Cellular Path；每次 `/app/linkgUser.json` 复位前后 SHA-256 相同。该人工复位只用于恢复测试起点，不能算自动回切通过。

T3/T4 更暴露出反向问题：TC 仍在生效时，部分业务短暂进入 Cellular 主后的冗余恢复并回到 Wi-Fi，随后又因持续丢包迁出，形成计划振荡。T5 清除 TC 后第一次全业务冗余验证退回 5G，第二次才回 Wi-Fi；首次全 Wi-Fi 计划约在清除后 27.161 秒，连续正常确认约 29.1 秒。

完整数据见 `tc/TC_LOSS_CAMPAIGN_REPORT.md`、`tc/TC_LOSS_METRICS.json` 和 `tc/TC_EVENT_TIMELINE.md`。

## 7. 可观测性缺口

当前 Web `cmd=1003` 提供：当前 Plan、PlanSync 摘要、Wi-Fi 当前 Probe 值与年龄、traffic 当前窗口、LOSS 当前窗口、Path 可用性。

任务书要求但当前 Web/日志未提供：

- 周期 Probe 发送/接收/超时累计与内部平均 RTT/统计窗口；
- HCC `tx_allowed`、队列长度、`flowctrl_off_count`；
- `LOSS_SCORE`、`LOAD_SCORE`、HCC pressure score；
- 当前 Operation ID、业务掩码、phase（WAIT_ACK/RETRY_WAIT/WAIT_VERIFY 等）；
- Verify 开始/结束、参与类别、每链路样本、结果和 VERIFY_DONE 交付状态；
- 可用于计算实际事件耗时的带时间戳成功事件日志。

源码存在同步 `linkg_path_probe_diagnose()` API，可生成 sent/received/lost/平均 RTT 等结果，但没有对应 Web 命令，也不能从外部附着到正在运行的 LinkG 进程调用。`/tmp/linkg_switch_wifi_250ms.log` 能补充 HCC/OVF/LOSS，但行本身没有单调或墙钟时间戳，也没有 score/operation/verify。

因此：

- 当前 RTT/LOSS 值及 freshness：可测；
- netem 检查点比例与 Web 非零 LOSS 窗口均可测，但 Probe 累计、内部 score、Operation/Verify 时间线在不改定版程序的边界内不可完整测；
- “Web/日志覆盖任务书全部指标”判定为 **FAIL（可观测性）**；
- 外部切换计划和实际 5G 用户路径已完成验证；内部四种 Verify 结果仍不能直接验收。

## 8. 恢复与清理

- T0/双链路前置阶段未修改 qdisc；TC 阶段按用户授权临时替换 root，并在每级结束恢复。
- 最终 AP/STA 均为 `mq + 8×fq_codel`，参数与初始默认 leaf 一致；动态 handle 由内核重新分配。
- 本次创建的 STA `/tmp/linkg_switch_trace.enable` 和 `/tmp/linkg_switch_wifi_250ms.log` 均已删除，并再次通过 `test ! -e` 验证。
- GStreamer、tcpdump、TC/复位脚本均无残留进程；PCAP 已归档。
- 用户在测试间隙开启了 5G Path。T2～T4 自动回切失败后，本测试代理依据用户“所有测试开关允许”的授权，临时关闭并恢复 STA Cellular Path 作为场景间复位；每次 `/app/linkgUser.json` 前后 SHA-256 一致，最终配置与 Path 均恢复。
- 未执行 Git Commit/Push。

## 9. 剩余边界

1. Probe 累计发送/接收/超时、内部平均 RTT/窗口、LOSS_SCORE、LOAD_SCORE、Operation ID/phase、Verify 四类结果仍缺少定版程序的外部诊断入口；因此只对外部计划和实际数据路径作结论。
2. LOAD/压力测试按用户边界由用户执行，本轮不以 TC 丢包冒充 HCC LOAD。
3. 真实 `ifdown`/关闭 Wi-Fi 的硬件故障未执行；T5 只代表双向网络层 100% 丢包。
4. 没有执行长稳、压力和外场测试。


# LinkG V1 链路切换专项测试矩阵

| 编号 | 测试项 | 结果 | 实际证据 / 结论 |
|---|---|---|---|
| ENV-001 | Commit、内核、板端 SHA-256、路由记录 | PASS | Commit `a1e74e...`; 两板 SHA-256 `325ab0...`; TinaLinux 5.4.61；完整原始数据见 `preflight_full.json` |
| ENV-002 | Wi-Fi Path 双向可用 | PASS | AP/STA 均 connected；三类 Probe reachable；实际 VIDEO 方向计数闭环 |
| ENV-003 | 两板蜂窝注册与互联网可用 | PASS | AP 5G SA、STA LTE，均 `available=true, internet_available=true` |
| ENV-004 | Cellular Path 对同一 Peer 双向可用 | PASS | 用户开启后，两侧 Path active/available；三类计划均有 Cellular secondary；三类 Probe 可形成双向 ACK |
| TC-001 | 原始 qdisc/统计记录 | PASS | 两板均 `mq` root + 8 个 `fq_codel` leaf；原始输出保存在 T0/preflight JSON |
| TC-002 | 用户授权的 root netem 注入与原队列恢复 | PASS | 用户明确允许测试期间不保留 QoS；0%/1% 微探针验证后执行 T1～T5；每级均恢复为 `mq + 8×fq_codel`。动态 qdisc handle 会变化，但队列语义和参数恢复 |
| T0-001 | 0% 正常 VIDEO，60 秒真实路径 | PASS | 47 MB 级业务贯穿 STA eth0→linkg0→wlan0→AP wlan0→linkg0→eth0；接口错误 0 |
| T0-002 | 正常期计划稳定，不误切换 | PASS | 74 次 AP/STA Web 采样，计划唯一且三类始终 Wi-Fi single |
| T0-003 | 正常期 RTT 当前值与 freshness | PASS | 三类 74/74 reachable；当前 RTT 1.664～33.966 ms；无旧值停滞迹象 |
| T0-004 | 正常期 LOSS 窗口 | PASS | UL 70/74、DL 74/74 有效；最大 lost=0、0‰；仅证明零丢包场景 |
| T0-005 | 正常期 HCC/OVF trace | PASS | 592/592 行解析；UL/DL 队列 0；OFF 增量 0；无 OVF |
| T0-006 | Web/驱动 TX/RX 累计对照 | PASS | STA TX、AP RX 的 Web driver 与 sysfs 场景增量方向一致，采样边界差异 <1% |
| DUAL-001 | 开启 5G 后双链路注册与 60 秒可达性 | PASS | AP/STA Path active；起止三类 Probe 全部 reachable；RTT 62～116 ms |
| DUAL-002 | 双链路 VIDEO 业务路径与计划稳定 | PASS | VIDEO 通过；74 轮计划均为 Wi-Fi 主、Cellular 备；接口错误 0；qdisc 未变 |
| DUAL-003 | 双链路 VIDEO 期间 Cellular Probe 全程稳定 | FAIL | 74 轮中 AP 1 个三类全失败快照，下一秒 STA 1 个 DATA 失败快照，随后恢复 |
| DUAL-004 | 180 秒 Cellular Probe 稳定性 | FAIL | 715 轮、86 个失败板侧快照、修正合并 30 个事件组；最长 1.5 秒；计划零变化 |
| DUAL-005 | TIMEOUT 后状态恢复且无旧状态残留 | PASS | 所有事件自行恢复，当前三类 reachable；715 轮无计划误切换 |
| DUAL-006 | 120 秒低频 Web + 双向 ICMP 同窗 | PASS | 120 轮 LinkG Probe 全可达；双向 ICMP 120/120、0% 丢包；计划稳定 |
| DUAL-007 | 60 秒高频 Web + 双向 ICMP 同窗 A/B | PASS | 239 轮 LinkG Probe 全可达；双向 ICMP 60/60、0% 丢包；未复现早期超时 |
| PROBE-001 | Probe 发送/接收/超时累计、平均值、窗口 | BLOCKED | 运行态 Web 仅给最近值；内部 diagnose API 没有外部入口 |
| LOSS-001 | 非零 LOSS 窗口计算与状态响应 | PASS | T1～T4 Web 有效窗口最大 LOSS 分别为 29/102/140/205‰；T5 黑洞后无新 Wire 样本，不能用“无样本”冒充 100% LOSS。LOSS_SCORE 内部值仍由 OBS-001 记录为不可观测 |
| HCC-001 | 驱动 HCC 与内部 trace 一致 | PASS | trace 数据直接来自 `wifi_driver_get_tx_flowctrl_status`；T0 OFF=0；T2/T3/T4 有效 OFF 分别 1056/1076/1078 条且全部为 0，有效 OVF 也全部为 0。TC 迁移没有直接 HCC 压力证据 |
| HCC-002 | HCC 与 Web 上报一致 | FAIL | `cmd=1003/1006` 不含 HCC 队列、tx_allowed 或 OFF count，无法形成任务书要求的 Web 对照 |
| OBS-001 | LOSS_SCORE / LOAD_SCORE 上报 | FAIL | 公共状态/Web/trace 均未暴露 score |
| OBS-002 | Operation/Verify/ACK 完整状态与时间戳 | FAIL | Web 仅有 PlanSync 摘要；没有 Operation ID/phase、Verify 结果和带时间戳成功事件日志 |
| T1-001 | 1% Wi-Fi 单向 TC 丢包 | PASS | 末次约 55 秒检查点 49,687 成功/510 丢弃，1.016%；三类计划无误切换，最大包间隙 DATA/VIDEO/REALTIME=230/383/269 ms |
| T2-001 | 3% Wi-Fi 单向 TC 丢包 | PASS | 检查点 22,572/684，2.941%；DATA 在 11.872/12.874 秒进入冗余/5G 主，VIDEO 在 21.898/22.899 秒，REALTIME 保留 Wi-Fi；自动回切失败另见 REC-001 |
| T3-001 | 5% Wi-Fi 单向 TC 丢包 | FAIL | 检查点 18,796/1,033，5.210%；完成 DATA→VIDEO→REALTIME 迁移，但丢包仍存在时发生 3 次 Cellular→Wi-Fi→Cellular 计划振荡 |
| T4-001 | 10% Wi-Fi 单向 TC 丢包 | FAIL | 检查点 6,037/674，10.043%；约 6.872 秒三类均 5G 主，但 DATA 在持续丢包期间回 Wi-Fi 后再次迁出 |
| T5-001 | 100% Wi-Fi 双向 TC 丢包 | FAIL | STA/AP 末次检查点均为 100%（0/3,858、0/104）；最终 5G 接管并恢复，但 DATA/VIDEO/REALTIME 最大中断 1.360/3.590/5.898 秒，超过 500 ms 参考目标；首次恢复验证还回退到 5G |
| ALG-001 | Wi-Fi 质量恶化外部行为：冗余 → 单发 | PASS | T2～T5 均实际观察到 REDUNDANT 与随后 Cellular single，AP/STA 计划一致；内部 migration_reason/事件名/Verify 结果不可见，不能直接宣称是 WIFI_LOSS_BAD 或 WIFI_LOAD_BAD |
| ALG-002 | DATA → VIDEO → REALTIME 分业务迁移 | PASS | T2 为 DATA→VIDEO 且 REALTIME 留 Wi-Fi；T3/T5 完整观察到 DATA→VIDEO→REALTIME，T5 首次 5G 主时间为 1.983/4.488/6.992 秒 |
| ALG-003 | Verify WIFI/CELLULAR/MIXED/INCONCLUSIVE | FAIL | 已构造真实故障并观察冗余/退出，但 Web/日志仍不暴露 Verify 开始、结果或分类掩码，四种内部结果无法按任务书直接验收 |
| REC-001 | 清除丢包后的恢复判定与分业务回切 | FAIL | T2～T4 清除 TC 后带业务观察 60 秒均未回切；migration_reason 不可见，但 T2 基线约 -38/-39 dBm、恢复期 -47～-49 dBm，既不满足 LOSS 的基线+3 dB，也不满足 LOAD 的不低于基线。T5 约 27.161 秒回 Wi-Fi，但第一次恢复验证回退，且未呈现逐业务回切 |
| LOAD-001 | 实际负载/HCC FLOWCTRL_OFF/LOAD | NOT RUN | 用户明确压力测试自行执行；本轮只做逻辑与短期运行 |
| HARD-001 | 受控 Wi-Fi 接口关闭硬故障 | NOT RUN | T5 已覆盖双向网络层 100% 丢包，但未执行 `ifdown`/关闭 Wi-Fi；两种故障分别记录，不能用 T5 冒充物理接口关闭 |
| CLEAN-001 | TC/QoS 恢复与临时文件清理 | PASS | 五级测试后 AP/STA 均为 `mq + 8×fq_codel`；GStreamer、tcpdump、TC 脚本、trace marker/output 均无残留；最终三类 Wi-Fi 主/Cellular 备 |

说明：当前 37 项中 PASS 24、FAIL 10、BLOCKED 1、NOT RUN 2。用户明确授权 root netem 后，T1～T5 和实际切换已完成；FAIL 包括原有 Cellular Probe 间歇性 TIMEOUT、可观测性缺口、T3/T4 持续丢包期间计划振荡、T5 业务中断超标及自动恢复不符合任务书预期。剩余 BLOCKED 仅为现有外部接口无法取得 Probe 累计/平均/窗口原始诊断值；压力/LOAD 和实际 `ifdown` 硬故障仍按边界 NOT RUN。


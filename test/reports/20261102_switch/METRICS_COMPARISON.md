# 统计指标正确性对照表

| 指标 | Web/状态来源 | 底层/原始来源 | 本轮结果 | 结论 |
|---|---|---|---|---|
| Wi-Fi 当前 RTT | `cmd=1003` / `cmd=1006` | 周期 Probe snapshot | 三类 74/74 reachable，1.664～33.966 ms | PASS（当前值/新鲜度） |
| Probe sent/received/timeout | 无 | 内部 `linkg_path_probe_diagnose()` 才有 | 运行态无外部入口 | BLOCKED |
| Probe 平均 RTT/窗口 | 无 | 内部诊断结果 | Web 采样均值不能冒充内部平均 | BLOCKED |
| Wi-Fi LOSS | `cmd=1003` lost/sample/permille | Switch Wire window / trace | T0 所有有效窗 0‰；T1～T4 `N>=20` 最大 29/102/140/205‰ | PASS（窗口值） |
| 非零 LOSS 计算 | 同上 | netem `tc -s` 检查点 | qdisc 检查点 1.016/2.941/5.210/10.043%；LinkG 为短 Wire 窗口，数值不应机械相等 | PASS（口径已区分） |
| 100% 黑洞 LOSS | `cmd=1003` | 两侧 netem 100% | 黑洞后无新 Wire 报告，最大有效 Web 窗口仍为故障起始前 0‰；随后 Path/Probe 不可用触发迁移 | PASS（无样本未误报为 GOOD 的外部行为） |
| HCC 队列/OFF | Web 无字段 | trace 直接调用驱动 flowctrl status | T0 592 条；T2/T3/T4 有效 OFF 1056/1076/1078 条且全部 0，有效 OVF 全部 0 | PASS（底层无压力证据）/ FAIL（Web 缺失） |
| LOAD_SCORE | 无 | 策略内部状态 | 未对外暴露 | FAIL（上报） |
| LOSS_SCORE | 无 | 策略内部状态 | 未对外暴露 | FAIL（上报） |
| Send Plan | `cmd=1003` AP/STA plan | wlan0/usb0/linkg0 与接收端 UDP | T0 Wi-Fi 49.226 MB 闭环；T2～T5 STA `usb0` TX 92.5～114.1 MB 级并在 AP/接收端闭环 | PASS（计划与实际路径一致） |
| Operation / Verify | 仅 PlanSync 摘要 | 策略内部状态 | 无 ID、phase、结果和成功事件时间戳 | FAIL（上报） |
| Wi-Fi TX/RX 字节 | `cmd=1006` driver | `/sys/class/net/wlan0/statistics` | STA TX、AP RX 同向；采样边界差异 <1% | PASS |
| Web 请求稳定性 | TCP 29608 | 74×两板×3 命令 | 444 次响应，无错误 | PASS |
| Cellular 当前 RTT | `cmd=1009` 三类 Probe | 双向 Cellular Path / ICMP 对照 | 正常窗口 AP/STA 均可达；低频 120 轮和高频 239 轮全通过 | PASS（当前值） |
| Cellular Probe 短时稳定性 | `cmd=1009` reachable | 715 轮 250 ms 采样 | 30 个短时事件组，最长 1.5 秒，均恢复 | FAIL（间歇性） |
| Cellular IPv6 底层连通 | `usb0` 双向 `ping6` | ICMP 原始统计 | 独立 60/60；同窗 120/120 与 60/60；全部 0% 丢包 | PASS |
| 备用链路抖动时 Send Plan | `cmd=1003` | AP/STA 715 轮计划 | 始终 Wi-Fi 主、Cellular 备，无误迁移 | PASS |
| 丢包期计划稳定性 | `cmd=1003` 约 250 ms | TC phase + 实际时间戳 | T3/T4 在 TC 仍生效时出现 Cellular→Wi-Fi→Cellular 振荡 | FAIL |
| LOSS/LOAD 区分 | Web 不含 migration_reason/score | TC LOSS + HCC OFF/OVF trace + Send Plan | 无 HCC 压力证据，但出现 DATA→VIDEO→REALTIME 的 LOAD 风格迁移；内部原因无法直接确认 | FAIL（区分验收） |
| TC 清除后恢复 | `cmd=1003` + Wi-Fi LOSS/RSSI | qdisc 已恢复、三类业务持续 | T2～T4 60 秒内未回切；RSSI 既不满足 LOSS 的基线+3 dB，也不满足 LOAD 的不低于基线；T5 第二次约 27.161 秒回 Wi-Fi | FAIL（任务书预期） |
| 三业务切换中断 | UDP 接收时间戳 | 三类接收 socket/PCAP | T5 DATA/VIDEO/REALTIME 最大间隙 1.360/3.590/5.898 秒 | FAIL（>500 ms 参考目标） |
| Cellular 接口错误 | sysfs `usb0` | 场景前后计数 | AP `rx_errors` T2/T3/T5 增量 4/3/1，T1/T4 为 0 | 记录，根因未唯一定位 |

边界：RTT 表中的“快照均值”只是重复读取最近值的统计描述；相同内部 Probe 值可能被多次观察，不能称为内部 Probe 样本平均。ICMP 与 LinkG Probe 协议/调度不同，ICMP 0% 丢包不能直接证明 LinkG Probe 没有超时。


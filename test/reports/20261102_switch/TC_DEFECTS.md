# TC 丢包专项问题记录

## TC-DEF-001：持续丢包期间计划回切振荡

- 状态：FAIL，可复现于 T3 5% 和 T4 10%。
- 前置：三类业务并发，Wi-Fi 主、Cellular 备；STA `wlan0` root netem。
- 实际：业务迁出到 Cellular 主后，在 TC 仍处于 loss phase 时进入恢复冗余并回到 Wi-Fi，随后又因持续丢包迁出。
- T3：记录到 3 次“已 Cellular 主后又 Wi-Fi single”的业务事件；DATA 与 VIDEO 均出现。
- T4：DATA 出现一次相同振荡。
- 影响：增加 PlanSync/Verify 次数，可能扩大业务抖动，并违背“恢复质量满足后再回切”的预期。
- 证据：`tc/T3_5P_STA/result.json`、`tc/T4_10P_STA/result.json`、`tc/TC_EVENT_TIMELINE.md`。
- 定位边界：Web 不暴露 Operation/Verify 结果，因此已证明外部计划振荡，但不能从定版接口确认是哪一个内部 Verify 枚举导致。

## TC-DEF-002：TC 丢包清除后 60 秒不自动回切

- 状态：FAIL，可复现于 T2/T3/T4。
- 实际：清除 netem 并恢复 `mq + 8×fq_codel` 后，Wi-Fi Probe reachable、LOSS 回到 0，但 Cellular 主业务 60 秒内不回 Wi-Fi。
- 源码条件：LOSS 迁出要求当前 RSSI 比基线至少提高 3 dB；LOAD 迁出也要求当前 RSSI 不低于基线。
- T2 实测：迁出约 -38/-39 dBm；恢复观察约 -47～-49 dBm，无法满足门槛。
- 影响：由非射频原因产生的网络层随机丢包消失后，如果 RSSI 没有超过迁出瞬间值，业务可长期留在 Cellular。
- 场景间复位：仅为了继续下一级，临时关闭并恢复 STA Cellular Path；每次 `linkgUser.json` SHA-256 前后一致。该人工复位不能算自动恢复通过。
- 证据：三个 `cellular_path_reset.json` 及 T2～T4 原始结果。

## TC-DEF-003：TC LOSS 条件下出现 LOAD 风格分级迁移

- 状态：FAIL（区分行为/可观测性），内部事件名尚不能直接确认。
- TC 是随机网络层丢包，不等同于真实 HCC 负载。T2/T3/T4 trace 中所有有效 `OFF` 都为 0，所有有效 `OVF` 也为 0，没有直接 HCC 压力证据。
- 实际计划却按 DATA→VIDEO→REALTIME 分级迁移；这与源码 LOAD 分支一致，而源码 LOSS 初始事件会让参与类一起进入冗余。
- 可能解释：Wire BAD + Probe TIMEOUT 的快速质量组合进入了 LOAD 风格路径；但 Web 不暴露 migration_reason/score，不能把该推断写成已确认内部事件。
- 影响：任务书要求区分 LOSS 与 LOAD；当前外部行为与 HCC 证据之间存在疑点，且定版接口不足以直接完成归因。
- 证据：T2～T4 trace、Send Plan 时间线和 `SOURCE_THRESHOLDS.md`。

## TC-DEF-004：双向 100% 丢包时业务中断超过 500 ms

- 状态：FAIL。
- T5 实际最大接收包间隙：DATA 1,359.821 ms、VIDEO 3,589.581 ms、REALTIME 5,898.312 ms。
- 计划时间：DATA/VIDEO/REALTIME 首次 Cellular 主分别为注入后 1.983/4.488/6.992 秒。
- 恢复：清除 TC 后第一次全业务冗余验证回退到 Cellular；第二次才回 Wi-Fi，首次全 Wi-Fi 约 27.161 秒。
- 正向证据：STA `usb0` TX 80,844,334 B，AP `usb0` RX 79,821,256 B，证明 5G 最终实际接管。
- 证据：`tc/T5_100P_BOTH/result.json`、`result.pcap`。

## TC-OBS-001：Verify/Operation/Score 仍不可直接观测

- 状态：FAIL（验收接口缺口，不等同于算法结果错误）。
- Web/日志缺失：LOSS_SCORE、LOAD_SCORE、Operation ID/phase、Verify 开始/结束、分类掩码、WIFI/CELLULAR/MIXED/INCONCLUSIVE 结果。
- 影响：本轮只能依据实际 Send Plan 和数据路径证明冗余/退出，不能直接验收任务书要求的四种 Verify 内部结果。

## TC-OBS-002：Cellular 接管期少量 `usb0 rx_errors`

- 状态：记录，未唯一定位。
- AP `usb0 rx_errors` 场景增量：T2=4、T3=3、T5=1；T1/T4=0。
- 限制：单一 sysfs 计数无法区分 USB/RNDIS、协议栈或瞬时链路原因；本轮未修改产品源码增加诊断。

## RF-OBS-001：noise 无独立 freshness，无法区分稳定值与旧缓存

- 状态：BLOCKED（可观测性缺口）；“主动扫描导致底噪卡死”未复现为稳定因果关系。
- 第一轮：STA 连续 20 秒 Web 快照持续更新、RSSI 变化，但 `noise_dbm` 20/20 次为 -68 dBm。
- 断电重启复测：两次正常扫描均完整结束且 `abort=0`；扫描后 noise 继续变化约 55 秒，随后保持为 -87 dBm，当时没有 scan 事件。
- 正常扫描监测：478 样本，noise 值域 -97～-77 dBm；1200 个 Ping 0% 丢包。
- abort 隔离：开始前 noise 已为 -87 dBm；abort 完整上报 `abort=1`，后续 359 样本仍为 -87，不能建立 abort→固定的因果关系。
- 源码定位：Web 直接读取 `hmac_vap->s_free_power`；survey 从扫描记录 `ast_chan_results[]` 单独计算。Web 的 `updated_ms` 是整份快照时间，不是 noise 测量时间。
- 确定缺口：没有 noise 专属的采样时间、对应信道、来源、generation 和 freshness；仅以 -120～0 dBm 数值范围设置 `noise_valid`。
- 影响：无法证明页面值是实时当前信道噪声，也无法安全判断由 `RSSI - noise` 得到的 SNR 是否新鲜。
- 证据：`rf_scan/RF_SCAN_RSSI_REPORT.md`、`rf_scan/rf_scan_observation.json`、`rf_scan/manual_scan_monitor.json`、`rf_scan/abort_scan_monitor.json`。

## 工具异常：`tc -d` 的零延迟显示为 275 秒

- 状态：工具显示问题，已通过实效探针排除真实延迟。
- 证据：1% 微探针 50 发 49 收，RTT 1.289～2.648 ms；qdisc 丢 1 包。
- 处理：正式场景仍显式指定 `delay 0ms loss random X%`，不使用显示的 275s 作为实际时延。

# 2026-11-02 链路切换专项测试归档

## 入口

- `SWITCH_SPECIAL_REPORT.md`：主报告、环境、T0、T1～T5、切换/恢复结论和剩余边界。
- `SWITCH_TEST_MATRIX.md`：逐项 PASS / FAIL / BLOCKED / NOT RUN 矩阵。
- `SOURCE_THRESHOLDS.md`：从当前 Commit 读取的 Probe、LOSS、HCC、LOAD、Operation、Verify 阈值。
- `TC_SAFETY_ASSESSMENT.md`：原安全挂载评估、用户授权偏离、恢复演练与最终恢复判定。
- `METRICS_COMPARISON.md`：Web、驱动/sysfs、trace 的指标对照。
- `EVENT_TIMELINE.md`：实际发生的前置检查、T0、双链路和 TC 总时间线。
- `DUAL_PATH_FOLLOWUP.md`：用户开启 5G 后的双链路 VIDEO、Probe 稳定性和 ICMP 同窗补充测试。
- `TC_DEFECTS.md`：TC 场景中发现的计划振荡、回切受阻、业务中断超标和可观测性问题。
- `rf_scan/RF_SCAN_RSSI_REPORT.md`：主动扫描业务影响、RSSI 安全获取方式、Web/survey 噪声口径及 noise freshness/扫描因果复核。

## 原始数据

- `preflight_full.json`：AP/STA/RK 身份、版本、配置、网络、Web 全量前置快照。
- `t0_baseline.json`：T0 采集器主记录，包含 74 组 AP/STA Web 快照、前后 qdisc、接口与 Wi-Fi 状态。
- `t0_video.json`：60 秒 VIDEO 数据路径结果与接口计数。
- `t0_switch_trace.log`：STA 双方向 250 ms HCC/OVF/LOSS trace 副本。
- `tc_preflight.txt`：T0 前后两板 qdisc 完整输出。
- `tc_capability_preflight.txt`：两板 root/ingress/egress filter、gact、模块与 `tc` 版本原始探测结果。
- `source_evidence.txt`：当前 Commit 关键阈值、状态结构和 Web 序列化源码的带行号摘录。
- `cellular_path_logs.txt`：两板当前 LinkG session 中 Cellular/Discovery/Path 相关原始日志。
- `post_5g_enable_preflight.json` / `post_5g_enable_60s.json`：5G 开启后的即时与 60 秒复查。
- `dual/`：双链路 VIDEO、180 秒 Probe 监测、独立 ICMP、低频/高频同窗对照的全部 JSON 和日志。
- `tc/TC_LOSS_CAMPAIGN_REPORT.md`：1%/3%/5%/10%/100% 丢包、迁移、5G 承载、振荡与恢复专项结论。
- `tc/TC_LOSS_METRICS.json`：五个场景的紧凑可机读指标。
- `tc/TC_EVENT_TIMELINE.md`：STA Send Plan 的全部实际 Web 事件时间线。
- `tc/T*/result.json` / `result.pcap`：每级完整 Web/TC/接口/接收统计和 AP `eth0` 抓包。
- `tc/T2_3P_STA`～`T4_10P_STA/cellular_path_reset.json`：自动回切失败后的场景间人工复位及配置哈希证据。
- `rf_scan/rf_scan_observation.json`：AP/STA 扫描前后 link、survey、无线直连 Ping、看门狗与清理证据。
- `video_sender.log` / `video_receiver.log`：GStreamer 原始输出。
- `sha256sums.txt`：本目录文件和专项测试脚本的 SHA-256。

注意：Cellular Path 缺失已经解决。用户明确允许测试期间覆盖 root QoS 后，T1～T5 已执行；发现 T3/T4 持续丢包期计划振荡、T2～T4 自动回切未发生，以及 T5 业务中断超过 500 ms。双链路补测还记录了可恢复的间歇性 Cellular Probe TIMEOUT。内部 score/Operation/Verify 结果仍因接口未暴露而不能直接验收。

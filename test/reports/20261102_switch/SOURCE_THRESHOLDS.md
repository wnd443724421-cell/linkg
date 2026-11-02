# 当前定版源码阈值与状态机依据

依据 Commit：`a1e74eda5fee1b3ab5fb62460ac3324c609f6c52`。

## Probe

来源：`core/probe/path_probe_internal.h`、`core/switch/switch_policy_quality.c`。

- 普通周期 Probe：1,000 ms。
- STA 以 Cellular 为主时快速周期：100 ms。
- 周期 Probe 默认超时：300 ms。
- Wi-Fi 单发质量快速 Probe：250 ms 周期、200 ms 超时。
- 只有最近 1.5 秒存在真实 USER_DATA 的类别参与快速 Probe。
- 两次 TIMEOUT 间隔超过 750 ms 不连续计分。
- 快速质量 Probe 的前置条件：Wi-Fi 与 Cellular observation 都 `USABLE`，两者 link ID 有效，且当前 Wi-Fi single 计划中 cellular 是 secondary。

## LOSS Wire 判定

来源：`core/switch/switch_policy_quality.c`。

- 单方向每批至少 20 个样本；低样本最多累计 2 秒。
- 连续 2 秒没有合法非零 Wire 新样本时清理旧积分。
- BAD 条件为 `loss >= max(0.5%, 50% / sqrt(N))`，整数实现：
  - `L >= ceil(N / 200)`；且
  - `4 * L * L >= N`。
- GOOD 条件为 `loss <= min(1%, BAD阈值的一半)`：
  - `N < 10000`：`100*L <= N && 16*L*L <= N`；
  - `N >= 10000`：`400*L <= N`。
- BAD 每方向加 1 分；上下行同轮可累计；GOOD 只使共享分数减 1。
- LOSS_SCORE 到 5 触发一次 `WIFI_LOSS_BAD`，触发后清零该积分。
- 无有效样本或零样本不能当作 GOOD。

注：T1～T4 的注入百分比不能机械映射为固定触发时间，因为 BAD 门槛还随每个 Wire 窗口的样本数 N 变化。

## HCC 与 LOAD

来源：`core/switch/switch_policy_quality.c/.h`。

- HCC 最大观测年龄：1 秒；基础窗口 250 ms。
- 新增 OFF 使 HCC pressure score +1；上下行同窗新增 OFF 可 +2。
- STA 本地 UL 在单个不超过 250 ms 的窗口内 `OFF > 5` 可快速确认压力。
- HCC pressure score 到 3 确认压力。
- 候选 2 秒没有新 OFF 自动老化；确认压力后连续 3 秒无新 OFF 且未阻塞才解除 LOSS 保护。
- LOAD 快速窗口 250 ms；HCC OFF、Wire BAD、Probe TIMEOUT 组合加分，每窗最多 +2。
- 同一类别首次 Probe TIMEOUT 权重 +1，750 ms 内连续 TIMEOUT 权重 +2。
- 只有所有活跃 Wi-Fi single 类别都收到合格 ACK 且窗口没有 BAD，LOAD_SCORE 才减 1。
- 无活跃业务时清空 LOAD_SCORE，不允许输出 LOAD_BAD。
- LOAD_SCORE 到 5 触发 `WIFI_LOAD_BAD`。

## Operation、迁移与 Verify

来源：`core/switch/switch_policy_operation.c`、`core/switch/switch_policy_verify.h`、`core/switch/switch_plan.c`。

- LOSS 事件：全部符合“Wi-Fi single + cellular secondary”的业务先进入 REDUNDANT。
- LOAD 迁移选择顺序：DATA → VIDEO → REALTIME；选择某类后，同时迁移它及更低优先级且仍为 Wi-Fi single 的类别。
- LOAD 冗余 ACK 后 1 秒迁移。
- LOSS Verify：3 秒。
- RECOVERY + LOSS Verify：3 秒。
- RECOVERY + LOAD Verify：5 秒。
- 迁出 Wi-Fi 后最短恢复等待：10 秒；恢复失败后重试延迟：15 秒。
- LOSS 迁出后的恢复候选还要求当前 Wi-Fi RSSI 比该业务迁出基线至少提高 3 dB；只清除 TC 丢包并不足以保证产生 `RECOVERY_READY`。
- LOAD 迁出的 RSSI 恢复门槛常量为 0 dB（不低于迁出基线）；非质量迁移的直接恢复要求 RSSI 至少 -60 dBm。
- Verify LOAD 每参与类别至少 10 个网络终态样本；ACK 最大年龄 1 秒。
- Cellular RTT 参考阈值：250 ms；Probe 最大年龄 1.5 秒；Observation 最大年龄 1 秒。
- Verify LOSS 单方向最少 20 样本；双链路分类比较最少 100 样本。
- Verify LOSS 初始 Wi-Fi score 5；严格小于 3 才允许进入 Wi-Fi 恢复判决。
- PlanSync 重试周期 200 ms、事务超时 2 秒、恢复初始退避 1 秒。

实测边界：T2 的 DATA/VIDEO 迁出时 Wi-Fi RSSI 约 -38/-39 dBm，清除 TC 后为 -47～-49 dBm。Web 不暴露 migration_reason；无论内部按 LOSS（基线 +3 dB）还是 LOAD（不低于基线）记录，本场景 60 秒内都不满足 RSSI 恢复门槛。该结果来自实际 Web 快照，不是用配置值推算耗时。


# LinkG V1 主动扫描、RSSI 与噪声口径专项实测

## 1. 结论

1. STA 当前对端 RSSI 可以直接用 `iw dev wlan0 link` 获取；AP 可以用指定已知 MAC 的 `iw dev wlan0 station get <STA-MAC>` 获取。两种方法均不需要断开连接，也不需要枚举全部 station。
2. `iw dev wlan0 scan` 不要求先人为断开 STA，但 HI1105 会离开当前工作信道执行全信道扫描，对在线业务造成明显影响。本轮扫描期间关联保持，UDP 吞吐却出现 38%～43% 的秒级丢包峰值，直连 Ping 最大 RTT 达 4.868 秒。
3. `iw dev wlan0 survey dump` 与 Web“信道噪声”不是同一数据源。survey 读取最近一次扫描的逐信道统计；Web 读取 HI1105 station-info 响应中的 `hmac_vap->s_free_power`。
4. 后续四轮单变量复测已稳定复现：STA 关联约 294～331 秒后，`noise_dbm` 分别固定为 -81、-84、-82 dBm；固定值每轮不同。冻结期间 RSSI、TX/RX 包计数、连接时长及状态时间戳仍持续更新。
5. 主动扫描、外部 `station get`、Web 轮询及 120 秒 STA PM 定时器均已排除为必要触发条件。错误边界位于 WCPU/DMAC station-info 响应的 `s_free_power` 生产端；Host 侧另有确定缺陷：仅按数值范围设置 `NOISE_VALID`，没有检查测量 freshness，导致旧值被无限期当作实时噪声展示。

## 2. 环境与对端

- AP：`192.168.1.1`，`wlan0=11.21.191.1`，MAC `00:01:02:F7:DA:4E`，`type AP`。
- STA：`192.168.3.1`，`wlan0=11.21.191.2`，MAC `00:01:02:F7:DB:B6`，`type managed`。
- 初始扫描专项工作频率：4960 MHz，窄带 10 MHz，信道 192；后续重启定位周期由当前配置运行在 5785 MHz、信道 157。两组结论均以各自同窗状态为准。
- 无线直连监测：AP→STA `ping -I wlan0 11.21.191.2`；STA→AP `ping -I wlan0 11.21.191.1`。
- 主动扫描期间未修改产品源码、Wi-Fi 配置、角色或工作频率。
- 最终审计发现工作区另有 `core/switch/switch_policy_verify.c` 格式化差异（1 行合并，mtime `2026-11-02 17:46:52 +0800`）。本轮 RF 测试所有写入均位于 `test/` 和 `/tmp`，没有触碰或回退该并发差异。

## 3. RSSI 获取实测

### 3.1 STA

```sh
iw dev wlan0 link
```

实测输出包含：

```text
Connected to 00:01:02:f7:da:4e (on wlan0)
freq: 4960
signal: -50 dBm
```

也可以指定已知 AP MAC：

```sh
iw dev wlan0 station get 00:01:02:f7:da:4e
```

实测同样返回 `signal: -50 dBm`。

### 3.2 AP

AP 上执行 `iw dev wlan0 link` 返回 `Not connected.`，这是 AP 角色的正常表现，不能用该命令读取客户端 RSSI。

已知 STA MAC 时可执行：

```sh
iw dev wlan0 station get 00:01:02:f7:db:b6
```

实测返回 `signal: -51 dBm`。该命令只查询一个已知 Peer，没有使用可能产生异常输出的 `iw dev wlan0 station dump`。

## 4. 主动扫描对业务的影响

### 4.1 受控脚本结果

原始脚本使用了板端不支持的小数 `sleep 0.1`。因此 150 次看门狗循环没有等待 15 秒，而是在约 2.4～2.7 秒后提前终止扫描。原始 stderr 已完整保存在 JSON 中；脚本随后已修正为 3 次整秒等待，但为避免再次影响业务，没有重跑主动扫描。

| 扫描侧 | 实际扫描命令运行时间 | 扫描结束方式 | 关联状态 | 无线直连 Ping |
|---|---:|---|---|---|
| STA | 2.437 s | 看门狗 TERM，返回码 143 | 扫描前、立即后、2 秒后均连接同一 BSSID，4960 MHz | 200 发 199 收，0.5%；最大 RTT 474.366 ms |
| AP | 2.721 s | 看门狗 TERM，返回码 143 | AP 角色 `iw link` 均为 `Not connected`；反向 Ping 证明链路仍在 | 200 发 198 收，1%；平均 RTT 516.842 ms，最大 RTT 4868.379 ms |

由于命令在约 2.5 秒被终止，不能据此断言完整扫描永远不会返回；只能确认扫描至少没有在该受控窗口内完成，且该窗口已经造成显著业务抖动。

### 4.2 同窗 iperf3 证据

用户在 AP 侧对 UDP/5600 业务持续观察，非扫描期约 15 Mbit/s、0% 丢包；两次扫描附近出现：

- 第一次：第 9～10 秒 `728/1913` 丢包，38%；
- 第二次：第 41～42 秒 `492/1134` 丢包，43%；下一秒仍有 `255/1857`，14%。

因此“STA 仍保持关联”不能等价为“扫描对业务无影响”。扫描期间的离信道驻留会造成吞吐下降、排队延迟和 UDP 丢包。

## 5. Web noise 与 survey noise 为什么不同

### 5.1 同一采样窗口

STA 同一轮顺序采集得到：

| 数据源 | 值 | 含义 |
|---|---:|---|
| Web `cmd=1006` | noise `-68 dBm` | HI1105 station-info 的 `hmac_vap->s_free_power` |
| `iw dev wlan0 link` | RSSI `-50 dBm` | 当前关联 AP 的接收信号强度 |
| `station get <AP-MAC>` | RSSI `-50 dBm` | 指定 Peer 的 station info |
| `survey dump` 4960 MHz | noise `-102 dBm` | 最近扫描记录中 4960 MHz 的逐信道空闲功率平均值 |

这些值不是同一指标，不能互相直接替换。

### 5.2 源码证据

Web 路径：

```text
HI1105 station-info response.s_free_power
  → hmac_vap->s_free_power
  → LinkG WAL_RADIO_STATUS_IOCTL noise_dbm
  → wifi_status snapshot
  → Web cmd=1006 noise_dbm
```

关键位置：

- LinkG `kernel/wal_linux_netdev_ops.c:1258`：`status->noise_dbm = hmac_vap->s_free_power;`
- SDK `hmac/dfx/hmac_reset.c:139`：`hmac_vap->s_free_power = reponse_event->s_free_power;`
- LinkG `modules/wifi/wifi_status.c:490-491`：复制 noise 并依据驱动 flag 标记有效。

survey 路径完全不同：

- SDK `hmac/connect/hmac_sme_sta.c:152` 从 `ast_chan_results[]` 读取扫描信道结果；
- `hmac_sme_sta.c:167-168` 使用 `free_power_stats_20m / uc_free_power_cnt` 计算每个信道的 survey noise；
- active/busy/rx/tx 时间同样来自该次扫描的短驻留统计，本轮通常只有 10 ms 或 25 ms，不是长期累计信道占用率。

## 6. noise 长时间同值与扫描因果复核

### 6.1 扫描前证据

主动扫描前保存的 T1～T5 JSON 中，Web noise 会变化，整体出现约 -80～-102 dBm，典型首批样本为 AP -101 dBm、STA -100 dBm。

### 6.2 第一轮固定值证据

连续 20 秒采样 STA `cmd=1006`：

- `updated_ms` 从 29450109 持续增加到 29469353；
- RSSI 在 -52～-46 dBm 之间变化；
- `noise_dbm` 20/20 次均为 -68 dBm。

数分钟后的补采：AP noise 为 -100 dBm，STA 仍为 -68 dBm。

这排除了 Web 前端冻结和 `cmd=1006` 整体缓存冻结，但只能证明 `s_free_power` 长时间返回同一数值；当时主动扫描与固定值在时间上相关，尚不足以证明扫描导致冻结。

### 6.3 断电重启后的正常完整扫描复测

断电重启后，内核日志记录：

```text
uptime 20.979   scan start
uptime 22.829   scan complete, abort=0
uptime 131.861  scan start
uptime 133.311  scan complete, abort=0
uptime 155.769  scan start
uptime 157.191  scan complete, abort=0
```

120 秒只读监测共 478 个 Web 样本：

- Web 失败 0；
- noise 出现 -97、-95、-94、-93、-92、-91、-90、-89、-87、-84、-77 dBm；
- RSSI 为 -31～-26 dBm；
- 1200 个无线直连 Ping 全收，0% 丢包，最大 RTT 16.956 ms；
- noise 在第二次正常扫描完成后仍继续变化约 55 秒，约在监测 elapsed=80.923 秒后保持为 -87 dBm；该时点没有新的 scan 日志。

因此完整主动扫描没有复现“立即固定为 -68”。

### 6.4 scan abort 隔离结果

随后执行 `scan trigger`，1 秒后执行正式 `scan abort`，两条命令均返回 0。内核日志为：

```text
uptime 459.978  scan start
uptime 460.993  cfg80211 abort enter
uptime 460.997  scan complete, abort=1
```

但 abort 测试开始前 noise 已经保持为 -87 dBm，因此该轮不能用来证明 abort 导致固定。90 秒共 359 个 Web 样本全部为 -87 dBm，同时 RSSI 从 -30 变化到 -23 dBm，900 个 Ping 全收，最终无残留 `iw`。

### 6.5 四轮单变量稳定复现

| 周期 | 控制条件 | 冻结点 | 固定值 | 关键结论 |
|---|---|---:|---:|---|
| 周期 1 | 重启后先做 90 秒纯 Web 基线；计划在第 30.16 秒执行一次 `station get` | `station get` 前 4.02 秒，连接 308 秒，`updated_ms=323015` | -81 dBm | 外部 `station get` 不是本轮触发；冻结前 90 秒基线出现 65 次 noise 跳变 |
| 周期 2 | 软件重启后约 390 秒内不读 Web、不执行 `iw` | 首次读取时已经固定；连接 367～411 秒的 45 个样本全部相同 | -81 dBm | Web 读取、主动 scan 和外部 `iw` 都不是必要触发条件 |
| 周期 3 | 关联早期执行私有 PM ON→OFF，销毁 120 秒自动 PM 定时器；dmesg 确认没有自动 `pm_enable=1` | 连接 294 秒，`updated_ms=308701` | -84 dBm | 即使阻止 PM 自动回开仍会冻结，排除 PM 定时器为根因；随后 131 个有效样本不变 |
| 周期 4 | 用户再次重启后，在 uptime 282～373 秒间仅读 Web | 连接 331 秒，`updated_ms=346426` | -82 dBm | 第三次自然复现；固定值不是硬编码常量 |

周期 1 的冻结窗口内，90 秒报告共有 120 个有效快照，`updated_ms` 119 次推进；周期 3 冻结后，RSSI 范围仍为 -30～-21 dBm。另做 45 秒字段新鲜度检查时，noise 45/45 次为 -81 dBm，但：

- RSSI 在 -26～-23 dBm 变化；
- `updated_ms` 和 `statistics_updated_ms` 均增加 44100 ms；
- TX/RX 分别增加 643/828 包；
- 连接时间增加 44 秒。

这组同一响应链证据表明 station-info 查询和响应没有整体停止，只有 `s_free_power` 字段停止变化。

### 6.6 精确错误边界

运行路径及精确位置：

1. LinkG `modules/wifi/wifi_status.c:29,811,961-963`：状态线程每 800 ms 调用 `wifi_driver_get_radio_status()`；冻结期间快照持续成功更新。
2. LinkG `kernel/wal_linux_netdev_ops.c:1258-1261`：直接读取 `hmac_vap->s_free_power`，只要数值在 `[-120,0)` 就设置 `WAL_RADIO_FLAG_NOISE_VALID`。这里没有 noise 专属时间戳、序号、信道或 freshness 检查。
3. HI1105 Host 驱动 `hmac/dfx/hmac_reset.c:127-151`：收到 `OAL_QUERY_STATION_INFO_EVENT` 后，将 DMAC 响应的 `reponse_event->s_free_power` 原样复制到 `hmac_vap->s_free_power`；同一个响应还更新包计数、RSSI、速率等字段并唤醒查询等待者。
4. HI1105 Host 驱动 `wal_linux_connect_cfg80211.c:1239-1267`：持续向 DMAC 发送 `WLAN_CFGID_QUERY_STATION_STATS` 并等待响应；冻结期间没有查询超时日志，且同一响应链的其他统计字段持续变化。

因此可确认：

- **设备侧问题边界**：WCPU/DMAC 仍返回 station-info 响应，但其中 `s_free_power` 在关联约 5 分钟后不再随新响应变化。
- **Host 集成缺陷**：`wal_radio_status_build()` 将该可能陈旧的字段仅凭数值范围标记为有效，并无限期向应用/Web 暴露。
- **不能再下探的边界**：当前 SDK 只有 Host 侧 C 源码；设备侧实现位于剥离符号的 `/vendor/firmware/hi1105/pilot/WL_ITCM.bin`，SDK 中没有对应 DMAC/WCPU ELF、MAP 或源文件，无法在现有材料中负责任地命名固件内部具体函数。

板上运行固件与当前 SDK 文件 SHA-256 完全一致，排除版本错配：

```text
WL_ITCM.bin   0fb0a2bc7cb68680560fecf47fbfc26c35709f53ef456ea00dd7306add7ca2a0
WCPU_ROM.bin  8b1c455d46ceb7dd7ca8500616fd67d28622edc2e07124fc6ea84d20add1d193
wifi_cfg      b65830bec313d84de711a7c91b006cdac5aa870aa61b2f5031141cac49b1edca
wifi_only_cfg a87a692b0af412c56ec87d888aef89ea5a00fed57bcf3822f8ffd5f59bee7fd8
```

### 6.7 修复方向

根治需要设备固件侧保证 `s_free_power` 持续重新测量，或在 station-info ABI 中同时返回 `noise_valid`、采样序号/时间、测量信道及来源。Host 侧必须将这些 freshness 元数据继续传到 LinkG/Web，不能只做数值范围判断。

在设备固件暂时不可修改时，可做保守降级：当没有可证明的新 noise 采样时返回 `noise_valid=false`，Web 显示“数据陈旧/不可用”，而不是继续显示固定数字。仅凭“连续相同若干秒”判旧只能作为临时启发式，因为真实噪声量化后也可能相同；不建议用周期主动扫描刷新 noise，扫描已实测会造成明显业务丢包和长时延。

## 7. 使用建议与未验证边界

- 在线获取 STA RSSI：使用 `iw dev wlan0 link`。
- 在线获取 AP 观察到的 STA RSSI：使用 `iw dev wlan0 station get 00:01:02:f7:db:b6`。
- 不使用 `iw dev wlan0 station dump`。
- `survey dump` 可以读取最近保存的信道统计，但没有新扫描时可能是旧数据；它不是持续实时的频谱监测。
- 不在承载视频、实时或高吞吐业务时运行无信道限制的 `iw dev wlan0 scan`。
- 如确需邻居 BSS RSSI，应在维护窗口执行，并同步监测业务中断；当前驱动的全信道扫描会明显影响在线业务。
- 已确认整板重启后 noise 会恢复变化，但约 5 分钟后再次冻结；已冻结后再次关闭 STA 省电 110 秒不能恢复。尚未用断开/重关联或驱动重载作为恢复方案，因为这类操作会中断当前链路，且不能修复 freshness 语义。

## 8. 证据文件

- `rf_scan_observation.json`：完整命令输出、扫描前后 link、survey、Ping 和看门狗 stderr。
- `manual_scan_monitor.json`：断电重启后完整扫描的 120 秒、478 样本复测。
- `abort_scan_monitor.json`：正式 scan abort 的 90 秒、359 样本隔离观察。
- `getstation_baseline_after_reboot.json`：重启后 90 秒纯 Web 基线，noise 65 次变化。
- `getstation_single_trigger.json`：冻结发生在单次外部 `station get` 之前的时序证据。
- `no_read_until_330s.json`：重启后约 390 秒内不访问 STA、首次读取即已固定的隔离证据。
- `frozen_field_freshness.json`：noise 固定时 RSSI、包计数、连接时间及状态时间戳持续变化的 45 秒证据。
- `pm_off_recovery_probe.json`：冻结后关闭 STA 省电仍不恢复的 120 秒证据。
- `pm_timer_prevention_probe.json`：取消 120 秒 PM 定时器后仍在连接 294 秒冻结的排除证据；末尾 SSH reset 发生在冻结约 131 秒后，不影响结论。
- `natural_cycle4_near_300s.json`：用户再次重启后的第三次自然冻结证据。
- `freeze_postmortem_no_iw.json`、`no_read_cycle2_postmortem.json`、`pm_prevention_midpoint.json`：无 `iw` 的进程、dmesg 与中点现场。
- `test/integration/rf_scan_observation.py`：受限扫描复现脚本；已修正为 3 秒整秒看门狗，未使用修正版重跑。
- `test/integration/rf_manual_scan_monitor.py`：不触发扫描、仅采集 Web/link/Ping 的观察脚本。
- `test/integration/rf_getstation_race_probe.py`：Web-only、单次 get_station、PM 关闭及 PM 定时器取消的单变量脚本。


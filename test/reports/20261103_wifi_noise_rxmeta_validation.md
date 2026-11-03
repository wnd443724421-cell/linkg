# HI1105 普通接收包 RSSI/SNR/噪声元数据实板验证报告

- 日期：2026-11-03
- STA 管理地址：192.168.3.1
- AP 管理地址：192.168.1.1
- STA/AP 物理 Wi-Fi 地址：11.21.191.2 / 11.21.191.1
- 频率：5785 MHz
- 目标：确认正常 STA/AP 业务接收包是否向 Linux Host 携带可持续读取的每包 RSSI、SNR 或噪声数据，以替代会影响吞吐的主动信道测量。

## 1. 结论

验证结果为 **FAIL：当前 HI1105 普通业务 RX 路径不能直接从 Host HCC CB 取得有效的每包 RSSI/SNR/噪声**。

设备侧接收描述符定义中存在 c_rssi_dbm、snr[]、rssi[]，但普通 Device-to-Host HCC 路径上传的是转换后的 Host mac_rx_ctl_stru。设备侧 dmac_rx_ctl_stru.st_rx_statistic 没有按公共结构位置保留到 Host 可读的 48 字节 CB 中。独立的 RX_SNIFFER_INFO 事件可以携带扩展统计，但属于 sniffer 专用路径，不等同于正常业务 RX。

因此：

1. 当前 Host 驱动不能仅靠读取普通业务包 CB 实现稳定的被动噪声采集。
2. 不能把 dmac_rx_ctl_stru.st_rx_statistic 强制转换用于正常 HCC 包；实测数据是无效/错位字节。
3. 如果必须获得无射频停顿的被动噪声，需要闭源 Device 固件在正常业务路径中聚合并上报 RSSI/SNR，或新增低频缓存事件/ABI。
4. start_chan_meas 主动测量仍不可用于 800 ms 周期生产采集；此前 A/B 已证明会产生吞吐下降、批量发送、抖动和 UDP 丢包。

## 2. 源码依据

### 2.1 Device 侧每包描述符

wifi/inc/hd_common/mac_cb_rx.h 定义：

~~~c
typedef struct {
    int8_t c_rssi_dbm;
    int8_t lp_rssi;
    int8_t snr[2];
    int8_t rssi[2];
} hal_rx_statistic_stru;

typedef struct {
    hal_rx_ctl_stru st_rx_info;
    hal_rx_status_stru st_rx_status;
    hal_rx_statistic_stru st_rx_statistic;
} dmac_rx_ctl_stru;
~~~

其中 Device 侧 hal_rx_ctl_stru 最大约 8 字节；Host 侧因 _PRE_PRODUCT_ID_HI110X_HOST 扩展了帧控制、PN、指针等字段，布局不同。

### 2.2 普通 HCC Host 接收适配

wifi/hmac/hmac_hcc_adapt.c 的 hmac_hcc_rx_event_comm_adapt() 只把 HCC 扩展区复制成 Host mac_rx_ctl_stru：

~~~c
memcpy_s(pst_rx_ctrl, sizeof(mac_rx_ctl_stru),
         puc_hcc_extend_hdr, sizeof(mac_rx_ctl_stru));
~~~

随后移除整个 OAL_MAX_CB_LEN 扩展区。正常业务 RX 没有把 Device st_rx_statistic 保存到长期 Host 缓存。

### 2.3 专用 sniffer 路径

枚举中存在 DMAC_WLAN_DRX_EVENT_SUB_TYPE_RX_SNIFFER_INFO。sniffer/radiotap 路径能接收扩展统计，并用 RSSI/SNR估算噪声；它不是当前普通 STA/AP 数据路径。

## 3. 测试方法

1. 未修改 /home/dawn/t113_tina5.0_v1.2 定版源码。
2. 把 HI1105 驱动复制到 Ubuntu /tmp 独立目录。
3. 仅在独立副本中添加测试探针：
   - 在 hmac_hcc_rx_event_comm_adapt() 移除 HCC 扩展区之前读取公共 dmac_rx_ctl_stru.st_rx_statistic 位置；
   - 内核内按 1 秒聚合样本数量、RSSI 最小/最大/末值、SNR0/1 最小/最大/末值；
   - 每秒最多打印一行；
   - 不扫描、不切换信道、不执行 start_chan_meas、不逐包打印。
4. 使用实板当前正式内核和相同 ARM 交叉工具链构建测试 wifi_1105.ko。
5. 在 STA 上短时启动测试模块。
6. 先观察自然业务/Beacon，再由 AP 向 STA 发送低速验证流量：

~~~sh
ping -I wlan0 -c 100 -i 0.1 -s 1200 11.21.191.2
~~~

## 4. 模块一致性

正式模块：

~~~text
SHA-256 c939eb82733ca98f30c039c2feaf59e4384eb760d6363969544adc433a3bbd12
vermagic 5.4.61 SMP preempt mod_unload ARMv7 p2v8
~~~

测试模块：

~~~text
SHA-256 f9e3a50661f592b36a0056e2dcc1a60cce4e4317df59711c7a7bc05170382dc2
vermagic 5.4.61 SMP preempt mod_unload ARMv7 p2v8
~~~

测试模块包含唯一字符串 RXMETA_PROBE，便于区分。

## 5. 实板结果

### 5.1 自然业务状态

STA 实际状态：

~~~text
Connected to 00:01:02:f7:da:4e
freq: 5785
signal: -22 ～ -23 dBm
rx/tx bitrate: 58.5 ～ 65.0 MBit/s
~~~

探针读取示例：

~~~text
RXMETA_PROBE n=18 rssi=0..0 last=0 snr0=0..109 last=0 snr1=0..9 last=0
RXMETA_PROBE n=19 rssi=0..5 last=0 snr0=0..101 last=101 snr1=0..1 last=0
RXMETA_PROBE n=20 rssi=0..5 last=0 snr0=-74..101 last=0 snr1=0..9 last=0
~~~

判定：

- 实际 RSSI 为约 -22 dBm，探针“RSSI”却主要为 0/5；
- “SNR”出现 101、109、112、-74、-99 等不合理组合；
- 这些字节不是有效的每包无线统计。

### 5.2 AP → STA 低速接收流量

结果：

~~~text
100 packets transmitted
100 received
0% packet loss
rtt min/avg/max/mdev = 1.961/2.416/4.894/0.375 ms
~~~

STA RX 计数在测试窗口内持续增长，实际 RSSI 保持约 -22 dBm。探针字段仍主要为：

~~~text
rssi=0..5
snr0=0..72/78/99
snr1=-40..101 或 0..9
~~~

没有出现与真实 RSSI/SNR相符并随业务包稳定更新的字段。

## 6. 解释

测试最初假设 HCC 48 字节扩展区仍包含完整 Device dmac_rx_ctl_stru。实测否定了该假设。

根因是 Device 与 Host 的 hal_rx_ctl_stru 布局不同。普通固件 HCC 路径已经把 Device RX 控制信息转换为 Host mac_rx_ctl_stru；每包物理层统计没有按 Device 结构继续附在同一位置。强制按 Host 编译的 dmac_rx_ctl_stru 读取时，实际读到的是 Host 控制字段、冗余区或后续数据，因此产生 0、5、72、101、-74 等错位值。

## 7. 运行时风险发现

当前驱动运行时执行 rmmod wifi_1105 会卡在：

~~~text
kthread_stop
~~~

模块状态显示 Unloading，同时出现 sysfs group 'vap' not found 告警。因此后续 HI1105 驱动替换测试不应采用运行时卸载/重载，应使用可恢复的启动模块切换并重启。

根文件系统 overlay 仅 10 MiB。测试过程中尝试额外复制完整模块时出现 No space left on device，SHA 检查发现中间文件被截断。截断文件在重启前即被完整正式备份恢复，未用截断模块启动。之后通过 v2 中同 SHA 正式模块恢复 v1。

## 8. 最终恢复验收

测试结束后 STA 已恢复正式模块：

~~~text
wifi_1105 module size: 2338816
SHA-256: c939eb82733ca98f30c039c2feaf59e4384eb760d6363969544adc433a3bbd12
RXMETA_PROBE marker: absent
Supervisor/LinkG/wpa_supplicant: running
~~~

无线状态：

~~~text
signal: -22 dBm
rx bitrate: 65.0 MBit/s
tx bitrate: 65.0 MBit/s
~~~

STA → AP 恢复验证：

~~~text
10 packets transmitted
10 received
0% packet loss
rtt min/avg/max/mdev = 1.384/1.649/1.913/0.167 ms
~~~

没有残留 rmmod、iperf3、start_chan_meas、gst-launch、测试 ping 或板端测试模块副本。

## 9. 后续可行方向

优先级建议：

1. **固件被动聚合上报**：Device 在正常 RX 描述符产生时聚合 RSSI/SNR，只以低频缓存事件上报 Host；不会切信道，最符合实时业务要求。
2. **验证 WLAN_CFGID_QUERY_RSSI**：该应答包含 c_rssi/c_free_power，但必须确认底层缓存是否独立于会冻结的 s_free_power，以及查询是否无射频停顿。
3. **验证 station-info 的 SNR 字段**：当前应答已有 c_snr_ant0/1，需要临时只读导出并观察在 s_free_power 冻结后是否仍更新。
4. **不采用 sniffer 作为生产周期接口**：其扩展元数据可用于实验标定，但需要专用模式，不能先验认为对业务吞吐零影响。

本报告结论边界：已证明“当前普通 Host HCC CB 的公共 dmac_rx_ctl_stru 位置不可作为每包 RSSI/SNR/噪声来源”；尚未证明闭源固件内部没有其他可查询的被动缓存。

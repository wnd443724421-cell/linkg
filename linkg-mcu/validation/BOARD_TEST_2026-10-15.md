# LinkG MCU V2.0 实板功能验证

测试时间：2026-10-15（Asia/Shanghai）。

## 测试对象和边界

- 用户确认已经烧录本次 MCU 固件，并保持 `192.168.3.1` 的外部 UART0 TX/RX 短接。
- 交付镜像为 `build/gcc/linkg_mcu_v2.bin`，SHA256：

  ```text
  36ab48c1ea86e09091571495fc50ed227474ff6da6d2204ef88ff124d4b867c4
  ```

- 测试机没有检测到 ST-LINK、STM32CubeProgrammer、OpenOCD 或 pyOCD，未能通过 SWD 回读 Flash。因此，只有用户实际烧录文件与上述镜像相同时，本报告的实板结果才与该交付镜像一一对应。
- LinkG A 管理地址为 `192.168.1.1`，LinkG B 管理地址为 `192.168.3.1`。实际测试脚本开始时，两块板的 Web 版本均为 `20261015-uart-5`。
- 主动测试只修改 LinkG B 的 UART0 配置。每项测试前保存原配置，失败和成功路径均执行恢复；最终配置逐字段相同，再次提交原配置返回 `apply=none`。

## 测试前状态

LinkG B UART0：

```text
type          = normal
device        = /dev/ttyS4
baud          = 115200
format        = 8N1
route         = UDP local RX/TX 172.28.2.1:7000
device_open   = true
route_state   = waiting_peer
errors        = 0
drops         = 0
```

LinkG A UART0 在本次脚本开始前已有 5 个错误、0 个丢弃。脚本没有修改 LinkG A；测试结束仍为 5 个错误、0 个丢弃，没有新增异常。

## 实际结果

| 项目 | 输入与操作 | 实际结果 |
|---|---|---|
| Normal 烧录后基线 | 115200/8N1；10 组，长度 1、2、3、7、16、25、64、127、255、512 | 合计 1,012 字节逐字节一致；四组计数增量均为 1,012；错误和丢弃均为 0 |
| SBUS 连续与突发 | 100000/8E2；64 帧逐帧发送，再用一个 UDP 数据报提交 4 帧 | 68 帧、1,700 字节按原顺序逐字节一致；数据区覆盖 `0x00`～`0xFF`；四组计数均为 1,700；错误和丢弃均为 0 |
| SBUS 波特率回读 | Linux `/dev/ttyS4` 驱动回读 | `baud_base=1500000`、`divisor=15`、`actual=100000`、`error_pct=0` |
| 模式切换 | 在初始 Normal、持续 SBUS、3 组额外 Normal/SBUS、最终 Normal 之间切换 | 共完成 4 个 Normal→SBUS→Normal 往返，8 次类型切换；每个阶段均成功打开 UART、启动或停止 SBUS 线程并完成精确回环 |
| SBUS 持续负载 | 2,000 帧逐帧检查，再提交 4 帧突发 | 2,004 帧、50,100 字节全部一致；数据区覆盖全部 256 个字节值；四组计数均为 50,100；错误和丢弃均为 0 |
| 配置恢复 | 恢复原始 UART JSON，再重复提交 | 配置逐字段相同；UART 服务与 Worker 保持运行；重复提交返回 `apply=none` |

第一轮 SBUS 单帧往返时间：

```text
minimum = 7.076 ms
median  = 7.640 ms
maximum = 28.485 ms
```

持续负载共运行 16.422 s，单帧往返时间：

```text
minimum = 5.438 ms
median  = 8.162 ms
maximum = 24.249 ms
```

该时间包含 Windows UDP、LinkG 网络处理、Linux SBUS FIFO 预热和 7.6 ms 调度、T113 UART、板上电气路径以及返回网络的总时间，不能当作 MCU 单独的逐字节延迟。首帧可能包含 Wi-Fi profile 目标水位的预热等待。

两轮实测合计完成：

```text
Normal 回环：5,060 字节
SBUS 回环：2,087 帧 / 52,175 字节
模式切换：8 次类型切换
```

所有有效数据阶段均满足：

```text
net_rx_bytes == uart_tx_bytes == uart_rx_bytes == net_tx_bytes == 实际测试字节数
errors 增量 == 0
drops  增量 == 0
```

## 已经证实的范围

1. 烧录后 MCU 不影响 Normal 115200/8N1 直通，外部 UART0 回环在多种长度和二进制内容下保持字节顺序与内容。
2. 切换 SBUS 后，网络、Linux SBUS 校验/FIFO/调度、T113 UART 和板上外部短接构成的端到端路径能够以 100000/8E2 连续传输真实 25 字节帧。
3. SBUS 有效载荷覆盖所有 256 个字节值，没有观察到 MCU 侧协议解析、内容改写、固定 25 字节重组或伪造数据。
4. 连续 2,004 帧没有丢失、重复、乱序、Linux I/O 错误或队列丢弃。
5. 多次 Normal/SBUS 热切换后均能恢复收发，LinkG Web、UART 服务和另一块板保持运行，最终配置恢复正确。

## 尚未由本测试证实的范围

外部 TX/RX 短接回环能强力覆盖预期 MCU 链路，但不能唯一证明数据只经过 MCU。如果 U16 异常保持导通，普通直通可能形成替代回路。因此，下列项目仍需示波器、逻辑分析仪或 SWD：

1. PA1/MCU_MODEL 为高时 PB1/CTL 实际为低，U16 确实断开且不存在两条路径同时驱动。
2. PA9/PA10 的 Host 侧为正常极性，PA2/PA3 的外部侧为反相极性；外部 TX 空闲低、Host TX 空闲高。
3. 实际位宽约 10 μs、8E2 字符约 120 μs，以及 HSI 在电压和温度范围内的绝对频差。
4. MCU 单独的中断转发延迟、32 字节 FIFO 高水位、持续满裸串口带宽下的两侧时钟偏差积累。
5. 通过 BREAK 或外部信号源注入 PE、FE、NE、ORE 后，错误统计、坏字节丢弃和下一字节恢复。
6. 阻塞主循环后 IWDG 的真实复位时间，以及复位、烧录和掉电期间 R118 NC 导致的 CTL 不确定窗口。
7. 通过 SWD 查看 `g_bridge.stats`，确认 MCU 两个方向的 `rx_bytes`、`tx_bytes`、`overflow_bytes` 和 `high_water_bytes`。

## 复现与证据文件

实测脚本：

```text
tests/hardware/board_mcu_validation.py
tests/hardware/board_mcu_soak.py
```

执行命令：

```powershell
python .\tests\hardware\board_mcu_validation.py
python .\tests\hardware\board_mcu_soak.py
```

原始 JSON 证据：

```text
validation/board-mcu-pretest-snapshot.json
validation/board-mcu-validation.json
validation/board-mcu-soak.json
```


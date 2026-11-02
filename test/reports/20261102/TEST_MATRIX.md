# LinkG V1 原清单覆盖矩阵

状态说明：`PASS` 表示列出的实际执行子项符合预期；`BLOCKED` 表示受当前环境或证据限制无法完成；`N/A` 表示按用户本轮明确范围不执行。一个大模块可同时包含 PASS 子项和 N/A/BLOCKED 剩余项，不用“部分通过”掩盖边界。

## T01～T16

| 编号 | 本轮实际执行及结果 | 未执行边界 |
|---|---|---|
| T01 基础工具与配置 | PASS：文本、时间、文件/原子文件、错误输入、Device/Network/Path/Link/Wi-Fi/Cellular/UART 配置，115 个相关断言 | `linkg_thread` 独立生命周期未单测；由 Supervisor/板端线程稳定性提供间接证据 |
| T02 Packet Pool | PASS：参数、耗尽、批量、引用、对齐、headroom、Push/Pull、busy deinit、全回收，51 断言 | 压力循环 N/A |
| T03 Node/Path/Link | PASS：角色、Peer 容量、Path 更新/统计、引用、延迟退役、Peer 注销，79 断言 | Link Manager 并发竞争与实际链路反复启停 N/A |
| T04 Transport | PASS：Wire、窗口、回绕、分片重组、最大包、Epoch、Peer reset，62 断言；视频数据面真实经过 Transport | Reorder/Forward/Relay、队列耗尽、并发 reset 的完整隔离套件未建立；压力 N/A |
| T05 Probe | PASS：Wire 非法输入；板端三业务 Probe 均显示可达 | RTT/jitter 诊断、异步取消/超时全状态未独立覆盖 |
| T06 Scheduler | PASS：实际视频经当前 Wi-Fi SINGLE 默认计划发送 | REDUNDANT、SPECIFIED、Maintenance 和批量部分失败未执行；当前 STA Cellular Path 不可用 |
| T07 TUN/QoS | PASS：真实 `linkg0` 双板同窗计数与视频方向一致 | 三类 QoS 队列、公平性、溢出和内核队列压力 N/A |
| T08 Switch Policy | PASS：Wire 4,129 断言；Verify 事务/时长/提前故障/幂等/清理 39 断言；板端计划状态正常 | LOSS/LOAD 积分、完整 Operation/PlanSync、真实双链路切换未执行；当前无可用 Cellular Path |
| T09 Discovery | PASS：STA_REPORT/AP_SYNC/LEAVE、拓扑边界和畸形语料；板端 AP/STA Peer 在线 | Session 冲突、通道老化、Peer 反复上下线 N/A |
| T10 Fast NAT | PASS：RK 到 AP Ethernet 的跨节点 RTP/H.264 实际数据面通过 | Related Flow、RTSP 控制、MSS、容量和驱动装卸未执行 |
| T11 DHCP/Network | PASS：RK 当前动态租约、Option 路由、网关/虚拟网可达 | 租约文件故障、进程崩溃恢复、重复启停 N/A |
| T12 Wi-Fi | PASS：AP/STA 关联、状态/统计/Probe、180 秒短窗稳定，接口 error 0 | 扫描失败、驱动装卸、故障重启和反复生命周期 N/A |
| T13 Cellular | PASS：两板 SIM ready、注册和 PDP/IP 状态可读取；AP 5G SA、STA LTE | SIM 拔插、PDP 故障重建、接口反复启停 N/A；STA Cellular Path 当前 inactive |
| T14 UART/SBUS | PASS：配置边界；MCU 7 动态测试；两板 UART0 normal/SBUS 共 4 子项；诊断后业务恢复 | UART1/2 实际回环、多目标跨节点 UART 未执行；用户说明 UART0 足够，本轮其余硬件项 N/A |
| T15 Supervisor/Upgrade | PASS：Supervisor 41 场景、Release 38 场景 | production timing、ENV lifecycle 计时、实际 OTA/MTD/断电/U-Boot 回退 N/A |
| T16 Web/System | PASS：56 次只读 GET、8 次非法命令/任务拒绝、2 次非法 UART 参数拒绝、UART 诊断启动/轮询/恢复 | 实际改配置、升级、重启、恢复出厂未执行，以免扩大破坏性范围 |

## I01～I16

| 编号 | 状态 | 证据与边界 |
|---|---|---|
| I01 Wi-Fi 生命周期 | PASS（当前运行）/ N/A（反复启停） | AP/STA 关联、状态、探测及 180 秒稳定；未重启驱动/WPA |
| I02 Cellular 生命周期 | PASS（当前运行）/ N/A（异常恢复） | SIM、注册、PDP/IP 当前正常；未拔插或断链 |
| I03 Discovery 与 Node | PASS | AP Node1 与 STA Node2 互见，Peer/Path 状态有效 |
| I04 Discovery 与 Route | PASS（静态当前拓扑）/ N/A（上下线） | AP/STA 拓扑与路由可读；未制造节点上下线 |
| I05 TUN 与 Transport | PASS | 20 秒真实 RTP 数据经两板 `linkg0`，方向计数一致 |
| I06 Scheduler 与 Transport | PASS（Wi-Fi SINGLE）/ N/A（其他策略） | 视频实际发送；未测 REDUNDANT/SPECIFIED |
| I07 Switch 与 Scheduler | N/A | 当前无可用 STA Cellular Path，未进行真实切换 |
| I08 QoS 与 TUN | N/A | 用户自行做压力/多业务测试 |
| I09 Fast NAT 与 TUN | PASS（基础跨节点 UDP） | RK `192.168.3.3` 发往 `172.28.1.100` 并落到 AP Ethernet；高级 NAT 场景未测 |
| I10 Fast NAT 与 RTSP | N/A | 本轮为 RTP/UDP 数据流，无 RTSP TCP 协商 |
| I11 DHCP 与 Ethernet | PASS | RK 动态地址及 `172.28.0.0/16 via 192.168.3.1` 路由有效 |
| I12 UART 与 Transport | N/A | UART0 做本地硬件回环，未做跨节点 UDP 串口流 |
| I13 UART 与 MCU | PASS | 两板 UART0 SBUS 各 20/20 帧，MCU 桥接主机逻辑通过 |
| I14 UART 诊断与业务管理 | PASS | 生产 Web 任务启动/轮询，diagnostic 独占后 `uart_restored=true`，状态前后一致 |
| I15 Upgrade 与 Supervisor | PASS（模拟逻辑）/ N/A（真实升级） | 38 个 Release 场景；未写 MTD、重启或断电 |
| I16 Web 与系统管理 | PASS（读取/诊断）/ N/A（破坏性写操作） | GET、非法输入、UART 任务通过；未改配置/升级/重启 |

## 性能、稳定性、外场和硬件专项

| 清单章节 | 状态 | 原因 |
|---|---|---|
| 0.6 通信性能 | N/A | 用户明确压力/性能自行测试；本轮数据不作为吞吐、P95/P99 或 CPU 指标 |
| 0.7 多链路切换性能 | N/A | 当前 STA Cellular Path 不可用，且用户不要求性能测试 |
| 0.8 无线及外场 | N/A | 需要外场、距离、遮挡和专用设备 |
| 0.9 长时间运行/压力 | N/A | 用户要求仅分钟级逻辑与短时 Bug 检查 |
| 视频完整解码与画面 | BLOCKED | Ubuntu 缺 `h264parse` 和 `avdec_h264`；仅 RTP/H.264 包和实际数据面 PASS |
| 实际 OTA/Kernel/看门狗 | N/A | 涉及 MTD、重启、断电与硬件看门狗，本轮不执行 |

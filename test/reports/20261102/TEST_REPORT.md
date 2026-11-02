# LinkG V1 软件逻辑与短时运行测试报告

## 1. 结论

本轮约定范围内的测试全部通过，未发现能够在当前条件下复现的产品逻辑缺陷：

- 定版产品源码完成 x86 Release 树外构建，181 个构建步骤完成。
- 新建 8 个独立逻辑测试程序，26 个用例、25,002 个断言全部通过；编译使用 `-Wall -Wextra -Wpedantic -Werror`，运行启用 AddressSanitizer、UndefinedBehaviorSanitizer 和 LeakSanitizer。
- Supervisor 两组非计时逻辑测试共 79 个场景通过：进程管理 41/41，Release 管理 38/38。
- MCU/SBUS 7 个动态逻辑测试通过，覆盖 20,000 字节有序转发、溢出、错误、切换、重试和看门狗；15 个 C/H 文件、64 个函数说明和 22 个 Keil 输入的一致性检查通过。
- 两块 LinkG 板完成 180 秒间隔的双快照短测：进程 PID、线程数、FD 数不变，RSS 各增加 20 KiB，所有采样接口错误计数为 0。
- 两块板的 UART0 normal 与 SBUS 新鲜回环测试全部通过，测试后业务 UART 自动恢复。
- RK DHCP 当前租约、Option 路由和跨节点可达性通过；20 秒摄像头 RTP/H.264 数据流经两块 LinkG 板到达 Ubuntu，收到 2,716 个 RTP 缓冲，同窗物理接口与 `linkg0` 方向计数一致，错误计数为 0。

这不是完整整机验收。按用户要求，压力、吞吐性能、长时间运行、外场、反复故障注入和专用硬件测试均未执行；Ubuntu 缺少 `h264parse` 与 `avdec_h264`，因此视频仅验证了摄像头编码、RTP 数据包接收和 LinkG 数据面，没有完成本机解码、显示、FPS、卡顿或画质验收。

## 2. 测试基线与可追溯性

| 项目 | 实际值 |
|---|---|
| 源码目录 | `/home/dawn/project/linkg_v1` |
| 分支 / Commit | `main` / `a1e74eda5fee1b3ab5fb62460ac3324c609f6c52` |
| 产品源码差异 | `core/`、`infra/`、`include/`、`modules/`、`platform/`、`service/`、`supervisor/`、`third_party/`、`linkg-mcu/` 无 tracked diff |
| Ubuntu | Linux x86_64，GCC 13.3.0，CMake 3.28.3，Ninja 1.11.1，Python 3.12.3 |
| LinkG AP | `192.168.1.1`，Node 1 / AP，Linux 5.4.61，`/app/releases/v2` |
| LinkG STA | `192.168.3.1`，Node 2 / STA，Linux 5.4.61，`/app/releases/v1` |
| RK 摄像头板 | `172.28.2.3`，Linux 6.1.75，`/dev/video51` |
| 两块 LinkG 运行二进制 | SHA-256 `325ab0c7196e0e4e9f94173fb2ab6a522debe17e9727d9505c25ffd1d18c28d1` |

两块板的运行二进制哈希相同，并与仓库现有 `upgrade/app/bin/linkg` 相同；但该二进制在测试开始前已经是 Git 修改状态，因此板端结果可精确追溯到上述 SHA-256，不能仅凭现有证据证明它由 Commit `a1e74eda` 可重复生成。源码逻辑测试则直接针对该 Commit 当前未修改的产品源码编译。

测试开始前已有的 `test/*` 删除、`upgrade/app/bin/linkg` 修改和升级包修改均保留。本轮只新增 `test/` 下的测试与报告，没有提交或推送。

## 3. 独立逻辑测试结果

| 套件 | 用例数 | 断言数 | 结果 | 主要对象 |
|---|---:|---:|---|---|
| base | 3 | 47 | PASS | 文本键值、时间、文件与原子文件 |
| packet_pool | 2 | 51 | PASS | 参数、分配耗尽、引用、Push/Pull、回收 |
| node_path | 3 | 79 | PASS | Peer 约束、Path 统计、延迟退役与引用释放 |
| transport | 3 | 62 | PASS | Wire、接收窗口、回绕、分片重组、Epoch 隔离 |
| wire_protocols | 4 | 20,527 | PASS | Probe、Discovery、4,096 组畸形语料，每组 5 个解码入口 |
| switch_wire | 4 | 4,129 | PASS | Plan/Quality/ACK/Maintenance 与 4,096 组畸形语料 |
| switch_verify | 3 | 39 | PASS | 事务校验、3秒/5秒选择、故障终态、幂等、Probe 清理 |
| config | 4 | 68 | PASS | Device/Network/Path/Wi-Fi/Cellular/Link/UART 配置 |
| **合计** | **26** | **25,002** | **PASS** | 0 sanitizer 报告，0 测试失败 |

### 3.1 用例明细

所有用例均以 Commit `a1e74eda`、Ubuntu x86_64 为环境；实际结果的逐断言输出见 `logic_ctest.log`。

| 测试编号 | 对象与输入 | 预期结果 | 实际结果 | 状态 |
|---|---|---|---|---|
| UT-BAS-001 | 文本键值替换、追加、删除；不存在键、非法键、换行值 | 正常编辑精确匹配；非法输入返回对应错误 | 符合，套件输出 `PASS test_text_edit` | PASS |
| UT-BAS-002 | 毫秒/微秒格式、短缓冲、单调时间和 2ms 睡眠 | 格式正确；短缓冲拒绝；时间不倒退 | 符合 | PASS |
| UT-BAS-003 | 临时目录内原子写读、长度上限、二进制 NUL、符号链接、删除后读取 | 数据一致；EFBIG/EINVAL/ELOOP/ENOENT 精确返回；临时文件清理 | 符合 | PASS |
| UT-POOL-001 | NULL、零容量、非法 headroom/slot 参数 | 全部拒绝且不产生池资源 | 符合 | PASS |
| UT-POOL-002 | 8 槽池、64 字节对齐、Push/Pull 边界、retain/release、耗尽和忙反初始化 | 生命周期与计数精确；耗尽返回 NULL；最终 8 槽全部回收 | 符合 | PASS |
| UT-NODE-001 | 独立 Path 激活、分类统计、2 个引用、退役、重复 release、reset | 最后引用触发一次回调；统计和状态转换正确 | 符合 | PASS |
| UT-NODE-002 | AP 注册 STA、重复注册、2 条 Path、第三条拒绝、持有引用时注销 Peer | 容量与角色约束生效；Peer 逻辑下线；引用释放后才回收 | 符合 | PASS |
| UT-NODE-003 | STA 连续注册两个 AP Peer | 仅允许一个直接 AP，第二个返回 ENOSPC | 符合 | PASS |
| UT-TRN-001 | Transport Header 正常、魔数/版本/类型/标志/Node/保留位/截断和序列更新 | 合法报文往返一致；所有非法字段拒绝 | 符合 | PASS |
| UT-TRN-002 | 连续、跳跃、重复、超窗、重置和 `UINT32_MAX` 回绕 | 窗口判定及确认丢包数正确 | 符合 | PASS |
| UT-TRN-003 | 最大包两片重组、乱序提交、旧 Epoch、Peer reset 和重复 init/deinit | 负载逐字节一致；旧代际拒绝；缓存和池全部释放 | 符合 | PASS |
| UT-WIRE-001 | Probe 请求编解码、魔数/类型/序列/长度非法 | 合法往返；异常精确拒绝 | 符合 | PASS |
| UT-WIRE-002 | STA_REPORT 含 IPv4/IPv6/MAC；错误头、类型、长度和地址族 | 合法字段一致；损坏报文拒绝 | 符合 | PASS |
| UT-WIRE-003 | AP_SYNC 拓扑、节点上限、PEER_LEAVE | 拓扑和会话字段一致；超限/截断拒绝 | 符合 | PASS |
| UT-WIRE-004 | 固定种子生成 4,096 个 0～最大长度随机缓冲，调用 5 个 Discovery/Probe 解码入口 | 无越界、崩溃或 sanitizer 报告；返回值不为正 | 20,480 次解码断言通过 | PASS |
| UT-SW-001 | SINGLE/REDUNDANT 三业务 Plan 往返、重复链路、非法版本/长度/ID | 合法计划一致；非法组合拒绝 | 符合 | PASS |
| UT-SW-002 | Quality Report、样本/丢包关系、HCC flag | 合法统计一致；矛盾统计拒绝 | 符合 | PASS |
| UT-SW-003 | Plan ACK、Maintenance BEGIN、Maintenance ACK 及保留字段 | 状态和接入类型一致；非法值拒绝 | 符合 | PASS |
| UT-SW-004 | 固定种子生成 4,096 个随机 Switch Wire 缓冲 | 解码无越界、崩溃或 sanitizer 报告 | 4,096 次断言通过 | PASS |
| UT-VERIFY-001 | LOSS 与 RECOVERY+LOAD 时长、Peer/Class/Operation ID、观测新鲜度和双链路校验 | 3秒/5秒选择正确；非法事务拒绝 | 符合 | PASS |
| UT-VERIFY-002 | 验证中 Wi-Fi 明确不可用、Cellular 可用；重复 process | 提前固定 CELLULAR；重复交付相同 VERIFY_DONE | 符合 | PASS |
| UT-VERIFY-003 | RECOVERY+LOAD 两类业务 Probe 提交和 reset | 每类 5 分起始；两个 Handle 建立并在 reset 取消 | 符合 | PASS |
| UT-CFG-001 | 字符串/数值边界、Device 默认与 JSON 往返、错误类型 | 合法值通过；非法角色/类型拒绝 | 符合 | PASS |
| UT-CFG-002 | `172.30.0.0` 虚拟网、Node 2 地址/反解、Node 254、网段冲突 | 地址计算正确；非法或冲突网络拒绝 | 符合 | PASS |
| UT-CFG-003 | Path/Wi-Fi/Cellular/Link 默认、JSON 往返及非法优先级/信道/模式 | 合法配置一致；边界拒绝 | 符合 | PASS |
| UT-CFG-004 | UART 默认、保留端口、UDP 远端路由、SBUS/normal 约束和 JSON 往返 | 合法配置一致；冲突/非法配置拒绝 | 符合 | PASS |

## 4. Supervisor 与 MCU/SBUS

### 4.1 Supervisor

仅执行用户要求的逻辑场景，未把 production timing 或 ENV lifecycle 计时套件计入报告。

| 测试编号 | 内容 | 实际结果 | 状态 |
|---|---|---|---|
| SUP-LOGIC-001 | 启动、READY/HEARTBEAT、退出/信号、重启、STOPPING、旧 PID 回收、锁和清理故障注入 | 41/41 场景通过，10.67s | PASS |
| SUP-RELEASE-001 | 稳定/候选、确认、READY/心跳超时、同/跨 Kernel 回退、损坏 JSON、ENV/JSON 故障、进程回收和中断恢复 | 38/38 场景通过，30.59s | PASS |

### 4.2 MCU/SBUS

| 测试编号 | 内容 | 实际结果 | 状态 |
|---|---|---|---|
| MCU-SBUS-001 | 透明桥接、20,000 字节顺序、缓冲溢出、UART 错误、模式切换、启动重试、看门狗 | 7 个动态测试通过 | PASS |
| MCU-AUDIT-001 | C/H 格式、函数说明、Keil/GCC 输入独立性与源一致性 | 15 文件、64 函数、22 输入通过 | PASS |

## 5. 板端短时集成结果

### 5.1 180 秒双快照

两块 LinkG 均保持同一 Supervisor 和 LinkG PID；共执行 56 次只读 Web GET（14 命令 × 2 板 × 2 快照），全部返回 `code=ok`。8 次非法命令/不存在任务检查均被明确拒绝。Web 覆盖 Header、Overview、Switch、Wi-Fi、Cellular、Traffic、UART、Network、Device、Upgrade 和 UART Diagnostic 状态。

| 指标 | AP `192.168.1.1` | STA `192.168.3.1` |
|---|---:|---:|
| LinkG PID | 1759 → 1759 | 1783 → 1783 |
| Threads | 23 → 23 | 25 → 25 |
| FD | 56 → 56 | 64 → 64 |
| VmRSS | 7552 → 7572 KiB | 7768 → 7788 KiB |
| 网卡 RX/TX error | 全部 0 → 0 | 全部 0 → 0 |
| Wi-Fi | AP，1 个关联 Peer | STA 已连接，采样信号 -41～-37 dBm |
| 当前发送计划 | 三业务 Wi-Fi SINGLE | 三业务 Wi-Fi SINGLE |

RSS 的 20 KiB 变化只说明该短窗口未观察到明显持续增长，不能外推为无泄漏或长稳通过。

### 5.2 UART0 生产诊断

| 测试编号 | 板 | normal | SBUS | 恢复 | 状态 |
|---|---|---|---|---|---|
| HW-UART-AP-001 | AP | 1536 TX / 1536 RX，188ms | 20/20 帧，500 TX / 500 RX，230ms | `uart_restored=true`，前后均 running | PASS |
| HW-UART-STA-001 | STA | 1536 TX / 1536 RX，188ms | 20/20 帧，500 TX / 500 RX，230ms | `uart_restored=true`，前后均 running | PASS |
| WEB-UART-VAL-001 | 两板 | `uart_num=3` | 参数校验 | 均返回明确错误 | PASS |

STA 的旧任务曾保存 UART0 超时结果，但在当前接线下的新任务已通过，因此旧结果不作为当前缺陷。

### 5.3 RK DHCP 与视频数据面

| 测试编号 | 输入 | 预期 | 实际 | 状态 |
|---|---|---|---|---|
| INT-DHCP-001 | RK `end0` 接入 STA Ethernet | 获得 `192.168.3.0/24` 租约和跨节点路由 | `192.168.3.3/24`；`172.28.0.0/16 via 192.168.3.1` | PASS |
| INT-DHCP-002 | RK ping STA 网关与 AP 虚拟地址 | 均可达 | 网关平均 0.489ms；`172.28.1.1` 平均 2.779ms，均 0% 丢包（3 包短样本） | PASS |
| INT-VIDEO-001 | `/dev/video51`，NV12 1920×1080@30，MPP H.264，RTP/UDP 5600，20 秒 | 编码器启动；RTP 到 Ubuntu；路径计数同向增长；无接口错误 | 发送器形成 High Profile 1080p@30 caps；接收 2,716 个 RTP 缓冲；无 GStreamer element error；接口错误 0 | PASS |

视频同窗关键增量：STA `eth0 RX +15,346,178`、`linkg0 TX +15,179,779`、`wlan0 TX +16,066,592`；AP `wlan0 RX +16,066,718`、`linkg0 RX +15,179,831`、`eth0 TX +15,353,019` 字节。该方向链条与 RK→STA→AP→Ubuntu 的实际数据流一致。计数含协议开销与窗口内少量背景流量，因此不作为吞吐性能值。

## 6. 问题与限制

| 编号 | 类型 | 说明 | 处理结论 |
|---|---|---|---|
| ENV-001 | 测试环境 | Ubuntu GStreamer 缺少 `h264parse`、`avdec_h264` | RTP/H.264 数据面已通过；解码、显示、FPS、卡顿和画质为 BLOCKED，不记为产品 FAIL |
| TRACE-001 | 可追溯性 | `upgrade/app/bin/linkg` 测试前已为 Git 修改状态 | 两板与该文件 SHA-256 一致；板测绑定哈希，不宣称部署物由 Commit 可重复生成 |
| SCOPE-001 | 用户约定 | 压力、性能、24/72h、外场及专用硬件测试不执行 | 标记 N/A，不纳入本轮通过率 |
| SCOPE-002 | 风险控制 | 未执行实际 OTA/Kernel 写入、断电回退、服务反复重启、SIM 拔插和 Wi-Fi 故障注入 | 只采用非破坏性逻辑场景与当前运行状态；不宣称整机恢复验收完成 |

## 7. 最终判定

本轮“逻辑正确性 + 分钟级短时运行是否暴露 Bug”的约定目标已完成，所有实际执行项为 PASS，未形成产品 FAIL。当前证据支持：核心 Wire/内存/生命周期/Verify/配置逻辑、Supervisor 主要异常分支、MCU/SBUS 主机逻辑、双板基本运行、UART0 回环、DHCP 当前配置以及真实跨节点视频数据面在短窗口内工作正常。

本结论不能替代压力、性能、长稳、完整双链路切换、OTA/Kernel、外场或视频主观质量验收；这些项目见 `TEST_MATRIX.md` 的 N/A/BLOCKED 边界。

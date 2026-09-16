# LinkG Path Probe 模块

本包实现四个职责文件：`linkg_path_probe.c`、`path_probe_runtime.c`、`path_probe_tx.c`、`path_probe_rx.c`。Pending 管理和多包诊断合并在 `path_probe_runtime.c`，不再另拆 `pending.c`、`diagnostic.c`。

同时提供必须配套的公开头、内部头和已有 8 字节协议的 Wire 实现。没有修改 Web、切换决策、Discovery 或用户业务转发策略。

## 1. 本次确定的行为

| 角色与路径 | REALTIME | VIDEO | DATA |
| --- | ---: | ---: | ---: |
| AP 的 Wi-Fi Path | 1000 ms | 1000 ms | 1000 ms |
| AP 的 Cellular Path | 1000 ms | 1000 ms | 1000 ms |
| STA 的 Wi-Fi Path | 1000 ms | 1000 ms | 1000 ms |
| STA 的备用 Cellular Path | 1000 ms | 1000 ms | 1000 ms |
| STA 的主用 Cellular Path，真实业务PPS <= 100 | 100 ms | 100 ms | 100 ms |
| STA 的主用 Cellular Path，真实业务PPS > 100 | 1000 ms | 1000 ms | 1000 ms |
| 不存在的业务 Path | 不发送 | 不发送 | 不发送 |

“主用 Cellular”使用该直接 Peer 的 `linkg_switch_get_plan()` 结果：发送模式为 SINGLE 或 REDUNDANT，且 `primary_link_id` 是本地 Cellular Link。只有 secondary 是 Cellular 不触发快速周期。

PPS策略参考原 `cellular_link_heartbeat.c`：以 1 秒窗口统计当前 Cellular Path 的真实业务PPS。Path累计统计本身会包含 Probe，因此模块维护本Path的Probe收发累计并在计算PPS时扣除；`PPS <= 100` 时保持 100 ms，`PPS > 100` 时恢复 1 s。AP始终为1 s，不参与100 ms快速策略。

AP 最多跟踪 16 个直接 STA；STA 仅跟踪直接 AP。不会依据 AP 拓扑快照向其他 STA 发送探测。AP、STA 都能响应对端请求；主动多包诊断仅允许 STA 发起，同一模块同时只接受一个诊断任务。

周期按到期时间推进，不等待前一个响应。首次探测分散到周期内，避免 AP 对全部节点同时集中发包。操作系统调度、锁竞争和 Scheduler 执行时间会影响实际时刻；这些间隔不是硬实时保证。

## 2. 文件放置

```text
core/path_probe/
    linkg_path_probe.c
    path_probe_runtime.c
    path_probe_rx.c
    path_probe_tx.c
    path_probe_wire.c
    path_probe_internal.h

include/linkg/core/path_probe/
    linkg_path_probe.h
```

如果项目已经使用 `core/probe/`，可以继续沿用原目录，将同名文件替换到原位置。不要同时保留 `core/probe/` 和 `core/path_probe/` 两套实现，否则递归构建会遇到重复符号。

**两个头文件必须一起替换。** 内部头补齐了跨文件声明、发送提交状态、运行代际、诊断样本状态及停止排空需要的字段。旧的单 Pending 头文件不能搭配本包使用。

`path_probe_wire.c` 与已经确定的协议一致：

```text
0..2  "LGP"
3     REQUEST=1 / RESPONSE=2
4..7  sequence，网络字节序，0无效
```

这份 Wire 实现增加 Packet 数据边界检查，不改变协议。Magic 只用于格式校验，不提供对端认证。

## 3. 已有工程必须满足的接口条件

### Transport

必须存在 `LINKG_TRANSPORT_TYPE_PATH_PROBE`。本次验证使用：

```c
typedef enum
{
    LINKG_TRANSPORT_TYPE_NONE       = 0,
    LINKG_TRANSPORT_TYPE_USER_DATA  = 1,
    LINKG_TRANSPORT_TYPE_PATH_PROBE = 2,
    LINKG_TRANSPORT_TYPE_COUNT      = 3
} linkg_transport_type_t;
```

`linkg_transport_delivery_t` 必须包含：

```c
uint32_t ingress_link_id;
```

普通本机交付携带真实本地入站 Link ID；不能唯一归属某条物理 Link 的重组交付使用 `LINKG_LINK_ID_INVALID`。Probe RX 拒绝无效入站 Link，也拒绝 `source_node_id != peer_node_id` 的转发来源。

你前面已经完成这一段时，不要再次修改 Wire 协议。`validation/transport_prerequisite.diff` 只是本包复现旧源码基线编译所用的补丁，不是要求覆盖你现在已修改好的 Transport。

### Scheduler / Node / Packet

使用已有 `linkg_scheduler_submit()`，指定 `LINKG_SCHEDULER_POLICY_SPECIFIED`。Probe 不新开 Socket，不访问驱动私有发送函数，不改变默认 Send Plan。

发送使用新申请的 Packet。Scheduler 同步借用 Packet，本模块在提交返回后释放自己的基础引用。Node Path 仅短时获取、立即释放，不在等待响应期间持有 Path 引用。

`sent_packets` 表示 Scheduler 成功接受的数量，**不是网卡硬件实际出线数量**。现有 Link 可能把发送暂时放入队列，因此 RTT 从 Scheduler 提交前开始计时，包含本机软件处理、队列等待、网络往返和对端回复处理。

## 4. 应用生命周期接入

公开头：

```c
#include "linkg_path_probe.h"
```

按现有应用初始化/清理框架加入以下调用，并同步维护应用层的 `probe_initialized`、`probe_started` 标志及失败回滚检查。不要从 Web Handler 自行编排模块生命周期。

```c
ret = linkg_path_probe_init(&g_app.packet_pool);
ret = linkg_path_probe_start();
ret = linkg_path_probe_stop();
ret = linkg_path_probe_deinit();
```

上面的四行是四个生命周期位置的调用示意，不是在一个位置连续执行。

推荐顺序：

```text
init：Packet Pool / Node / Transport / Scheduler 已就绪 → Discovery init → Probe init
start：Network / Link Manager / Discovery start → Probe start → Web
stop：先停止上层控制和Web请求 → Probe stop → Discovery / Link Manager / Network stop
deinit：Probe deinit 必须先于 Node / Scheduler / Transport / Packet Pool deinit
```

`init()` 借用 Packet Pool；Pool 需要至少 16 字节 Transport headroom 和 8 字节有效载荷容量。`start()` 内部注册 PATH_PROBE 接收回调，`stop()` 内部注销，不需要应用层重复注册。

停止过程中：先拒绝新工作、取消诊断并唤醒等待者，再停止 Worker，排空已经进入的 RX 回调。`stop()` 返回前不能销毁 Pool 或下层发送资源。

Probe 状态锁和条件变量保持进程生命周期有效，避免 Transport 已复制但尚未进入的旧回调触碰被销毁的同步对象。每次 start 使用新的回调 cookie；旧 cookie 的回调直接返回，不使用新一轮的 Pool。

生命周期接口由应用管理线程串行调用，不能在 Probe 的 RX 回调或 Worker 内调用 stop/deinit。不要持有 Discovery、Node、Transport 等外层模块锁后再等待 Probe 停止。

## 5. 快照查询

```c
linkg_path_probe_peer_snapshot_t snapshot;
int                            ret;

ret = linkg_path_probe_get_peer_snapshot(peer_node_id, &snapshot);
```

通过以下字段读取三类结果：

```c
snapshot.wifi.classes[LINKG_TRANSPORT_CLASS_REALTIME]
snapshot.wifi.classes[LINKG_TRANSPORT_CLASS_VIDEO]
snapshot.wifi.classes[LINKG_TRANSPORT_CLASS_DATA]
snapshot.cellular.classes[LINKG_TRANSPORT_CLASS_REALTIME]
snapshot.cellular.classes[LINKG_TRANSPORT_CLASS_VIDEO]
snapshot.cellular.classes[LINKG_TRANSPORT_CLASS_DATA]
```

接口只加锁复制缓存，不发送网络包、不等待响应。Peer/Path 列表每秒同步一次，刚启动、刚发现节点时可能尚未形成快照。

状态语义：

- `active`：最近同步时 Node 存在该业务 Path，不代表 Probe 一定成功。
- `valid=false`：尚无结果，或最近一次本地提交失败，不能解释为网络断链。
- `valid=true, reachable=false`：最近一次已接受探测响应超时。
- `valid=true, reachable=true`：最近一次探测在期限内收到匹配响应。
- `rtt_us`：最近成功 RTT；只有当前 valid、reachable 且更新时间足够新时，才作为当前 RTT 使用。
- `updated_us`：CLOCK_MONOTONIC 微秒时间，不是 Unix 时间戳。

主要返回值：未初始化 `-ENODEV`，未运行 `-ENETDOWN`，尚未跟踪该直接 Peer `-ENOENT`，参数错误 `-EINVAL`。

## 6. 多包诊断

以下是一次 STA → 直接 AP 的 Cellular REALTIME 诊断。1 秒发送窗口内计划 100 包，不等上一包响应再发下一包。

```c
linkg_path_probe_diagnostic_request_t request;
linkg_path_probe_diagnostic_result_t  result;
int                                  ret;

memset(&request, 0, sizeof(request));

request.peer_node_id        = ap_node_id;
request.access             = LINKG_LINK_ACCESS_CELLULAR;
request.traffic_class      = LINKG_TRANSPORT_CLASS_REALTIME;
request.packet_count       = 100U;
request.send_window_ms      = 1000U;
request.response_timeout_ms = 300U;

ret = linkg_path_probe_diagnose(&request, &result);
```

发送时刻按 `start + i × send_window / packet_count` 计算，i 从 0 开始；例如 100 包/1 秒为 0、10、20…990 ms。单包诊断立即安排，忽略发送窗口。

输入限制：数量 1~128，响应超时 1~10000 ms，发送窗口最多 60000 ms，多包时平均间隔不得小于 1 ms。

实际调度若错过某个完整发送时隙，该样本记录为本地失败，不集中补发历史样本制造突发。诊断总期限包含最多 100 ms 的额外 Worker 调度余量；单包响应期限仍从该包提交前计时。

接口阻塞调用线程，但网络发送与响应由 Worker/RX 异步处理。不要放在 TUN、Scheduler、Link RX、Transport RX 回调中。Worker 自调用返回 `-EDEADLK`；AP 调用返回 `-EPERM`；已有诊断占用返回 `-EBUSY`。

返回值与统计：

- 返回 0：本次全部样本均已归类，100% 响应超时也会返回 0，调用者必须读取统计。
- 正常完成时 `requested = sent + send_failed`，`sent = received + lost`。
- `loss_permille = lost * 1000 / sent`，只计算已被 Scheduler 接受的探测；没有任何包被接受时置 0，但此时不能当成“0% 丢包、链路健康”。
- `min/average/max_rtt_us` 只统计期限内成功响应。
- `jitter_us` 是按发送顺序相邻成功 RTT 的绝对差均值，丢失样本跳过；它不是单向 IP 时延变化，也不是特定媒体协议的抖动公式。
- 停止返回 `-ESHUTDOWN`，观察到路径替换返回 `-ESTALE`，路径消失等也可能返回对应负值；这些中止结果可能只有部分统计，上述完整计数关系不再保证成立。

## 7. 在途记录和线程边界

Pending 固定 256 个，周期和诊断各预留 128 个槽位，诊断不会把低频保活的全部槽位占完。Pending 存序列号、目标、时间和归属，不长期保存 Packet/Path 引用。

关键竞态已经处理：

```text
预留Pending → 构包 → 发布提交时间和SUBMITTING状态 → 解锁 → Scheduler提交
                                                           ↓
                                               RX可能已经收到响应
                                                           ↓
                        暂存提前响应 → Scheduler返回 → 统一判定成功或本地失败
```

乱序响应按 sequence、直接 Peer、实际入站 Link、Transport Class 联合匹配。重复回复不会重复计数，超时后的回复不回填本次结果。旧请求的结果也不会覆盖发送时刻更新的周期快照。

REQUEST 回调使用新 Packet 原路回复，不写 Pending；RESPONSE 回调只更新在途状态，不调用发送。所有 Scheduler/Node 外部调用都不跨持有 Probe 状态锁。

路径端点与 Link 变化会清理旧样本、取消诊断。内部 generation 表示本模块观察到的变化。当前 Node 公共接口没有独立、不可复用的 Path incarnation，所以相同端点、相同 Link 在两次同步之间快速撤销并重建，不能仅靠此模块保证识别。后续如果需要严格区分这种重建，应由 Node 提供代际或退役通知，不能假装地址相等就证明是同一会话。

## 8. 替代旧 Cellular Heartbeat

本包不会自动编辑旧 Heartbeat 的启动调用。在板上验证 Probe 双向回包和低流量 SSH 后，停止启动旧 `cellular_link_heartbeat`，避免两套机制同时发送。

在当前源码中定位：

```c
ret = linkg_cellular_link_heartbeat_start(cellular_link->heartbeat, link->id);
```

在决定切换到 Probe 的版本里，移除这次 start 及其配套失败处理块；保留前面正常 Cellular TX/RX 启动逻辑。后续再独立清理旧 Heartbeat 的 create/destroy 和接收识别，不要误删 Cellular 数据通道。

一次 Probe 含请求和响应，而且还经过 Transport 封装；不能声称它与旧的单向 8 字节心跳总线流量完全相同。其收益是一次往返同时得到可达性/RTT并产生双向业务端口流量。具体保活效果、温度、低流量延时仍需实机确认。

## 9. 当前 Transport 的两个外部边界

这两个是实际检查上传源码发现的，不属于本模块内部状态机：

1. 现有 `transport_rx.c` 和 TX 统计不是严格按 USER_DATA 类型隔离。因此通过 Transport 发送 PATH_PROBE 后，现有 Peer/Class 累计统计可能包含 Probe，不能继续把总计无条件称为纯用户流量。本包没有偷偷修改这些计数。
2. 现有 RX Window 按 Peer/Class 共享，Wi-Fi 与 Cellular 合并，基线窗口为 512。高流量快速链路可能推进窗口，使迟到的备用链路 Probe 在到达本模块前就被窗口丢弃。因此 Probe 的超时比例首先表示“指定路径上的探测未在期限内被交付”，不等于纯射频丢包率。正式把它作为高速业务下的切换依据前，应针对这类跨链路窗口干扰做专门验证；需要独立 Probe 协议序列域时必须同时调整 Transport TX/RX，不能只绕过一端窗口。

这里保留你已确定的 Scheduler → Transport 路径，没有改回裸 UDP 发送来掩盖上述边界。

## 10. 编译与验证范围

实际核验基线为已上传 `linkg_v3.tar.gz`，其 Git HEAD：

```text
b3c31f6dd4e391b01f55c8bd4b3e8a20ee6be64d
```

不是在线仓库最新提交的声明。验证副本只额外补入已约定的 PATH_PROBE 枚举及 ingress_link_id 递交链。生产模块使用的 Node/Scheduler/Packet/Thread 函数签名来自这份实际源码，没有通过伪造新外部 API 来凑编译。

已执行：

```bash
cmake -S . -B build/probe-validation -DARCHITECTURE=x86
cmake --build build/probe-validation --clean-first -j4
```

x86 全工程编译和链接成功，编译日志中 warning 为 0。

主机测试使用真实项目 Packet Pool、线程/eventfd 和时间模块；用受控 Node、Scheduler、Transport 测试替身注入回包、丢包和生命周期事件。它是模块测试，不是真实 Wi-Fi/5G 数据通路测试。

```bash
./tests/run_tests.sh /path/to/linkg address
./tests/run_tests.sh /path/to/linkg thread
```

`address` 启用 AddressSanitizer 和 UndefinedBehaviorSanitizer；`thread` 启用 ThreadSanitizer。两组各 16 项测试通过，日志在 `validation/`。覆盖 100包/1秒并行在途、乱序、提前回包、本地失败、迟到/重复/错误来源、单包超时、慢提交不补发突发、序列号回绕、主备周期变化、并发诊断、停止取消、端点替换、AP16个Peer、回调排空、旧回调隔离及引用释放。

尚未在 T113 ARM 交叉工具链、真实 Wi-Fi/5G 网络或满速转发条件下实测。硬件验收至少需要验证两个方向的三端口响应、1s/100ms实际发送节奏、高负载备用链路 RTT、低流量 SSH、服务重启及关闭旧 Heartbeat 后的持续连接。

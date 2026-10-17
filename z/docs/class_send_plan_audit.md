# LinkG 三类业务发送计划审查与修复报告

日期：2026-10-17。工作目录：`/home/dawn/project/linkg_codex`。

审查基于本轮开始时 Ubuntu 目录的实际源码快照，不以 Git HEAD 或此前讨论代替当前源码。开始前已读取根目录 `LinkG_C_Code_Style_Guide.md`。没有重置、提交或推送 Git，没有修改 `/home/dawn/project/linkg-v2`。

用户在本轮明确确认了两项产品约定：Switch Wire 统一为 `1U`；窄带为 1 AP + 1 STA，宽带保留多 STA，Cellular 和 Discovery 全局上限保持不变。

## 1. 审核结论

说明中既有实际缺陷，也有已经正确实现的机制和后续策略需求。已修复的严重问题集中在同步事务、陈旧操作和备用链路校验。三类结构本身、整组 AP 应用、Wire Access 转换、默认分类调度及原有分类 Reorder 没有发现需要推倒重做的问题。

| 项目 | 状态 | 核对位置、根因、处理及影响 |
|---|---|---|
| Wire 版本 | FIXED（用户确认的产品约定） | `core/switch/switch_wire.h`：由 3U 改为 1U。编解码统一引用宏，头 8 字节、三类负载 12 字节、PLAN_SYNC 20 字节、ACK 12 字节保持不变。测试与说明同步更新。 |
| 拒绝、超时后没有恢复入口 | CONFIRMED → FIXED | `switch_plan.c` 的 ACK 和周期处理原来直接结束事务，Reconcile 只比较本地路径和计划，可能永久不再发送同一份计划。现在由现有 Worker 的计划 deadline 驱动最多三轮退避恢复，新事务使用新编号，不新增管理线程。 |
| 明确拒绝与结果未知 | FIXED | 内部及状态快照分别记录确认状态和远端明确拒绝来源。即使 AP 返回的错误码也是 `-ETIMEDOUT`，也不会误当成本地 ACK 等待超时。超时不回滚本地计划，页面明确显示未确认。 |
| ACK 缺少 Peer Generation 校验 | CONFIRMED → FIXED | `_linkg_switch_plan_process_ack_event()` 原来只按节点查找 Peer。现在同时核对事件的 Peer Generation 和事务 Message ID，旧队列事件不能结束新 Peer 的事务。 |
| Peer 重建复用消息编号 | CONFIRMED → FIXED | `_linkg_switch_plan_allocate_message_id_locked()` 原来使用 Peer 内清零的编号。现在从 Switch 进程内共享基线分配，Peer 删除重建和服务 deinit/init 不回退编号；Peer Generation 基线同样保留。进程完全退出后的网络迟到报文仍需实板验证。 |
| 本地新计划留下旧同步事务 | CONFIRMED → FIXED | `set_plan` 和 `apply_local` 真实改变整组计划时，撤销旧重发及恢复任务。旧 ACK 不会把新本地计划标记为已确认；保持这两个接口不自动启动远端同步的原有语义。 |
| SINGLE 备用候选必须在线 | CONFIRMED → FIXED | STA 提交原来校验所有非空 Secondary 的 Path。现在只对 REDUNDANT 要求主备实际可用；SINGLE 仍允许合法但暂时不可用的备用候选。主链路仍须可用，备用 Link ID 仍须能转换为合法 Access。 |
| Reconcile/Policy/Maintenance 的陈旧快照覆盖 | CONFIRMED → FIXED | 原先快照复核与实际写入分属两个锁区间。新增内部 `linkg_switch_plan_update_if_current()`，在实际写入锁内比较完整原计划并核对 Generation。三个模块均改用该入口，竞争返回 `-EAGAIN` 等待下一轮，不覆盖新计划。 |
| Maintenance Block 到实际迁移间的发送窗口 | CONFIRMED → FIXED | Scheduler 原来仅依赖计划迁移及 Link running 状态。现在获取新 Target 时通过 `linkg_switch_access_blocked()` 复核本机及对端 Block；不选择被维护接入。已提交到 Transport 的在途批次不做追溯撤销。 |
| 三类整体校验和 AP 原子应用 | NOT_A_BUG | NONE/SINGLE/REDUNDANT 逐类校验，转换先完成，AP 最后一次整组赋值。任意类别失败不部分应用；Link ID 不直接作为远端 Access。补充故障回归验证最终整组值。 |
| `_linkg_switch_plan_has_active_class()` 冗余 | NOT_A_BUG（当前源码无该函数） | 当前使用 `_linkg_switch_plan_empty()` 判断整组是否无可用业务，该判断仍服务 Bootstrap、Maintenance 和全部 NONE 的本地应用，不删除。 |
| NONE 恢复为 SINGLE | NOT_A_BUG | NONE 表示没有可用路径，不是用户禁用业务。路径恢复后逐类恢复 SINGLE 符合现设计，不增加业务禁用开关。 |
| SINGLE 不自动发送 Secondary | NOT_A_BUG | 默认调度只发 Primary；主链路失败返回错误，等待既有校准或 Policy 更新。没有新增 Scheduler 自动故障切换。显式指定链路和控制面冗余策略保留。 |
| Policy 自动分业务迁移 | DEFERRED | Wi-Fi 明确硬故障迁移已有实现，补上原计划比较保护。丢包判决仍固定 NONE，双发健康检查及 Cellular 恢复仍占位。DATA → VIDEO → REALTIME 的自适应顺序留待后续实现。 |
| 窄带单 STA | CONFIRMED → FIXED（用户确认的产品约定） | 实际固件源、升级包和生成 rootfs 模板均为 16；`wifi_hostapd.c` 在生成窄带运行配置时通过现有文本接口设置唯一 `max_num_sta=1`。缺键可补齐、重复键可合并；宽带模板原样保留。没有改动外部 Tina 固件树。 |
| 全部调用方 | NOT_A_BUG（已核查） | 已检索 Switch、Discovery、Probe、Scheduler、Transport RX、Web 状态及概览。剩余直接 `.mode`、`.primary_link_id`、`.secondary_link_id` 均属于单类结构，不是遗漏的旧 Peer 字段。顶栏明确为实时摘要，概览比较三类计划。 |
| 实板行为及完整 TSan | UNVERIFIED | 实板 AP/STA 联调未执行。TSan 尝试在新增程序启动时被运行时地址映射问题中断，未将完整 TSan 计为通过。 |

## 2. 同步恢复的边界

保持原时序：STA 本地整组计划先应用，再发送完整 PLAN_SYNC。没有改成等待 ACK 后才发送业务。

- 同一轮最多发送 5 次，仍使用现有 200ms 重试间隔和 2s 事务截止时间。
- 本轮失败后最多追加 3 轮恢复，等待间隔为 1、2、4 秒。每轮分配新的非零 Message ID，AP 因此可以重新校验，不会一直命中上轮拒绝的缓存 ACK。
- 同一 Message ID 的重发仍返回缓存 ACK，不重复应用新的副作用。
- ACK 明确拒绝与等待超时来源分别记录。超时只表示远端结果未知，不能证明 AP 未应用计划，因此不回滚。
- 尚未开始新事务时，匹配原事务的迟到成功 ACK 可结束不确定状态；新事务开始后，旧 ACK 不能结束新事务。
- 一个完整计划最多 4 轮、合计 20 次发送。达到上限后停止自动发送，状态保留为拒绝或未确认。
- 同一份完整计划可以通过原 `linkg_switch_plan_commit_local()` 显式重新提交，获得新 Message ID 并重新开始有限恢复；真实路径变化导致 Reconcile 产生新计划时也会启动新事务。
- 持续拒绝、链路持续中断或全部 ACK 长期丢失时，不保证两端计划已经一致。页面显示实际未确认状态；检查条件后需要显式重新提交，不能把本地生效当作远端确认。

Reconcile 保持路径一致性职责。事务失败的恢复交给同一个 Worker 已有的计划周期入口和 deadline 查询，无需把网络确认逻辑混入路径选择算法，也不需要新增长期线程。

## 3. 接口和运行路径

公开 `set/get/get_class_plan/remove` 参数保持原样。`set_plan` 仍只设置本地完整 Peer 计划；`commit_local` 仍是 STA 的远端同步入口。

新增内部接口：

```c
int linkg_switch_plan_update_if_current(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *previous, const linkg_send_plan_t *plan, bool synchronize, uint64_t now_us);
```

`previous` 比较、Generation 校验和整组赋值在同一个 Switch 锁区间中完成。`synchronize=true` 用于 STA 需要远端同步的 Reconcile/Policy；false 用于维护协调后的本地应用。所有调用方已检查。

新增只读公开查询 `linkg_switch_access_blocked()` 供 Scheduler 获取发送目标时复核维护约束。它不切换 Primary/Secondary，也不改变计划内容。

公共 Switch 状态快照增加计划同步状态，Web 增加远端确认及恢复显示。此结构变化需要整项目重新编译；Transport 数据头、TUN 和批量发送接口没有修改。

单链路主链路失效时，当前这次发送仍可能返回失败。既有路径校准周期为 500ms，观测/策略周期为 250ms；这些周期不等于端到端切换时延上限，实际发现、无线驱动和路径恢复耗时仍需测量。

## 4. 本轮修改文件、函数和原因（23 个文件）

| 文件 | 主要函数或内容 | 原因 |
|---|---|---|
| `core/switch/switch_plan.c` | ACK、编号分配、恢复辅助、set/apply/commit/process/deadline | 恢复有限闭环、旧事务撤销、Generation 校验、SINGLE 候选语义、原子比较提交。 |
| `core/switch/switch_plan.h` | 同步运行状态、新内部提交接口 | 保留明确确认和恢复状态。 |
| `core/switch/switch_internal.h` | 全局计划编号基线 | Peer 重建不复用编号。 |
| `core/switch/linkg_switch.c` | `_reset_all_locked`、`linkg_switch_access_blocked` | 保留代际/编号并提供维护约束查询。 |
| `core/switch/switch_plan_reconcile.c` | `_reconcile_commit`、取消 NONE 同步 | 条件提交及清理恢复状态。 |
| `core/switch/switch_maintenance.c` | BEGIN、本地 begin 的延迟应用 | 携带原计划快照，避免覆盖并发更新。 |
| `core/switch/switch_policy_wifi.c` | 硬故障提交和双发入口 | 原有策略也采用条件提交，不新增策略。 |
| `core/scheduler/linkg_scheduler.c` | `_acquire_target` | 获取目标时拒绝维护中的接入。 |
| `core/switch/switch_wire.h` | 版本宏 | 按确认统一为 1U。 |
| `core/switch/switch_status.c` | STA 状态复制 | 公开同步确认及恢复结果。 |
| `include/linkg/core/switch/linkg_switch.h` | 维护查询声明 | Scheduler 使用公开查询。 |
| `include/linkg/core/switch/linkg_switch_status.h` | 同步状态结构 | Web 可区分本地、同步中、确认、拒绝及未知。 |
| `platform/wifi/wifi_hostapd.c` | `_update_station_limit`、运行配置生成 | 窄带强制一个 STA，宽带不改。 |
| `service/web/handlers/web_handler_switch_status.c` | 同步 JSON 输出 | 不将未确认计划当作已确认。 |
| `www/js/pages/switch_status.js` | STA 计划及运行概况 | 显示拒绝、超时未知、退避及停止重试。 |
| `www/view/switch_status.html` | 两个状态字段 | 展示远端确认和同步恢复。 |
| `test/send_plan/plan_test.c` | 版本预期和测试适配器 | 复用原回归，支持竞争注入及维护查询。 |
| `test/send_plan/audit_test.c` | 新增故障回归 | 同步、重连、竞争提交、维护、NONE 和状态语义。 |
| `test/send_plan/hostapd_test.c` | 新增配置生成回归 | 缺项、重复键、宽窄带及实际模板。 |
| `test/send_plan/run_tests.py` | 三个程序及可选 TSan | 自动构建实际源码测试。 |
| `test/send_plan/page_test.cjs` | 同步状态展示回归 | 明确拒绝和未知不能显示为运行正常。 |
| `docs/class_send_plan_delivery.md` | 累计交付说明 | 更新协议、累计清单和修复入口。 |
| `docs/class_send_plan_audit.md` | 本报告 | 记录审查证据、测试和边界。 |

本轮修复文件清单为 `build/linkg-plan-audit-files.txt`，修复包为 `build/linkg-plan-audit-fixes.tar.gz`。累计完整清单在 `docs/class_send_plan_delivery.md`，更新后的完整包为 `build/linkg-class-send-plan-sync.tar.gz`，包含 36 个文件。两种包均保留相对路径，不包含 `.git`、构建目录或历史无关修改。

## 5. 实际验证

### 修改前缺陷复现

使用本轮开始的原始源码快照，在独立 `build/plan-audit-before` 目录编译运行探针，确认以下五项原始行为：SINGLE 备用离线拒绝提交、错误 Generation ACK 结束事务、AP 拒绝后没有恢复 deadline、本地 apply 留下旧重试、Peer 重建复用 Message ID。

日志：`/tmp/linkg-plan-audit-baseline-proof.log`。该目录不参与交付，没有修改正式工作源码。

### 构建

已执行并通过：

```sh
cmake --build build/class-plan-host -j4
export STAGING_DIR=/home/dawn/t113_tina5.0_v1.2/prebuilt/rootfsbuilt/arm/toolchain-sunxi-glibc-gcc-830/toolchain
cmake --build build/class-plan-arm -j4
file output/bin/arm/linkg
```

宿主机 x86 Debug 和 T113 ARM Release 整项目均构建成功。ARM 输出确认为 32 位 ARM EABI5 ELF，使用 `/lib/ld-linux-armhf.so.3`。构建日志分别为 `/tmp/linkg-plan-audit-host-build.log` 和 `/tmp/linkg-plan-audit-arm-build.log`。

### 单元及故障回归

已执行并通过：`python3 test/send_plan/run_tests.py`，三个程序均启用 `-Wall -Wextra -Werror`、ASan 和 UBSan。原有十组回归、新增九组故障回归及 hostapd 配置生成回归通过。

| 场景 | 实际覆盖 |
|---|---|
| T01 | 真实 Scheduler 对实时 Wi-Fi、视频/数据 Cellular 分别选择目标，随后进行维护回归。 |
| T02 | 只修改 DATA 的整组提交，另外两类逐字段保持。 |
| T03 | 原有调度及 RX 测试中仅 VIDEO 冗余，其他类别单发。 |
| T04 | 全部 NONE 拒绝用户业务，控制面仍选可用 Cellular。 |
| T05 | AP/STA 快照模拟，最终完整计划和新事务 ACK 一致。 |
| T06 | 成功及拒绝缓存 ACK；同编号重复消息不重新应用。 |
| T07 | AP 拒绝时完整旧计划不变，新事务可在条件修复后恢复。 |
| T08 | AP 已应用但 ACK 丢失，超时不回滚；迟到成功 ACK、新轮 ACK 正确区分。 |
| T09 | 连续拒绝和连续超时均受硬上限约束，远期驱动不再发送。 |
| T10 | Wi-Fi 不可用时仅修复依赖其发送或备用的类别。 |
| T11 | Cellular 不可用时 Wi-Fi 主链路业务继续保留，清理失效候选。 |
| T12 | 实际 BEGIN/END/ACK 状态处理、重复 END、迟到 BEGIN、未受影响业务及 Block 检查。 |
| T13 | 原有事件队列中的旧 Generation ACK、重连后旧 ID，以及运行状态清理时编号保留。 |
| T14 | 连续整组提交的旧 ACK、旧事务取消及旧恢复任务停止。 |
| T15 | 原有 10,000 次并发整组写读，以及 Path 检查期间注入新计划的竞争窗口。 |
| T16 | 截断、非法模式/接入/保留字段及错误协议版本。 |
| T17 | SINGLE 候选不可用仍可提交；主链路失败不自动改发备用；REDUNDANT 双目标要求保持。 |

测试核对完整计划、消息编号、确认状态、发送目标及引用释放，不只核对返回值。网络发送及硬件路径由测试适配器替代，不能当作实板结果。

另将实际固件源、升级包和生成 rootfs 中的 `hostapd_*.conf` 输入真实配置辅助函数，仅在内存生成，验证窄带限制为 1、宽带文本不变。外部模板源文件没有被修改。

前端执行 `node test/send_plan/page_test.cjs` 及修改页面的语法检查，覆盖拒绝、未确认、恢复等待和成功确认状态；不得把未确认结果显示为“运行正常”。

### Git 差异检查

已执行 `git diff --check`、`git diff --stat`，同时使用本轮原始快照检查独立修复差异。

全工作区已有大量此前修改，普通 `git diff --stat` 不能直接视为本轮变更。全局检查仍包含本轮未改文件的既有问题：`platform/wifi/wifi_driver_loader.c` 文件末尾空行、`service/network/network_wifi.c` 尾随空格、`www/version.json` 换行尾随字符。这些未混入本轮修复。`wifi_hostapd.c` 因本轮实际修改，已按规范整理其原有空白问题。本轮独立修复差异通过空白检查。

### 未完成的验证

- ThreadSanitizer 已尝试执行。原有测试程序运行后，新增程序启动被 `FATAL: ThreadSanitizer: unexpected memory mapping` 中断。因此完整 TSan 未通过，不能据此宣称全部运行路径无数据竞争。日志为 `/tmp/linkg-plan-audit-tsan.log`。
- AP/STA 实板、真实 Wi-Fi/5G 断链、驱动恢复、真实网络 ACK 丢包和跨整进程重启的迟到报文尚未验证。
- 窄带第二个 STA 的实际关联拒绝、宽带 16 个 STA、多设备吞吐和长时间 Reorder 没有上板执行。
- 本轮没有重新打包或刷写整个 Tina 固件，也没有修改或启动真实 hostapd 服务。

## 6. 最终判断

代码和可执行测试层面已达到静态三类发送计划首轮实板测试的准备条件：整组读写、分类调度、分类 Reorder 入口、有限同步恢复、Maintenance 和 Reconcile 已形成可观察的处理闭环。已确认的阻断性一致性及候选链路缺陷已修复，没有把尚未执行的实板场景当作通过。

持续故障达到恢复上限后，保留本地计划并明确报告远端未确认，需要条件修复后显式重新提交；这不是强一致或无限自动重试协议。AP、STA 应统一部署当前三类 Wire v1 程序。

自动拥塞迁移、DATA → VIDEO → REALTIME 顺序、丢包窗口及恢复阈值属于后续策略阶段。两项产品约定已由用户确认，本轮没有其他必须等待人工决定才能进行静态首测的接口变更。完整进程重启、真实链路和维护过程的时延与网络迟到行为仍需实板核验。

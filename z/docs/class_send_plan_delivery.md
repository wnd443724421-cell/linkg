# LinkG 按业务发送计划改造交付说明

本次在 `/home/dawn/project/linkg_codex` 修改，以修改前该目录的实际源码为基线。没有向 `/home/dawn/project/linkg-v2` 回写文件。

2026-10-17 审查修复已并入当前源码，新增的故障恢复、并发提交保护和窄带产品限制见 `docs/class_send_plan_audit.md`。本说明的清单为累计完整同步清单，本轮修复差异另列在审查报告中。

## 1. 本次支持的行为

`linkg_send_plan_t` 现在包含 `classes[LINKG_TRANSPORT_CLASS_COUNT]`，每一项是 `linkg_class_send_plan_t`，包含 `mode`、`primary_link_id` 和 `secondary_link_id`。

三类业务分别是 `LINKG_TRANSPORT_CLASS_REALTIME`、`LINKG_TRANSPORT_CLASS_VIDEO`、`LINKG_TRANSPORT_CLASS_DATA`。同一个 Peer 可以同时采用实时 Wi-Fi 单发、视频 Cellular 单发、数据双链路冗余等组合。

- 普通发送及 AP 转发按当前业务读取对应计划。
- RX 判断是否进入原有 Reorder 流程时，按 Peer 和业务类别缓存计划判断结果，避免同一批次中某一业务的冗余状态影响其他业务。
- Discovery 首次建立 Peer 时，为三类业务初始化相同的可用链路计划。
- Maintenance 遍历三类业务，修复使用维护链路的类别，然后整组应用；无关类别保持原计划。
- 路径校准逐类修复失效链路；三类计划统一提交。
- 原有 Wi-Fi 硬故障处理适配为三类计划，仍一次提交完整 Peer 计划。
- STA 使用一次完整 PLAN_SYNC 事务同步三类计划，AP 校验后整组应用，ACK、重试和超时仍按整个事务处理。
- Switch 控制消息可借助可用物理链路发送，避免实时业务为 NONE 时阻断 PLAN_SYNC、ACK 等控制消息。指定链路探测仍遵守指定链路规则。
- Web 切换状态页以三行表格分别展示三类业务的模式、主链路、备用或冗余链路；AP 列表逐 Peer 展示三类计划。
- 概览使用后端完整计划快照，在原有一列中合并相同计划；不同计划显示“按业务分流”，悬停查看三类摘要，点击进入切换状态页。仅备用链路配置不同及部分业务无链路也分别提示。顶栏明确保留实时业务链路摘要。

本次没有增加按容量逐步迁移视频、实时、数据的自动切换算法，也没有补完原来尚未实现的丢包策略、双发健康判断及恢复阈值。

`NONE` 延续原语义，表示当前没有可用链路，不是永久禁用业务的管理开关。后续有可用路径时，既有校准逻辑仍可恢复发送。

## 2. 获取、修改和提交约定

公开接口保持以 Peer 为单位写入：

```c
int linkg_switch_set_plan(uint8_t peer_node_id, const linkg_send_plan_t *plan);
int linkg_switch_get_plan(uint8_t peer_node_id, linkg_send_plan_t *plan);
int linkg_switch_get_class_plan(uint8_t peer_node_id, linkg_transport_class_t traffic_class, linkg_class_send_plan_t *plan);
```

没有新增单类写入接口。修改单类时先读取整个 Peer 计划，在副本中修改目标类别，再提交整个副本。例如：

```c
linkg_send_plan_t plan;
int               ret;

ret = linkg_switch_get_plan(peer_node_id, &plan);
if (ret != 0)
{
    return ret;
}

plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].mode              = LINKG_SEND_MODE_SINGLE;
plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].primary_link_id   = cellular_link_id;
plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].secondary_link_id = wifi_link_id;

return linkg_switch_set_plan(peer_node_id, &plan);
```

注意：`linkg_switch_set_plan()` 延续原有本地设置语义，不会自动发起远端同步。上面的示例只演示完整计划的本地写入。

Switch 内部 STA 策略需要与 AP 同步时，使用原有 `linkg_switch_plan_commit_local(peer_node_id, peer_generation, &plan, now_us)`，每次传入完整三类计划，只调用一次。该函数提交前校验所有业务；本地整组更新后创建一个远端同步事务。发送失败延续原机制，由 Worker 重试，不代表远端已确认成功。维护流程使用 `linkg_switch_plan_apply_local()` 整组应用已经协调好的结果。

整组读写由现有 Switch 互斥锁保护，不会读到同一次写入的半组结果。读取副本后再提交并不提供多写入者的比较交换语义；策略仍应由现有 Switch Worker 串行协调。

## 3. 协议和部署

按用户确认的研发阶段约定，Switch Wire 版本统一为 1。PLAN_SYNC 的三类顺序固定为 REALTIME、VIDEO、DATA，每类 4 字节，完整消息为 8 字节公共头加 12 字节计划负载，共 20 字节。PLAN_ACK 为 12 字节。每类分别校验发送模式、主备接入及保留字段。

研发阶段不兼容旧控制协议。AP、STA 必须统一使用当前三类业务版本，不应混用仅包含一份计划的历史开发程序。本次不改变 Transport 数据帧头。

## 4. 需要同步的生产文件（累计 29 个）

以下路径相对于 `/home/dawn/project/linkg_codex`。回同步到 `linkg-v2` 时保持相同相对路径：

```text
include/linkg/core/switch/linkg_switch.h
include/linkg/core/switch/linkg_switch_status.h
core/switch/linkg_switch.c
core/switch/switch_internal.h
core/switch/switch_plan.c
core/switch/switch_plan.h
core/switch/switch_status.c
core/switch/switch_wire.h
core/switch/switch_wire.c
core/switch/switch_maintenance.c
core/switch/switch_plan_reconcile.c
core/switch/switch_policy.c
core/switch/switch_policy_internal.h
core/switch/switch_policy_wifi.c
core/scheduler/linkg_scheduler.c
core/transport/transport_rx.c
core/discovery/discovery_peer_runtime.c
core/probe/path_probe_runtime.c
platform/wifi/wifi_hostapd.c
service/web/handlers/web_handler_switch_status.c
service/web/handlers/web_handler_header.c
service/web/handlers/web_handler_overview.c
www/js/pages/switch_status.js
www/js/pages/index.js
www/js/pages/overview.js
www/view/switch_status.html
www/view/overview.html
www/css/pages/switch_status.css
www/css/pages/overview.css
```

测试及说明文件（建议一并同步，7 个）：

```text
test/send_plan/plan_test.c
test/send_plan/run_tests.py
test/send_plan/page_test.cjs
test/send_plan/audit_test.c
test/send_plan/hostapd_test.c
docs/class_send_plan_delivery.md
docs/class_send_plan_audit.md
```

累计 36 个文件。无需同步整个目录、`.git`、`build`、`output` 或其他未修改文件。更新后的完整同步包 `build/linkg-class-send-plan-sync.tar.gz` 包含这 36 个相对路径文件。本轮修复包为 `build/linkg-plan-audit-fixes.tar.gz`，适用于已同步上一轮 30 个文件的源码。覆盖目标文件前应保留目标当前工作副本；部署时仍需重新构建。

## 5. 已实际执行的构建与测试

### 全量构建

Linux x86 Debug 全量 CMake 构建通过，输出 `output/bin/x86/linkg`。

```sh
cmake -S . -B build/class-plan-host -DARCHITECTURE=x86 -DCMAKE_BUILD_TYPE=Debug
cmake --build build/class-plan-host -j4
```

T113 ARM Release 全量 CMake 构建通过，包含 `linkg_fast_nat` 目标，输出 `output/bin/arm/linkg`，`file` 确认为 ARM EABI5 ELF。使用开发机已有 Tina 工具链，需设置其 `STAGING_DIR`：

```sh
export STAGING_DIR=/home/dawn/t113_tina5.0_v1.2/prebuilt/rootfsbuilt/arm/toolchain-sunxi-glibc-gcc-830/toolchain
cmake -S . -B build/class-plan-arm -DARCHITECTURE=arm -DCMAKE_TOOLCHAIN_FILE=toolchains/arm.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/class-plan-arm -j4
```

### 行为回归

```sh
python3 test/send_plan/run_tests.py
node test/send_plan/page_test.cjs
node --check www/js/pages/switch_status.js
node --check www/js/pages/index.js
node --check www/js/pages/overview.js
```

C 回归在 Ubuntu 执行，编译启用 `-Wall -Wextra -Werror`、AddressSanitizer 和 UndefinedBehaviorSanitizer。测试调用实际计划、Wire 编解码、Scheduler、RX、Discovery、Maintenance、校准及 Web JSON 代码，仅用测试适配器代替硬件路径、网络发送等外部依赖。以下十组均通过：

1. 完整计划校验、整组写入、分类读取和 Peer 隔离。
2. v1 三类业务协议往返、NONE、截断、各类非法字段和保留字节、错误版本拒绝。
3. 一次三类同步、ACK、错误 ACK、重复消息、目标不可用时不部分写入。
4. 普通发送、AP 中继、指定链路探测、业务 NONE 及控制消息恢复路径。
5. 同批次混合业务按各自计划选择 Reorder 入口。
6. Maintenance、逐类资源校准、Discovery 三类初始化。
7. 既有 Wi-Fi 硬故障处理一次提交，并清理其他类别失效的备用链路。
8. Web JSON 三类字段及无有效快照时的规范输出。
9. 10,000 次并发整组写入与 10,000 次完整快照读取。
10. 概览 JSON 完整输出三类业务计划；经 AP 转发的远端节点不误报为本机直接计划。

前端 Node 功能回归及上述三个 JS 语法检查通过。前端功能回归调用页面真实格式化与渲染函数，验证 STA 三行表格、AP 三类主备展示、概览的一致计划合并、不同计划分流、仅备用配置不同、部分或全部 NONE、详情链接、悬停摘要及刷新后的节点复用。Node 检查在 Windows 开发环境执行。

另使用 Windows Edge 无界面浏览器加载实际 HTML、CSS 和页面渲染函数，生成并人工查看 STA、AP、概览三张预览图。布局测试使用构造的混合业务计划，不代表真实设备运行状态。1440、1100、960 像素桌面窗口宽度下未发现新增页面级横向溢出；宽表保持容器内滚动。沿用项目现有 960 像素全局最小宽度，没有实现新的移动端布局。

### 尚未执行的硬件场景

没有把新程序部署到开发板，没有执行真实 Wi-Fi/5G 双链路、多个 AP/STA、长时间视频/实时业务、实际路径中断和恢复、维护升级期间网络抖动等硬件联调。相关协议、吞吐、时延、乱序恢复和页面实际连接状态仍需上板验证。构建通过及测试适配器回归不代表这些硬件场景已经通过。

# 串口硬件诊断修复与验证记录

验证日期：2026-11-01（Asia/Shanghai）

工程：Ubuntu `192.168.80.100:/home/dawn/project/linkg-v2`。
开发板：`192.168.1.1`。最终 Web 版本：`LinkG-v1.0.0+20261101.2`。

按用户最新确认：UART1 用于系统终端输出，保留该配置；实际短接和完整回环验收改用 UART2。

## 已修复的问题

1. `service/uart/uart_diagnostic.c` 原先在释放配置事务、UART 诊断独占权和 busy 前先公开终态。GET 已结束后紧接 START 可能仍被拒绝。现在先释放资源，再在同一任务锁内发布终态并清除 busy。
2. 准备阶段失败或取消后，未执行的测试项统一标为 skipped；准备阶段也保留真实的原始 UART 生命周期信息。deinit 等待线程退出前关闭新任务入口。
3. `www/js/pages/diagnostics_uart.js` 用操作版本识别旧 GET 响应，防止旧任务/旧取消状态覆盖新一轮；状态轮询只保留一条计时链。启动响应丢失时先回读最新任务，避免重复启动。首次成功读取状态前不开放开始按钮。
4. 历史任务和终态会更新操作提示，清除“正在读取/等待结果”残留；取消已结束任务回读最终结果；成功、失败、取消后均恢复开始按钮和诊断选择。
5. 检测到真实系统控制台冲突：UART1 -> `/dev/ttyS3`，板端 `console=ttyS3,115200`、`uart3_mode=debug`，`/proc/consoles` 为 `ttyS3 -W- (EC)`。原回环间歇通过，但同时出现数据错位、POLLIN|POLLERR|POLLHUP（0x19）和内核 SysRq 记录。`TIOCEXCL` 无法隔离系统控制台。现在在发送测试数据前检查已启用控制台，返回 -EBUSY，前端提示检查系统控制台或其他进程占用。没有修改启动参数或控制台配置。
6. 更新前端版本号，并同步升级目录和升级包，避免缓存或后续打包重新引入旧实现。

## 回归与实机验证

- 旧后端在控制释放动作的测试中稳定失败于“释放中仍应为活动态”的断言；修复后通过。
- 旧前端稳定复现历史提示残留以及旧 GET 覆盖新任务、错误解锁；修复后通过。
- 后端真实源码 + 生命周期适配器，使用 GCC、ASan、UBSan：20 次连续成功；单项失败、准备失败、取消、恢复失败；原始运行/停止/未初始化状态恢复；资源释放之前保持活动态；卸载期间拒绝新任务；未执行项收尾，均通过。
- 控制台识别测试：启用的目标控制台在 open/write 前拒绝；其他控制台、禁用控制台和读取 proc 失败分支均通过。
- Playwright + Microsoft Edge：成功/失败/取消终态直接到达、重复执行、旧 GET/不存在响应晚到、启动响应丢失、取消与查询交错、卸载重挂载和单一轮询链，均通过。
- 接线切换前，开发板真实浏览器验证了 UART1 控制台占用（-16，TX/RX 均 0）、UART2 未短接超时（-110）、真实取消（-125）、取消后重测，以及页面返回与刷新后的继续启动。每轮资源恢复且控件解锁，无脚本错误。
- 用户指定改用 UART2 并短接后，两组浏览器各连续 5 轮全部通过；页面取消 UART2 实际返回 cancelled、cancel_requested=true、uart_restored=true；取消后下一轮与离开再返回后的测试均成功。
- UART2 额外连续 20 轮 API 快速测试（任务 #21—#40），每次终态一到立即 START；20/20 通过，TX/RX 均 1536 字节、错误字节 0、uart_restored=true，单轮总耗时 207—269 ms，无 busy 残留。
- UART2 本轮合计 32 次完整回环全部通过，另 1 次主动取消按预期完成。
- ARM 正式构建 `./build.sh arm` 成功。
- 本次修改的 scoped diff 格式检查通过。全工程 diff 检查还会报告开始前即存在的 `service/uart/uart_internal.h` CR/尾部空白；该文件未由本次修改。
- 升级包解包内容的二进制、诊断 JS 和版本文件，与最终构建/源码一致。

## 硬件验收边界

UART1 由用户确认为系统终端用途，保持原配置，不作为回环验收串口。其控制台冲突检测保留，以免误选后发送二进制测试数据。

有效硬件验收已改用 UART2 并完成：32 次完整回环零错误，主动取消后能够再次成功测试。UART0/SBUS 缺少 MCU/外设，未进行真实 SBUS 通路通过验证。失败流程使用接线切换前 UART2 未短接时的真实超时记录覆盖，任务级准备/恢复失败由后端故障注入回归覆盖。

## 交付位置与运行方式

源码和测试已写回 `/home/dawn/project/linkg-v2`。新增：

- `test/uart/run_diagnostic_tests.py`
- `test/uart/uart_diagnostic_lifecycle_test.c`
- `test/uart/uart_diagnostic_console_test.c`
- `test/uart/test_diagnostic_page.cjs`
- `test/uart/test_diagnostic_board.cjs`

后端：`python3 test/uart/run_diagnostic_tests.py`。
前端（需 Node、Playwright 和浏览器）：`node test/uart/test_diagnostic_page.cjs`。
板端（会实际下发诊断）：`UART_DIAG_LOOPBACK_PORT=2 node test/uart/test_diagnostic_board.cjs http://192.168.1.1`。
可用 `PLAYWRIGHT_BROWSER_PATH` 或 `PLAYWRIGHT_CHANNEL` 指定浏览器。本次浏览器测试在 Windows 现有 Edge/Playwright 环境执行。

修复版应用和页面已部署到板端当前 v2，重启了 LinkG 服务。原始文件备份在板端 `/tmp/linkg-uart-diag-20261101/`（重启设备后 tmp 可能清空）。没有执行固件升级或修改分区状态。

升级包：`/home/dawn/project/linkg-v2/upgrade/output/linkg-upgrade-v1.0.0.tar.gz`。
包中仍包含工程原有 kernel/system_install 内容；本次板端验证仅替换应用与诊断前端。

最终应用 SHA256：`05cbb5a7f3274c2ce370ed9bc1f36e0e09ec9c80e612e946b2454caf0e322ea6`。
最终诊断 JS SHA256：`881a6cc54022c48870625cd2866aac86e15002458d93e31e30533dc1272d4c0b`。
升级包 SHA256：`69502e1e031167abe7f0c8aa49828dd3aa59c562be286d164b341c64ccbe0932`。

此目录的 patch 仅包含本次源码/测试改动，以任务开始时已有文件为基线，不包含其他未提交修改和二进制产物。没有创建 Git 提交。

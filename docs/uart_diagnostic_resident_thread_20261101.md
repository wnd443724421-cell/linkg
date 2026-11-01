# UART 诊断常驻线程改造与验证

日期：2026-11-01，Asia/Shanghai。
工程：`192.168.80.100:/home/dawn/project/linkg-v2`。板端：`192.168.1.1`。

## 实现

参照 `core/upgrade/linkg_upgrade.c` 的事件驱动常驻线程模式，将 UART 诊断接入真实 `linkg_thread_t`：

1. `linkg_uart_diagnostic_init()` 初始化线程对象和 eventfd，不创建 pthread。
2. `linkg_uart_diagnostic_start()` 随应用启动创建 `linkg_uart_diag` 常驻线程。
3. 空闲时 `poll(wakeup_fd, ..., -1)` 无限期阻塞，不使用定时轮询或周期 sleep。
4. `linkg_uart_diagnostic_submit()` 接受 Web 1025 任务并唤醒线程；每次任务不再 create/join 线程。
5. `linkg_uart_diagnostic_cancel(job_id)` 供 Web 1027 取消单个任务；恢复资源后线程继续等待下一轮。
6. `linkg_uart_diagnostic_stop()` 关闭提交入口，取消已接受任务，等待资源恢复和 join；stop 失败时保留资源以便重试。
7. `linkg_uart_diagnostic_deinit()` 在 stop 成功后销毁线程对象。标准 init/start/stop/deinit 与任务 submit/cancel 分开。

应用在 UART Manager 启动后、Web 启动前启动诊断线程；先停止 Web 和诊断线程，再停止 UART Manager/GPIO。新增生命周期锁，join 不持任务锁。保留上一轮的“资源释放之后才公布终态并清 busy”修复。

唤醒失败会释放诊断独占权并恢复上一轮结果；事件等待异常会关闭新任务入口并收尾尚未执行的任务。初始化、启动和停止失败均允许按生命周期重试。Web 命令和 JSON 响应格式保持一致，前端资产未改动，Web 版本仍为 `LinkG-v1.0.0+20261101.2`。

常驻方式避免每个任务创建和回收线程的开销；空闲时保留一个线程对象、线程栈和唤醒 fd。本次未进行吞吐或延迟性能对比，不将线程复用描述为所有内存开销都降低。

## 验证结果

- Ubuntu 编译并运行真实 `uart_diagnostic.c` + `infra/base/linkg_thread.c`，以生命周期适配器隔离硬件，启用 ASan/UBSan，全部通过。
- 连续 20 个成功任务使用同一 pthread ID；计数断言任务之间没有 pthread_create/pthread_join。
- 无限期空闲等待、重复 init/start、单次取消后继续执行、活动任务和已接受但未执行任务的 stop、stop/start 重启、deinit 并发拒绝新任务，均通过。
- eventfd 初始化失败、线程创建失败、任务唤醒失败、等待失败、join 失败和重试均通过。保留原有测试：准备失败、测试超时、资源恢复失败、未执行项 skipped、生命周期恢复和控制台冲突。
- 原前端异步查询/重复任务/取消/卸载重挂载回归通过。ARM `./build.sh arm` 构建通过。修改范围 diff 格式检查通过。
- 实机进程 PID=16871，诊断线程 TID=16928：首次任务前即存在；连续 20 轮 UART0 和后续页面取消/重测后仍为同一 TID，空闲状态为 S，wchan=`poll_schedule_timeout.constprop.1`。
- 多组连续 1.2 秒空闲采样，utime/stime 保持不变。任务执行期间的 CPU tick 正常增加，空闲采样无增长，不表示线程始终不消耗 CPU。
- 真实 UART0 连续 20 轮均完成收尾、uart_restored=true，终态后立即提交下一轮均被接受。页面再次验证 3 轮完成、1 次取消、取消后重测：普通项 cancelled，SBUS 项 skipped，资源恢复，前端控件解锁。

## 本轮硬件边界与对照

用户最终确认当前短接串口为 UART0，UART1 保留终端输出。

此前自动测试那批 UART0 普通路径结果为 TX=256、RX=0、error=-110；SBUS 子项也超时。这些历史结果没有被当作回环通过。

为排查线程改造是否引入接收故障，已临时切回改造前备份做同接线对照：旧版普通路径同样 TX=256、RX=0、error=-110，SBUS 也超时，资源恢复成功。对照结束后已恢复新版。该对照未显示线程改造改变了本次接收结果，但不据此确定接收超时的物理或配置根因。

用户随后提供了任务 #27 的成功截图，并已通过只读 Web 1026 查询核实：UART0 普通回环 state=passed，TX=1536、RX=1536、error_count=0、error=0、elapsed_ms=188；任务 completed，uart_restored=true。SBUS 子项 RX=0、error=-110，因此 tests_passed=false。常驻线程仍为 PID=16871 / TID=16928，空闲等待 poll。

最新验收结论：UART0 普通回环已通过，资源恢复和进入下一轮正常，常驻线程复用/休眠及失败/取消流程也已完成验证。页面总体“存在失败项”仅由 SBUS 子项导致。SBUS 缺少 MCU 治具，仍不作通路验收。此前 RX=0 的记录保留为历史对照，不再代表当前普通回环结果；未据现有信息推断前后变化的硬件原因。

严格实机回环脚本会断言普通回环成功；本轮该断言如实失败，并未放宽为超时也通过。脚本支持 UART_DIAG_LOOPBACK_PORT=0，UART0 默认仅检查普通路径；有完整 MCU 治具时可设置 UART_DIAG_CHECK_SBUS=1。

## 交付

修改：

- `service/uart/uart_diagnostic.c`
- `include/linkg/service/uart/linkg_uart_diagnostic.h`
- `app/linkg_main.c`
- `service/web/handlers/web_handler_uart_diagnostic.c`
- `service/uart/README.md`
- `test/uart/run_diagnostic_tests.py`
- `test/uart/uart_diagnostic_lifecycle_test.c`
- `test/uart/test_diagnostic_board.cjs`

源码、测试、文档已写回上述 Ubuntu 工程，未创建 Git 提交。现有未提交修改以本轮开始时的文件为基线保留。

新版应用已部署，最终运行文件与构建一致：
`0fdfbf305895d52557bc532e2d297a289d62be9fb1e700926e28e4b403b75f4c`。

板端本轮前应用备份：`/tmp/linkg-uart-resident-20261101/linkg.before`。临时目录重启后可能清空。
升级包已同步：`/home/dawn/project/linkg-v2/upgrade/output/linkg-upgrade-v1.0.0.tar.gz`。
包 SHA256：`193b603f55edab32003c9027e6495237c1db022727876f0323638e53114c2648`。已核验包内应用等于新构建。包保留原有 kernel/system_install 内容，本轮板端只替换了应用。

回归：`python3 test/uart/run_diagnostic_tests.py`。
物理回环（需要匹配接线）：`UART_DIAG_LOOPBACK_PORT=0 node test/uart/test_diagnostic_board.cjs http://192.168.1.1`。

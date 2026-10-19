# Supervisor可执行测试

测试使用真实Supervisor进程、真实fork/exec子进程和匿名管道，不启动正式LinkG、不访问开发板、不修改系统网络或密码。C模拟程序直接链接本次实现的客户端。

```bash
cmake -S supervisor -B /tmp/linkg-supervisor-v01-build -DLINKG_SUPERVISOR_BUILD_TESTS=ON
cmake --build /tmp/linkg-supervisor-v01-build -j4
ctest --test-dir /tmp/linkg-supervisor-v01-build --output-on-failure -j2

# 查看所有PASS和具体错误日志
cat /tmp/linkg-supervisor-v01-build/Testing/Temporary/LastTest.log
```

`supervisor-scenarios`使用独立的 `linkg-supervisor-test` / `supervisor-mock`：READY 600ms、心跳400ms、服务重启900ms、TERM等待200ms、退避100～800ms。产品目标 `linkg-supervisor` 从不设置测试宏，保持60s/4s/60s/2s和1～30s参数。

快速场景包括：

| 场景 | 检查 |
|---|---|
| 正常READY与周期心跳 | 仅启动一个PID、无超时、稳定后清零 |
| 无READY、只有HEARTBEAT | 心跳不能替代READY，启动超时后恢复 |
| READY后停止心跳 | 4秒对应的测试期限超时并恢复 |
| 非零退出、SIGSEGV | waitpid报告退出码/信号并重新拉起 |
| READY前退出码0 | 视为未完成启动，退避后重试 |
| 进程内服务重启 | 保持同一PID，收到READY重新RUN |
| 服务重启超时 | 故障恢复 |
| 重复RESTARTING和夹杂心跳 | 不延长重启期限 |
| 重复READY | 不延长RUN心跳期限 |
| 拒绝SIGTERM | SIGKILL后waitpid回收，才启动下一个PID |
| 连续失败 | 退避递增并饱和，检查真实启动间隔 |
| 管道EOF但进程仍存活 | 等待真实退出，清理超时后恢复 |
| 正常清理超过RUN心跳期限 | 使用清理宽限而非误判心跳丢失 |
| 正常退出码0、READY后立即退出 | Supervisor保持常驻，退避后重新启动LinkG |
| 外部依次TERM、KILL子进程 | Supervisor始终存活，旧PID回收后启动新PID |
| 四轮服务重启 | 仍为原PID，客户端保持两个线程总数 |
| 无环境变量、无效描述符 | 独立运行无心跳线程，非法通道返回错误 |
| 写满管道、关闭读端 | EAGAIN不阻塞、EPIPE不被SIGPIPE杀死 |
| 协议时序 | RESTARTING和后续READY之间无心跳 |
| 重复Supervisor | 同一锁路径拒绝第二个实例 |
| FD和进程回收 | 多次重试FD数有界，退出后子PID不存在 |
| exec路径不存在 | 退出127，按退避恢复 |

`supervisor-production-timing`运行产品目标和不缩短心跳周期的客户端，实际等待正式期限：

- 连续1秒心跳下稳定运行，不重复启动。
- 首次READY等待60秒，HEARTBEAT不能代替READY。
- RESTARTING等待60秒，重复RESTARTING不能延长窗口。
- 正常进程内服务重启暂停30秒，再READY，保持同一PID。
- RUN阶段4秒心跳超时，然后约1秒退避。
- 拒绝TERM时等待约2秒再KILL，然后约1秒退避。
- LinkG退出码0时Supervisor保持常驻，约1秒退避后重新启动。

两组可以并行执行，使用各自的临时锁和PID，正式期限测试约62秒完成。首次READY及服务重启的60秒被实际等待，不是静态读取宏值。

测试不是开发板整机业务验收；尚未覆盖板上真实模块清理耗时、系统关机/重启全过程、不可中断内核等待、进程或机器掉电、故障注入下的pthread创建失败。独立心跳不表示所有业务线程都健康。

## 本次实际结果（2026-10-19）

- Ubuntu x86，GCC 13.3.0：快速场景29/29通过，产品原始期限场景7/7通过；CTest两组全部通过，约62秒。
- 完整LinkG和独立Supervisor：x86编译及链接通过。
- 项目现有GCC 8.3.0 ARM工具链：完整LinkG与Supervisor交叉编译及链接通过，产物识别为ARM EABI5。
- README中的独立ARM构建命令实际通过。
- 新生产源码GCC `-fanalyzer -Wall -Wextra -Wpedantic -Werror -fsyntax-only` 检查通过。
- 本次C/H源文件检查：无Tab、无尾随空白、函数签名单行、C函数具备brief、头文件声明对齐、宏值列统一；`git diff --check`通过。
- 本次没有安装到开发板或切换正式守护入口，实板整机业务验证尚未执行。

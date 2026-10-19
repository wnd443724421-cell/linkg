# Supervisor V0.1代码地图

| 文件 | 职责 |
|---|---|
| `src/supervisor_main.c` | 路径参数、锁路径、独立入口 |
| `src/supervisor_process.c` | fork/exec、poll/waitpid、状态机、TERM/KILL、退避及互斥进程锁 |
| `include/supervisor_config.h` | 产品固定期限与独立测试期限 |
| `include/supervisor_process.h` | 单一运行接口 |
| `../include/linkg/base/linkg_supervisor_protocol.h` | 双方共享的单字节通知值和环境变量名称 |
| `../include/linkg/base/linkg_supervisor_client.h` | 主程序通知接口 |
| `../infra/base/linkg_supervisor_client.c` | 管道接管、非阻塞消息、SIGPIPE处理、唯一心跳线程 |
| `../app/linkg_main.c` | 原有生命周期中的少量通知调用 |
| `tests/supervisor_mock.c` | 使用真实客户端或裸协议模拟故障 |
| `tests/test_supervisor.py` | 真实子进程场景断言、期限检查、PID/FD/线程检查 |

审核重点：

1. `_supervisor_exec()`：父子管道所有权、exec继承、子进程信号复原。
2. `_supervisor_monitor()` / `_supervisor_message()`：固定期限、重复通知及waitpid优先。
3. `_supervisor_receive()`：EOF清理窗口与真实退出分离。
4. `_supervisor_terminate()`：旧PID回收之前绝不重启。
5. `_supervisor_backoff()`：饱和退避、退出信号响应与稳定运行清零。
6. `_supervisor_client_send_locked()`：无阻塞发送、EPIPE/SIGPIPE处理。
7. `_supervisor_client_worker()`：服务重启期间无心跳，线程不重复创建。
8. `linkg_main.c`的INIT、RUN、CLEANUP和最终退出路径：通知调用不会重构业务生命周期。

空的 `supervisor_boot.*`、`supervisor_version.*` 和JSON夹具保持原样，不进入本次构建，也不承诺其功能已实现。

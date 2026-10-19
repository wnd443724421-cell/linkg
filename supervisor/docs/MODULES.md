# Supervisor V0.2 代码地图

| 文件 | 职责 |
|---|---|
| src/supervisor_main.c | 入口参数、默认路径，选择版本管理或显式 V0.1 进程模式 |
| src/supervisor_process.c | 单实例锁、fork/exec、Pipe 状态机、waitpid、TERM/KILL、退避；健康窗口通过后调用确认，旧进程回收后调用回滚 |
| src/supervisor_version.c | 固定 JSON 严格校验和保存、Release 与 Kernel 映射、App/Web 安装检查 |
| src/supervisor_boot.c | 从真实 cmdline 识别 Kernel；核对 Tina ENV；批量写入、检查结果及读回；请求 reboot |
| src/supervisor_release.c | 组合版本、启动和链接模块，执行开机恢复、CONFIRM 和 ROLLBACK 顺序 |
| src/supervisor_links.c | 两条 current 的临时软链接、rename、失败协调恢复，不操作 Release 内容 |
| src/supervisor_storage.c | 有界文本读取、同目录临时文件、fsync、rename 和目录同步 |
| include/supervisor_config.h | 产品时限、隔离测试时限及默认路径 |
| ../include/linkg/base/linkg_supervisor_protocol.h | 已审核的匿名 Pipe 单字节协议 |
| ../infra/base/linkg_supervisor_client.c | LinkG 通知和心跳线程，本轮不改业务生命周期 |
| tests/test_supervisor.py | V0.1 的进程、协议、回收故障和正式时间回归 |
| tests/test_release.py | A/B 选择、试运行、确认、回滚和部分持久化结果恢复 |
| tests/release_mock.py | 独立安装树中的 LinkG 模拟进程及 ENV 工具替身 |
| tests/release_faults.c | JSON rename、父目录 fsync、第二条软链接切换错误，仅测试使用 |

## 调用流程

~~~mermaid
flowchart TD
    A[获取唯一进程锁] --> B[加载固定JSON]
    B --> C[读取实际Kernel槽位]
    C --> D{比较JSON映射和完整ENV}
    D -->|候选仍运行旧Kernel| R[JSON恢复稳定目标]
    D -->|稳定目标仍运行新Kernel| E[恢复ENV并请求reboot]
    D -->|跨Kernel确认ENV已完成| F[补写稳定JSON]
    D -->|匹配候选或稳定版本| G[验证App和Web目录]
    R --> E
    E -->|实际Kernel匹配稳定映射| G
    F --> G
    G --> H[协调切换两条current]
    H --> I[启动唯一LinkG]
    I --> J{现有Pipe监控}
    J -->|候选连续RUN 60秒| K[ENV确认后写稳定JSON]
    K --> J
    J -->|候选失败| L[终止并真实回收旧PID]
    L --> R
    J -->|稳定版本故障| M[终止回收并退避]
    M --> B
~~~

## 审核关键点

1. supervisor_release_prepare：每次启动前按 JSON、实际 Kernel 和完整 ENV 恢复；App-only 不借用 ENV 判断 App 已确认。
2. supervisor_release_confirm / _supervisor_release_commit：健康窗口后 ENV 在前、稳定 JSON 在后；失败不在内存中虚假确认。
3. supervisor_release_rollback：JSON 在前、ENV 在后；实际 Kernel 不匹配时禁止启动并请求 reboot。
4. _supervisor_monitor：READY 不立即确认版本，RESTARTING 后重新计完整窗口；版本确认与 V0.1 故障清零分别处理。
5. _supervisor_process_run：候选回滚与稳定版本无限守护分支分开；回收不成功时不切换、不启动第二个实例。
6. _supervisor_boot_read / _supervisor_boot_write：校验启动规则，批量提交四项 ENV 并读回；不写 bootlimit。
7. supervisor_links_select：先保存两条旧链接，任一步失败均尝试协调恢复，全部完成后才允许启动。
8. supervisor_storage_write：rename 后目录同步失败属于“可能已生效”，上层通过比较完整 JSON 幂等重试。

本轮无专用事务日志、额外 JSON 字段、Supervisor 自有启动次数计数、Kernel 刷写或 OTA 安装逻辑。ENV 自身写入时掉电风险按用户明确边界保留。

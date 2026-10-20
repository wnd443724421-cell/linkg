# LinkG Supervisor 与 OTA 状态衔接

Supervisor 独立常驻，负责选择 v1/v2、启动唯一的 LinkG、监控 READY/HEARTBEAT/RESTARTING/STOPPING、确认候选版本及失败回滚。保留 V0.1 的匿名 Pipe 单字节协议、waitpid 回收、TERM/KILL 和稳定版本退避恢复。

Supervisor不安装 OTA 包或刷写 Kernel；这些由 LinkG Upgrade Worker 负责。Web 1021 接受安装请求，1022 读取同一份持久化状态。Supervisor仍独立负责候选确认、回滚和启动恢复，不改 U-Boot、设备树或分区表。

## 文件与启动

~~~text
/app/version_state.json
/app/releases/v1/bin/linkg
/app/releases/v2/bin/linkg
/app/current -> /app/releases/v1 或 /app/releases/v2
/www/releases/v1
/www/releases/v2
/www/current -> /www/releases/v1 或 /www/releases/v2
/var/run/linkg-supervisor.lock
~~~

版本 JSON 只有以下结构，空对象表示未安装，不能启动；兼容旧版缺少 upgrade_state 时按 idle 读取：

~~~json
{
  "schema_version": 1,
  "release_partition": "v1",
  "stable_release_partition": "v1",
  "upgrade_state": "idle",
  "releases": {
    "v1": {"kernel_slot": "boot_a"},
    "v2": {}
  }
}
~~~

schema_version 只能为 1，Release 只能为 v1/v2，Kernel 只能为 boot_a/boot_b。拒绝未知字段、重复键、损坏 JSON、内嵌零字符和未安装的稳定 Release。不增加事务字段或专用事务日志。

LinkG与Supervisor共用严格解析器及原子保存。运行期通过 version_state.json.lock 比较完整旧快照后更新；LinkG安装期间全程持有 version_state.json.ota.lock，Supervisor启动恢复遵守同一租约。不得手工删除锁文件来解锁，也不能由维护脚本绕过锁覆盖状态。

默认启动 /usr/sbin/linkg-supervisor。可配置状态与安装路径：

正式部署应由唯一的 init/procd 入口运行 Supervisor，停止旧 linkg_watchdog.sh，避免两套守护同时拉起 LinkG。本版不修改这些启动入口。

~~~bash
/usr/sbin/linkg-supervisor \
    --lock-file /var/run/linkg-supervisor.lock \
    --state-file /app/version_state.json \
    --app-root /app \
    --web-root /www
~~~

显式指定子程序保留 V0.1 进程守护模式，此时不启用版本管理：

~~~bash
linkg-supervisor --lock-file /tmp/supervisor-demo.lock -- /absolute/program arg
~~~

## 开机和试运行

先获取单实例锁，再读取 JSON、核对实际 Kernel 和 ENV、检查 App/Web 安装目录与可执行文件，最后协调切换两个 current 并启动 LinkG。实际 Kernel 只认 /proc/cmdline 中唯一且合法的 kernel_slot=boot_a/boot_b，不使用 ENV 的期望值替代它。无法确认时常驻记录错误并退避，不启动可能依赖错误 Kernel 的 App。

| 状态或期限 | 正式参数 |
|---|---|
| 首次 READY | 60 秒 |
| HEARTBEAT | 客户端每 1 秒发送，4 秒超时 |
| RESTARTING | 60 秒，重复消息不续期 |
| 显式 STOPPING | RUN 中收到后最长 60 秒清理，重复消息不续期 |
| 候选确认 | 首次或重新 READY 后，连续 RUNNING 满 60 秒且心跳有效 |
| 稳定版本故障退避 | 1、2、4、8、16、30 秒，连续 RUN 60 秒后清零 |
| SIGTERM 宽限 | 沿用当前配置 5000ms，之后 SIGKILL 并回收 |

RESTARTING 不累计候选稳定时间，重新 READY 后重新计满 60 秒。READY 本身不表示升级成功。候选在确认前退出（包括退出码 0）、崩溃、通知异常或超时，回收旧进程后立即回滚，不进入无限重启。

稳定确认完成后继续守护当前版本，后续普通进程故障使用 V0.1 退避恢复，不再回滚旧 Release。单独停止 LinkG 会被拉起；维护时应停止 Supervisor，由它回收 LinkG 后再操作。

## 三次事务及中断恢复

| 事务 | 第一步 | 第二步 | 后续 |
|---|---|---|---|
| PREPARE（LinkG安装完成） | JSON 设目标候选、保留旧 stable及rebooting | ENV 设候选 Kernel 及旧稳定 Kernel，开启试启动保护 | reboot |
| ROLLBACK（本版实现） | JSON 目标恢复旧 stable | ENV 恢复稳定 Kernel，bootcount=0、upgrade_available=0 | Kernel 不同才 reboot |
| CONFIRM（本版实现） | ENV 完整确认新稳定 Kernel | JSON stable 更新为候选 | 继续守护当前进程 |

每次开机都执行恢复判断：

| JSON 与实际 Kernel / ENV | 动作 |
|---|---|
| JSON 为候选，实际仍是旧稳定 Kernel，两个映射不同 | 取消候选并恢复旧 Release；包括 PREPARE 只写完 JSON 和 U-Boot 已回退 |
| JSON 已回滚旧稳定版本，实际仍是新 Kernel | 继续修复 ENV 并 reboot；本轮不启动旧 App |
| JSON 为候选，实际是候选 Kernel，完整 ENV 已确认候选 Kernel | 补写稳定 JSON，完成中断的 CONFIRM |
| JSON 为候选，实际是候选 Kernel，ENV 尚未完整确认 | 保留或修复试启动保护，开始完整 60 秒试运行 |
| v1/v2 使用相同 Kernel，JSON 仍是候选 | 不能从 ENV 推断 App 已通过测试；重新计满 60 秒 |
| JSON 为稳定，实际 Kernel 与映射一致 | 修复一致的稳定 ENV、切换两个 current、正常守护 |

“完整 ENV 已确认”要求 boot_partition 和 stable_boot_partition 均为候选 Kernel，bootcount=0 且 upgrade_available=0，并且实际 Kernel 匹配、启动命令校验通过；不能只检查 boot_partition。

JSON 采用同目录临时文件、fsync、rename 和父目录 fsync。ENV 失败不写稳定 JSON；JSON 保存或目录同步失败时不在内存中宣称成功，保留候选监控并重试。rename 已成功而目录同步失败可能导致当前文件已变化，后续比较接受完整的目标状态并重新完成持久化。

ENV 辅助命令等待期间继续处理 Pipe 和真实子进程退出，稳定 JSON 提交前再次检查健康状态，避免慢速 ENV 操作遮蔽心跳故障或进程退出。

current 使用临时软链接加 rename；切换任一失败时恢复两条旧链接并禁止启动。两条链接不能作为一组原子提交，断电重启后按 JSON 与实际 Kernel 重新协调，完成后才启动 LinkG。不删除任何 Release 目录。

无论失败回滚、正常恢复或 Supervisor 主动停止，都先确认旧 LinkG 回收。waitpid/waitid 失败时保留 PID 和进程锁并退避重试，不启动第二个 LinkG，不用无效 PID 掩盖回收失败。

## ENV 交互和边界

ENV 工具按 READ/WRITE 区分生命周期。读取保留 1000ms 硬超时，超时终止并回收只读工具；写入超过 1000ms 仅 WARN，之后每 30 秒提示仍在等待，直到工具真实退出并被 waitpid 回收。普通超时、LinkG 健康故障、Supervisor 正常退出和辅助等待错误均不向写工具发送 SIGTERM/SIGKILL。写工具永久挂起时保持等待和低频告警，不确认 JSON、不另启写入、不请求依赖此次写入的重启。

所有 ENV 读取及读改写/读回事务遵守 `/var/run/linkg-supervisor-env.lock`。父进程以 CLOEXEC 打开并持有 flock，只有写工具在 exec 前清除该描述符的 CLOEXEC，继承同一打开文件描述的锁；不执行显式 LOCK_UN 或删除锁文件。父进程正常持有到工具回收及读回结束。父进程被 SIGKILL 后，写工具仍持锁，新 Supervisor 必须等待旧操作释放锁后才能读写 ENV。

写工具不设置父进程死亡信号，并忽略 SIGINT/SIGTERM/SIGQUIT/SIGPIPE；标准输入输出及错误重定向到 `/dev/null`，避免父进程输出管道关闭间接中断写入。因此不保留写工具文本诊断，Supervisor 记录 PID、退出状态、耗时及后续读回错误。读取工具保留 PDEATHSIG(SIGKILL)。正常 SIGTERM/SIGINT 会取消确认，但先等当前写入结束再回收 LinkG 并退出；ENV 等待期间继续检查 Pipe、心跳和子进程状态。

该锁只协调遵守此约定的 Supervisor/辅助工具，不自动约束外部手工运行的 fw_setenv、U-Boot 或其它 Flash 写入者。遗留工具若关闭继承描述符、主动解除锁或自行 fork 脱离执行，也需单独核实；当前调用的 fw_setenv 是前台执行工具。本版仅新增upgrade_state，没有新增事务日志，锁文件只是运行期互斥载体。

使用设备实际的 /usr/sbin/fw_printenv、/usr/sbin/fw_setenv，通过 argv 执行，不拼接 Shell 命令。读取并校验 boot_partition、stable_boot_partition、bootcount、upgrade_available、bootlimit 及现有启动命令。四项更新通过 fw_setenv -s 批量提交，检查返回结果，再重新读取验证。即使失败时可能已写入，也不会把失败当作成功。

ENV 已正确时不重复写 Flash；候选进程重启不清零 bootcount，App-only 升级不作不必要的槽位切换。bootlimit 由用户自行配置为 5，本版不修改它，也不维护自己的启动计数。SDK bootcount_error 的判断为 bootcount > bootlimit，即 1～5 次尝试后在下一次启动进入 altbootcmd。

按用户补充边界，不实现 ENV 擦写中的突然掉电保护，不添加 ENV 冗余方案、专用事务日志或 U-Boot 修改。恢复前提是有效 ENV 可以被现有工具和 U-Boot 正确读取。ENV 自身损坏或两端选择不同快照时，不承诺只靠 JSON 顺序恢复系统。

只读调查另发现：SDK U-Boot 的 Sunxi 保存和导入使用递增 flags；当前 OpenWrt uboot-envtools 2021.01 的 NOR 分支使用布尔 flags。板上工具哈希与 SDK 产物相同。该兼容性差异未在本轮修改 SDK；部署前需单独核对读回结果与重启后 U-Boot 的实际选择，不能用 Mock 宣称通过。

## 构建与自动测试

在仓库根目录执行：

~~~bash
cmake -S supervisor -B /tmp/linkg-supervisor-v02-x86 \
    -DCMAKE_BUILD_TYPE=Release \
    -DLINKG_SUPERVISOR_BUILD_TESTS=ON \
    -DCMAKE_C_FLAGS="-Werror -Wmaybe-uninitialized"
cmake --build /tmp/linkg-supervisor-v02-x86 -j4
ctest --test-dir /tmp/linkg-supervisor-v02-x86 --output-on-failure
~~~

测试目标分离：

- 正式 Supervisor：真实 60 秒参数，无测试 ENV/reboot 替身入口。
- linkg-supervisor-test：缩短时间，必须提供独立 ENV、cmdline 和 reboot 文件替身，缺失时不访问真实 ENV。
- linkg-supervisor-release-timing：仅替换 I/O，保留正式 60 秒、4 秒参数，验证真实时间窗口。
- supervisor-release-faults：只在测试进程中注入 JSON rename、目录 fsync 和软链接失败。
- supervisor-env-lifecycle：验证超过 1 秒的写入、读取硬超时、正常停止等待、SIGKILL 后工具继续和锁继承、挂起及辅助监控错误。所有工具使用临时文件替身，只有测试清理可以显式释放或终止挂起 Mock。

ARM 交叉构建：

~~~bash
export T113_TOOLCHAIN_DIR=/home/dawn/t113_tina5.0_v1.2/prebuilt/rootfsbuilt/arm/toolchain-sunxi-glibc-gcc-830/toolchain
export STAGING_DIR="$T113_TOOLCHAIN_DIR"
cmake -S supervisor -B /tmp/linkg-supervisor-v02-arm \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/arm.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-Werror -Wmaybe-uninitialized"
cmake --build /tmp/linkg-supervisor-v02-arm -j4
~~~

根工程构建仍生成 output/bin/arm/linkg-supervisor。只链接仓库现有 cJSON 源码及 C/POSIX 接口，不依赖 LinkG 业务模块。

## 实板验证清单

本轮不覆盖开发板上的正式程序、不修改启动服务、不调整 bootlimit、不写真实 ENV 或 Kernel、不重启正式板。

此前只读调查曾观察到单boot分区、无kernel_slot及缺少A/B ENV；这属于历史快照，本轮未刷新开发板状态。实板必须具备正确A/B固件后再验证：

1. 实际 cmdline 槽位与每次 U-Boot 启动一致。
2. ENV 批处理写入、读回及重启后的选择一致，确认工具 flags 兼容性和执行时间。
3. 同 Kernel 回滚不重启、跨 Kernel 回滚重启后启动稳定 App。
4. ENV 先确认而 JSON 仍为候选时补完成功提交。
5. JSON 先回滚而 ENV 未恢复时继续恢复正确 Kernel。
6. 两条 current 链接在 JFFS2/overlay 上可原子替换并协调恢复。
7. 新 LinkG 实际正常退出及强杀后的板级资源恢复。

这些项目不能由 x86 Mock、ARM 编译或源码检查替代。源码职责见 [docs/MODULES.md](docs/MODULES.md)。


## OTA阶段、兼容迁移与新增测试

唯一JSON仅新增 upgrade_state：idle、verifying、installing、flashing、rebooting 由LinkG安装流程记录，testing、completed、rolling_back、rolled_back 由Supervisor在实际状态满足后记录，failed由安装器或启动恢复记录。completed、rolled_back、failed不会在普通重启时清零。安装中断且没有候选时，稳定App进入RUNNING后修正failed；跨Kernel回滚必须实际运行稳定Kernel且稳定App已READY后才记录rolled_back。状态显示写入失败不能跳过必要启动恢复。

共享解析与ENV工具位于 infra/base/linkg_release_state.c、linkg_atomic_file.c、linkg_boot_env.c，Supervisor版本与启动模块保留原接口并调用共享实现。ENV锁与安全写入生命周期不变，仍保留读取1000ms超时、写入1000ms告警/不强杀、30秒持续等待告警，以及写工具继承flock。

首次迁移必须先部署兼容Supervisor并让它实际启动，再使用新版LinkG发起OTA。新Supervisor向受管理子进程提供能力标记；LinkG接管真实Pipe时保存已验证父PID，升级前再次核对。旧运行版本不提供能力，1021会拒绝，不消费上传包或扩展JSON。仅替换磁盘文件不会改变正在运行的管理进程。可通过 linkg-supervisor --ota-capabilities 检查二进制支持，但它不代替运行期父进程检查。

后续管理进程更新采用包内 app/usr/sbin/linkg-supervisor 及system_install.json原子替换 /usr/sbin/linkg-supervisor，rc/procd在重启后加载新程序。共享Supervisor不随App/Web回滚，新版本必须兼容候选和上一版稳定LinkG。实际安装期间仍由旧程序运行。详见 upgrade/SUPERVISOR_UPDATE.md。

新增隔离集成测试：

```bash
python3 test/upgrade/build_tests.py
python3 test/upgrade/test_ota.py
python3 test/upgrade/test_system_install.py
```

浏览器测试为 test/upgrade/test_web.cjs，使用Playwright和本地Chromium/Edge；可设置 LINKG_PLAYWRIGHT 与 LINKG_TEST_BROWSER 指定安装路径。所有请求均拦截为替身，测试不会操作开发板。

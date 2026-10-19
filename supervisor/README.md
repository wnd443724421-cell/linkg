# LinkG Supervisor V0.2

Supervisor 独立常驻，负责选择 v1/v2、启动唯一的 LinkG、监控 READY/HEARTBEAT/RESTARTING/STOPPING、确认候选版本及失败回滚。保留 V0.1 的匿名 Pipe 单字节协议、waitpid 回收、TERM/KILL 和稳定版本退避恢复。

本版不安装 OTA 包、不校验或刷写 Kernel、不提供 Web/LinkG 升级请求接口，不改 U-Boot、设备树、分区表或开机脚本。

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

版本 JSON 只有以下结构，空对象表示未安装，不能启动：

~~~json
{
  "schema_version": 1,
  "release_partition": "v1",
  "stable_release_partition": "v1",
  "releases": {
    "v1": {"kernel_slot": "boot_a"},
    "v2": {}
  }
}
~~~

schema_version 只能为 1，Release 只能为 v1/v2，Kernel 只能为 boot_a/boot_b。拒绝未知字段、重复键、损坏 JSON、内嵌零字符和未安装的稳定 Release。不增加事务字段或专用事务日志。

Supervisor 是状态 JSON 的唯一持久化写入者。当前可在 Supervisor 停止时预置候选 JSON 测试；正式运行时不得由 LinkG、Web 或维护脚本直接并发改写状态。未来安装模块通过专用请求由 Supervisor 提交候选，本版不实现该请求接口。

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
| PREPARE（未来安装请求） | JSON 设目标候选、保留旧 stable | ENV 设候选 Kernel 及旧稳定 Kernel，开启试启动保护 | reboot |
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

本轮不覆盖正式程序、不修改启动服务、不调整 bootlimit、不写真实 ENV 或 Kernel、不重启正式板。

当前 192.168.1.1 和 192.168.3.1 的只读结果均为单 boot 分区、无 kernel_slot、缺少 A/B ENV，不能直接用作本版 A/B 验收。具备正确 A/B 固件后仍需验证：

1. 实际 cmdline 槽位与每次 U-Boot 启动一致。
2. ENV 批处理写入、读回及重启后的选择一致，确认工具 flags 兼容性和执行时间。
3. 同 Kernel 回滚不重启、跨 Kernel 回滚重启后启动稳定 App。
4. ENV 先确认而 JSON 仍为候选时补完成功提交。
5. JSON 先回滚而 ENV 未恢复时继续恢复正确 Kernel。
6. 两条 current 链接在 JFFS2/overlay 上可原子替换并协调恢复。
7. 新 LinkG 实际正常退出及强杀后的板级资源恢复。

这些项目不能由 x86 Mock、ARM 编译或源码检查替代。源码职责见 [docs/MODULES.md](docs/MODULES.md)。

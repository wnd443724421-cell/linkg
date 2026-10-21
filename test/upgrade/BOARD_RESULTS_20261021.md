# LinkG OTA 实板验收记录（2026-10-21）

## 环境与方法

- 设备：T113-i，Tina Linux 5.4.61（2026-10-19 内核），SPI NOR 32 MiB，RAM 512 MiB。
- COM3 / 115200 持续采集启动日志，SSH 192.168.1.1 用于部署和只读交叉核对。
- 源码基线：GitHub 48ac0a6，加本次 Web 路由与 MTD 同步修复。用户并行修改的 Switch 代码及临时脚本未纳入测试构建或提交。
- 浏览器：真实 Edge / Playwright，打开设备完整首页 `#/system/upgrade`；不拦截请求、不用 Mock 替代板上 ENV、MTD、重启或健康检查。
- 每次安装均通过页面选择文件、上传、勾选确认、点击开始升级；观察断线重连及持久化结果。
- 板上初始为旧 shell watchdog、无 Supervisor/版本 JSON。备份后部署兼容 Supervisor，配置唯一 procd 启动入口，停用旧 shell watchdog；保持 BOA、procd 硬件喂狗及现有分区。
- 板上实际 `bootlimit=2`，本次未修改次数限制。超过限制即第三次启动进入 altbootcmd。
- 故障操作前已把 App/Web/启动文件、A/B 完整分区和 ENV 备份到电脑。故障测试始终写非活动 Kernel。

## 本次修复

1. Web 注册名从 `#/system/upgrade` 改为路由器实际使用的 `system/upgrade`。修复前真实页面只有 HTML，未挂载状态轮询及按钮事件。同步更新升级目录及缓存指纹。
2. `upgrade_kernel.c` 不再对 MTD 字符设备调用 fsync。Linux 5.4 mtdchar 没有 fsync 文件操作，真实设备返回 EINVAL，曾导致整分区写入正确仍被判断为失败。NOR 同步写入后检查关闭结果，利用 mtdchar_close 的 mtd_sync，再重新打开字符设备读回完整分区 SHA-256。
3. MTD 测试替身改为模拟真实 fsync(EINVAL)。修复前跨内核用例失败（failed != rebooting），修复后 20 项 OTA 用例全部通过。
4. 新增完整 index/bootstrap/router 浏览器回归，避免手工 mount 页面掩盖注册名错误；原 Web 上传/重连/卸载回归也通过。

## 实板场景

| 场景 | 真实结果 |
|---|---|
| 正常 App-only | 上传、安装、实际重启、连续 RUNNING 60 秒后 completed；App/Web 从 v1 切换并确认为 v2；Kernel 保持 boot_a |
| 篡改 LinkG 但不更新 FILES 清单 | 校验失败，failed；未提交候选、未重启，稳定版本及两个 Kernel 哈希保持 |
| LinkG ELF 头损坏，FILES 清单正确 | 安装并重启后 execv 报 Exec format error，子进程退出 127；Supervisor 回收后恢复稳定 App/Web，rolled_back |
| 测试包遗漏 tun.ko | 候选启动明确报缺少驱动并退出，自动恢复稳定 App/Web。此包是 Git archive 漏掉忽略文件的测试构造错误，不计为正常升级通过；正式包包含四个匹配当前内核的 ko |
| Kernel Android 头损坏，FILES 清单正确 | 非活动 boot_b 写入、关闭、完整读回及 ENV 提交成功；U-Boot 实际尝试 boot_b，Wrong Image Format 后停在命令行。观察 34.65 秒无自动复位；通过 COM3 手动 reset 两次，bootcount 1→2→超限，altbootcmd 恢复 boot_a，Supervisor 恢复稳定 v2，rolled_back |
| Kernel 启动后 Panic | 测试镜像仅追加 init=/sbin/linkg-ota-missing-init panic=0，保留有效 Android 头和真实 Kernel。boot_b 约 1.45 秒启动 16 秒看门狗，约 2.4 秒 Panic；两次硬件自动复位后超过 bootlimit，自动回 boot_a，Supervisor 恢复稳定 App/Web，rolled_back。整个场景串口命令条数未变化，无人工 reset |
| 正常 A→B Kernel 更新 | 用正常镜像的无效尾部添加测试标记，使完整分区哈希不同而执行真实擦写。实际 cmdline=boot_b，试运行 60 秒后 ENV 和 JSON 确认为 v1 / boot_b；completed |
| 正式包 B→A Kernel 更新 | 正式正常包真实擦写非活动 boot_a，cmdline=boot_a，60 秒健康确认后 v2 / boot_a 成为稳定版本；completed。启动时一次 ENV 准备读取错误由 Supervisor 退避重试后恢复，未错误启动或提前确认 |
| completed 后普通重启 | boot_id 变化，仍为 v2 / boot_a、completed、trial=false；bootcount=0、upgrade_available=0；一个 Supervisor 和一个 LinkG，旧 shell watchdog 未运行 |

正常 A→B 的 CONFIRM 中，真实 fw_setenv 耗时 1569ms：1002ms 输出 WARN，继续等待，1569ms 正常退出，随后才确认 JSON。B→A 的确认写入同样耗时 1535ms，并按此策略完成。现场验证了超过 1 秒不会被普通超时强杀。

## 交付与边界

- 正式 ARM LinkG SHA-256：`08fbdc4e45ad73060f4cf6f114c5ed56970481ab29cbeea49519872906e18b52`。
- 正式包 `upgrade/output/linkg-upgrade-v1.0.0.tar.gz`：6,780,318 字节。
- 正式包 SHA-256：`32679b6841fbf623a2829ed15fb7d7b56a4f4c144c31166e838fb980b987b9ae`。
- 正式包内 Kernel 为原始正常镜像，SHA-256 `1ba1efb0eee81f7370cb5a099274bde930dfa5f892550450bf7f2424effb873c`；没有打包任何故障注入镜像。
- App-only 正常包、损坏 LinkG、头损坏 Kernel、Panic Kernel、尾部标记 Kernel 均为独立测试包，未放入正式升级输出或提交 Git。
- 按用户明确选择，升级采用包内默认配置，不迁移旧 linkgUser.json。
- x86 Release、ARM Release 编译通过；仅保留既有 wifi_rx.c:60 枚举比较警告，无本轮新增编译警告。
- 本轮不是任意早期故障的无人值守保证。BOOTON 在 watchdog 驱动 probe 后才启动；无法进入该阶段的镜像损坏，已实测需要人工复位才能推动 Boot Count。改进此边界需另行设计 U-Boot 看门狗或加载失败分支。
- 最终 UDISK 使用 5772 KiB / 10240 KiB，剩余 4468 KiB；MemAvailable 451524 KiB。故障内核已移除，boot_a 恢复原始正常镜像，boot_b 为已实际启动确认的正常镜像（仅带无效尾部测试标记）。
- 普通重启曾观察到 web_server.c:609 监听绑定返回 -99（EADDRNOTAVAIL），导致首次 LinkG 启动退出；Supervisor 退避重试后进入 RUNNING，boot_id 未再次改变，最终稳定运行超过 60 秒。该现象不属于本次两项 OTA 修复，保留为单独的启动时序问题。
- 本轮未执行真实 ENV 擦写中断电、整机物理断电、满业务压力和长时间老化。没有修改 SDK、U-Boot、看门狗驱动或 NOR 布局。
- 详细 HTTP、COM3、文件日志和备份保存在本地 board-ota-20261021；含设备配置的完整备份不提交公开仓库。

# LinkG 信道扫描实现与验收说明

> 同步说明：2026-11-13 已按用户要求合入正式源码并同步 SDK。下文为同步前的审核与实板验收记录。

本次实现位于 `/home/dawn/project/linkg_v1_codex`。正式目录 `/home/dawn/project/linkg_v1` 未同步本次改动；没有提交或推送。板端 AP（172.28.1.1）与 STA（172.28.2.1）已运行本次 review 程序。

## 页面与使用

进入 **诊断工具 → 信道扫描**，确认可以暂时中断 Wi-Fi 后开始。固定扫描 20 轮，范围为 149、153、157、161、165（5745、5765、5785、5805、5825 MHz）。

页面提供两张表：

1. 信道统计：平均繁忙率、单轮最高繁忙率、噪声中位数及范围、有效采样轮数、累计采样时间。缺少有效数据时显示“未测得”。平均繁忙率按累计 busy / 累计 active 计算。
2. 附近网络：信道/频率、SSID、RSSI、BSSID。按信道递增，同信道内信号由强到弱、BSSID确定顺序。按BSSID去重，保留最近一次有效信号，不过滤 LinkG 自身组网 AP，不返回安全认证 flags。未广播名称显示“未显示名称”。最多保留128条，达到上限会明确提示。

不推荐或自动修改信道。页面解释如何结合繁忙率和RSSI判断、40/80MHz的组合占用关系，且165在当前40/80MHz配置下标记不可用。测量使用20MHz统计口径，不能当作10MHz窄带噪声标定值。设备时钟未校准时不显示误导性的1970日期。

## 实现分层

| 所属层 | 主要文件 | 职责 |
|---|---|---|
| Network管理 | service/network/network_manager.c；include/linkg/service/network/linkg_network_task.h | 通用Wi-Fi独占维护生命周期：停止并回收正常Owner、运行独占任务、恢复原Owner、通知结果。扫描线程独立，Manager继续管理蜂窝Owner |
| Network扫描任务 | service/network/network_scan.c；include/linkg/service/network/linkg_network_scan.h | 异步任务编号、一致性状态快照、取消、结果缓存，以及扫描错误/恢复错误分别记录 |
| Wi-Fi模块 | modules/wifi/wifi_scan.c；modules/wifi/wifi_scan_result.c；include/linkg/modules/wifi/linkg_wifi_scan*.h | 对外扫描入口、结果初始化、五频率过滤、survey有效性校验、统计累计、BSSID去重 |
| Wi-Fi平台 | platform/wifi/wifi_scan.c；platform/wifi/wifi_scan.h | 专用无network项WPA、控制socket、被动扫描事件、iw survey子进程、有界等待与清理 |
| Web后端 | service/web/handlers/web_handler_wifi_scan.c；service/web/web_dispatch.c | 请求校验、JSON结果、诊断/升级/配置互斥 |
| Web前端 | www/view/diagnostics_wifi_scan.html；www/js/api/wifi_scan.js；www/js/pages/diagnostics_wifi_scan.js；www/css/pages/diagnostics_wifi_scan.css | 复用原路由与资源加载框架；确认、进度、取消、恢复提示及两张结果表 |

main没有增加扫描业务。未增加桥接、Wi-Fi DHCP、NAT或手机路由功能。

## 执行顺序与恢复约定

1. Web校验当前应用实例、用户确认、其他诊断/升级及配置事务，分配任务编号。预留1秒发送响应。
2. Network建立维护事务，停止并join原Wi-Fi Owner，完成模块Stop/Deinit。
3. 独占线程启动专用WPA，配置无network项。每轮BSS_FLUSH → 被动SCAN → 等待SCAN-RESULTS → 读取survey → 逐条读取BSS。20轮串行执行，避免正常STA监控扫描覆盖survey。
4. 成功、取消或失败均关闭控制socket、停止/reap扫描WPA和survey子进程，清理本任务临时目录。
5. Network重新Init/Start原Wi-Fi Owner。服务恢复完成后结束任务；这表示服务启动成功，重新关联及组网连通还可能需要片刻。

关闭页面不会取消后台任务，返回页面会查询现有状态。经Wi-Fi虚拟组网地址访问AP时，扫描期间页面可能不可达；恢复后自动查询结果。以太网和蜂窝不为扫描整体重启，扫描不退出LinkG进程，不改用户配置。

资源未确认释放时，禁止启动第二个正常Wi-Fi Owner并锁存资源故障；测试覆盖这一分支。取消不会跳过恢复。配置修改、新诊断和升级在扫描/恢复期间被拒绝；其他诊断/升级正在运行时也不能启动扫描。

## Web协议

| CMD | 操作 | 参数 |
|---|---|---|
| 1033 | 提交扫描 | confirm=true；instance为当前应用实例 |
| 1034 | 查询任务及结果 | 无；返回job_id、instance、state、active、rounds/total_rounds、started_at、channels、bss、scan_error、restore_error等 |
| 1035 | 取消任务 | job_id及instance；旧实例或旧任务编号不能取消后续任务 |

状态依次为starting、running、restoring，收尾后为completed、cancelled或failed。未运行时为idle。survey缺失字段返回null。BSS对象仅含bssid、frequency_mhz、signal_dbm、ssid。

## 实际耗时

以下为本次宽带40MHz双板近距离环境的一轮完整计时，不是固定保证：

| 扫描端 | Web取得完成结果 | 两端重新互通 |
|---|---:|---:|
| STA | 15.29秒 | 19.99秒 |
| AP | 24.33秒 | 24.37秒 |

STA首次看到running为1.61秒，20轮结束进入restoring为11.95秒，服务恢复完成为15.29秒，之后等待重新关联至约20秒可达AP。AP经虚拟Wi-Fi组网路径访问，中间不可达，因此未单独推算各阶段。另一次最终AP浏览器完整扫描至显示结果为21.49秒，说明重新连接耗时存在波动。

## 验证结果

- ARM Release交叉编译通过。AP与STA最终可执行文件SHA-256相同：`4f62b3dba0b218e942836731d9df26b1445134faa3fb1f7755cb9430c23e5e9f`。
- 12项回归测试全部通过，启用Wall/Wextra/Wpedantic/Werror及ASan/UBSan。新测试覆盖survey缺失、零繁忙、无效/重复数据、采样上限、BSSID去重和列表上限；真实Network线程测试覆盖成功、测量失败、恢复失败、重复提交、取消、任务中退出、蜂窝独立重启及资源未释放时禁止重启。
- AP与STA通过真实Chrome、真实板端Boa/CGI完成20轮。两端五信道都获得20份有效统计。现场AP数量随扫描时刻变化，约16～17条。
- STA实测运行中取消后恢复；对本次专用扫描WPA发送TERM模拟异常，任务报告扫描失败但恢复正常Owner并重新关联AP。
- 扫描、取消和异常恢复前后的应用instance未变化；完整用户配置导出结果一致。
- 真实页面验证：离开页面后后台继续，返回后可取消；配置写入在扫描期间明确被互斥拒绝。
- 列表四字段、信道排序、165提示及768/375/320像素布局通过；手机端仅表格区域独立横向滚动。浏览器未记录脚本异常。
- 收尾确认正常STA WPA与AP hostapd已恢复，专用扫描WPA和任务目录无残留，172.28.1.1/172.28.2.1双向ping通过。

本次实板验收使用宽带40MHz；没有把10MHz窄带、80MHz或远距离环境声称为已验收。未执行整机掉电重启；已验证部署后的应用重启和扫描后的Wi-Fi Owner重建。

## 构建与审核资料

独立构建目录：`build/channel-scan-arm`、`build/channel-scan-tests`。没有复用从正式目录复制而来的CMake缓存，避免回写正式源码。

本地附件保留审核差异channel-scan-review.patch、测试日志、计时JSON、页面截图，以及双板原程序和原index/version文件的board-original-backups.zip。板端当前启动周期内也保留 `/tmp/linkg-channel-scan-20261113/backup`。配置文件没有替换，未进行固件OTA或修改Supervisor版本状态。

正式源码中与本次23个改动路径对应的内容已逐一与初始基线核对一致。请先审核review目录；本次没有向正式目录同步。

# OTA 隔离验收记录（2026-10-20）

基于工作区最新提交 `0aebd9d` 接入本轮修改，测试运行于 Ubuntu 24.04。本记录不代表开发板升级验收。

- x86 Release：LinkG、Supervisor及独立测试目标编译通过。
- ARM Release：Tina GCC 8.3.0 完成 LinkG、Supervisor与上传CGI交叉编译。
- 原有Supervisor回归：5个CTest测试组全部通过，含正式60秒时间参数和ENV写进程生命周期。
- OTA集成：20个unittest测试全部通过，包含真实CGI、Worker、Web1021/1022、模拟NOR和ENV、App-only、相同/不同Kernel、失败补偿、三种事务中断恢复、历史终态和管理进程迁移限制。
- 系统文件安装：20个unittest测试全部通过，含运行中程序的原子替换、相对app路径、旧app前缀、权限及路径限制。
- 浏览器：无头Edge运行实际页面资源并拦截所有请求，通过原始字节上传、用户确认、状态查询、重启断线重连、真实软件版本、错误显示、退出清理和重新进入。窄屏测试隔离了原应用全局960px最小宽度，不声明整站已适配手机。
- 实际生成的ARM升级包通过C归档解压和Manifest校验。

构建保留一个原有警告：`modules/wifi/wifi_rx.c:60 -Wenum-compare`，不属于本次OTA修改范围。新增OTA测试驱动及涉及的后端模块使用 `-Wall -Wextra -Wpedantic -Werror` 构建通过。

包路径：`upgrade/output/linkg-upgrade-v1.0.0.tar.gz`，含本次ARM程序和Web以及原有待打包Kernel（本轮未重编SDK内核）。

SHA-256：`f4b73040a989123deca07a6256df57341f83c2bdd231bb89244a0ac9663110e2`

首次实板迁移必须先部署并实际运行支持扩展状态的Supervisor，再让新版LinkG发起OTA；新Supervisor需要兼容旧稳定LinkG。仅替换磁盘文件不改变旧运行进程。本轮未修改开发板ENV、未刷写真实MTD、未重启开发板；T113上的实际升级、重启、看门狗及U-Boot回退仍需单独验收。

ENV自身擦写掉电的风险按用户确认的边界保留，不添加ENV事务日志或冗余实现。实际rc/procd启动入口仍应指向 `/usr/sbin/linkg-supervisor`。

## 2026-10-21 实板补充

已完成真实 Web 上传、损坏 LinkG、损坏 Kernel 头、内核 Panic 自动回退、双向 Kernel 升级和普通重启验证。发现并修复路由注册和 MTD fsync 两项问题；详细通过结果、人工复位边界和保留问题见 [BOARD_RESULTS_20261021.md](BOARD_RESULTS_20261021.md)。

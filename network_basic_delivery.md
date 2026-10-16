# 基础网络与 DHCP 统一管理交付说明

实现基线：GitHub `wnd443724421-cell/linkg` 的 `main`，提交 `f2d699314cb0029bb4d8ef823cb1d2fe130d2487`，与 Ubuntu `/home/dawn/project/linkg-v2` 一致。

开发与构建目录：`/home/dawn/project/linkg_codex/linkg-v2`，分支 `codex/network-basic`。

原 codex 副本存在未提交修改，已保存为同级目录 `linkg-v2.previous-20261015`，另有完整压缩备份 `linkg-v2-before-network-basic-20261015.tar.gz` 与补丁 `before-network-basic-20261015.patch`。原始最新源码目录未被覆盖。

## 行为与协议

- `NETWORK_BASIC_GET = 1015`：返回 `running`、`saved`、`pending_restart`、`runtime_confirmed`、`persistence_confirmed`、`restart_failed` 和服务 `instance`。
- `NETWORK_BASIC_SET = 1016`：请求包含 `node_id`、`virtual_network`、`ethernet_network` 和 `dhcp: {enabled, default_gateway}`。仅覆盖完整配置快照中的五项，不覆盖 traffic_rules、Wi-Fi、Cellular、UART 等内容。
- `NETWORK_BASIC_RESTART = 1017`：独立确认请求，参数是上述五项加 `instance`。后端检查确认配置仍与保存值一致，防止陈旧页面确认其他人的更改。
- 五项未改变：`apply=none`，不持久化，不重载，不重启。
- 只改变 DHCP 开关：`apply=dynamic`，通过 `linkg_udhcp_reload()` 应用。若已有地址等待重启，DHCP 仍使用原运行地址池。
- 地址参数改变，包括同时改变 DHCP：`apply=restart_required`，仅保存。用户确认后才发出重启请求，绝不提前重载 DHCP。
- 完整构造成功 JSON 响应之后才提交配置。保存、替换或 DHCP 重载失败都会尝试恢复；包括 save 已经 rename、仅目录 fsync 失败的情况。
- 恢复无法确认时明确返回错误。GET 同时标记运行或持久配置不确定；持久配置不确定时禁止根据内存快照直接重启。
- Web 服务线程完整发送确认响应、关闭本次 TCP 连接后，给予独立 CGI 输出短暂宽限，再通知 `LINKG_EXIT_ACTION_RESTART_SERVICES`。不调用 Linux reboot，不新增长期管理线程。
- Web 最后启动。每轮 init 都生成不同的 instance；浏览器只有在 instance 改变且运行值与保存目标一致时才报告生效。失败、响应丢失、重连超时均有明确提示，地址改变时提供新管理入口。

## 运行地址与租约

网络服务保留本轮启动地址快照。基础网络摘要、概览、UART 地址示例和远端节点路由判断均读取运行地址；保存后的未来地址不会提前混入这些路径。

DHCP 每次启动前检查 `/var/run/linkg/udhcp/lease-pool-v1`。记录包含版本、Ethernet 网络、掩码、服务端地址和地址池起止值，跟随租约目录保存，不依赖进程内存或 Web 页面。记录匹配时复用租约；缺失、损坏、过长或不匹配时，先使旧租约失效，再通过已有原子写接口提交新标识。清理或标识写入失败则停止启动，避免沿用错误网段。运行目录被设备上电清空时会安全创建空租约与新标识。

地址池保持 `.2–.254`，不增加静态预留。`default_gateway=false` 继续仅通过 121/249 下发虚拟网段路由；开启后在 121/249 中同时下发默认路由，兼容 Router Option 的忽略语义。

## 页面与客户端帮助

保留三列摘要、并排网段卡片、一体式 IPv4 输入和浅灰固定字段。原静态参考页已接为实际页面，独立 DHCP 菜单已删除；原 main 没有独立 DHCP 页面文件，因此没有额外可删除的 DHCP HTML。旧 `#/network/dhcp` 入口转到基础网络。

DHCP 关闭时禁用默认网关交互，但保留布尔值。保存、还原、刷新、前后端校验、修改检测、等待、确认重启及重连均已实现。帮助默认折叠，使用运行配置生成 Windows active/persistent、Linux ip 和 NetworkManager 永久命令，支持网卡/连接名称和客户端 IP 输入及复制。参数通过文本绑定展示，并校验或引用命令参数。

按实际更改显示提示：开启 DHCP 提醒重新获取 IP；关闭时提示静态 IP；默认网关改变提示更新租约；Ethernet 改变提示更新 DHCP 或静态地址、新管理入口；节点 ID 改变提示客户端通常不必更新 IP。拔插网线只是更新 DHCP 的可选方法。跨网段变更时 Windows 先执行 `ipconfig /release` 再执行 `ipconfig /renew`，避免客户端继续向旧 DHCP Server 单播续租；Linux 使用指定 NetworkManager 连接的 down/up 命令重新获取地址。

静态示例不添加 Internet 默认路由，提醒使用连接 LinkG 的专用网卡并记录原配置。NetworkManager 示例需要填写已有连接名称；非 NetworkManager 系统需用 netplan、systemd-networkd 等机制。提示 DHCP 路由一般无需重复手工添加、静态用户需手工路由，以及 DHCP/静态混用的地址冲突风险。

参考命令格式：[Microsoft netsh interface](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/netsh-interface)、[NetworkManager settings](https://networkmanager.pages.freedesktop.org/NetworkManager/NetworkManager/nm-settings-nmcli.html)。

## 需要同步到源码的文件

以下 **18 个运行源码/页面文件**需作为一组同步，不要只复制 HTML：

| 路径 | 作用 |
| --- | --- |
| `core/udhcp/linkg_udhcp.c` | 跨生命周期租约地址池识别、故障后的相同配置恢复 |
| `include/linkg/service/network/linkg_network.h` | 运行地址快照接口 |
| `service/network/linkg_network.c` | 运行地址快照查询 |
| `service/uart/uart_route.c` | UART 使用实际运行虚拟网段 |
| `service/web/handlers/web_handler_network_basic.c` | 新增基础网络 GET/SET/重启确认处理 |
| `service/web/handlers/web_handler_overview.c` | 概览使用实际运行地址 |
| `service/web/handlers/web_handler_uart.c` | UART 地址示例使用实际运行地址 |
| `service/web/linkg_web.c` | 捕获本轮 Web 运行配置 |
| `service/web/web_dispatch.c` | 新命令注册、写事务互斥、退出时拒绝写入 |
| `service/web/web_internal.h` | 命令编号与内部接口 |
| `service/web/web_server.c` | 应答完成后延迟通知重启 |
| `www/view/network_basic.html` | 基础网络、DHCP、折叠帮助 |
| `www/css/pages/network_basic.css` | 原风格上的 DHCP/帮助/状态样式 |
| `www/js/api/network_basic.js` | 新增页面 API |
| `www/js/pages/network_basic.js` | 新增页面逻辑与命令生成 |
| `www/js/pages/index.js` | 原 DHCP 入口兼容 |
| `www/index.html` | 注册新脚本、移除独立菜单 |
| `www/version.json` | 静态资源缓存版本 |

完整开发交付还包括 **7 个测试文件**和本说明：

- `test/network_basic/run_tests.py`
- `test/network_basic/network_basic_fixture.c`
- `test/network_basic/udhcp_fixture.c`
- `test/network_basic/commands_test.js`
- `test/network_basic/browser_test.cjs`
- `test/uart/uart_route_peer_test.c`
- `test/uart/uart_test_server.c`
- `docs/network_basic_delivery.md`

不需要修改出厂 JSON、CMake 源码列表或 DHCP 默认地址池；原 main 已支持 DHCP 两项配置及默认值，CMake 自动发现新增 Handler。

## 已执行验证

1. Ubuntu x86：`cmake -S . -B build/network-basic-x86 -DARCHITECTURE=x86` 及完整构建。
2. Tina ARM：`cmake -S . -B build/network-basic-arm -DARCHITECTURE=arm -DCMAKE_TOOLCHAIN_FILE=toolchains/arm.cmake`，设置 `STAGING_DIR` 为现有 Tina toolchain 目录；应用与 `linkg_fast_nat.ko` 构建成功。产物识别为 ARM EABI5 ELF 与 ARM 内核模块。
3. `python3 test/network_basic/run_tests.py`：真实网络解析/校验、Handler、JSON、文件及 TCP 服务代码；AP/STA 默认配置、旧 DHCP 缺省兼容、五项修改32组合、陈旧确认、写事务冲突、rename后保存失败、内存替换失败、DHCP恢复失败、JSON分配失败、发送失败取消重启、生命周期拒绝、真实 TCP 应答先于重启通知。ASan/UBSan 开启。
4. 同一测试中的 DHCP 文件场景：网段改变、相同网段复用、deinit/init、关闭期间改网段后启用、记录缺失/损坏、租约缺失、标识写入失败阻止启动、另一个进程复用可信记录；核对两种模式的121/249内容及完整地址池。
5. `node test/network_basic/commands_test.js`：六组命令、实际网段派生、接口/连接名、无新增 Internet 默认路由、租约更新命令、参数验证与引用。
6. `node test/network_basic/browser_test.cjs`：生产 HTML/CSS/JS，明确模拟 API；默认显示、禁用保值、动态提示、重启取消/确认、运行与保存差异、响应丢失、重启失败、重连超时、新地址入口、输入校验、复制、XSS文本绑定、读取失败恢复与旧菜单入口。桌面和紧凑宽度截图。
7. 原 `python3 test/uart/build_tests.py` 与 `python3 test/uart/test_api.py`：原 UART/PTY/TCP、GPIO故障、配置回滚、重复重载与资源计数回归，ASan/UBSan 开启。

## 尚未验证的硬件与客户端场景

- 没有在 T113 板上运行此次程序、修改实际网口、加载 NAT 模块或执行服务重启。
- 节点 ID 改变后 Wi-Fi/TUN/本机虚拟 IP/Ethernet MAC/NAT 的完整硬件同步，以及真实初始化失败后的设备日志与恢复，需要上板确认。
- 未实际断电上电；已验证独立进程和缺失记录对应的文件处理路径。
- 未真实运行 udhcpd 收发 DHCP 报文，也未实测 Windows/Linux 各版本客户端的121/249、默认路由及续租行为。
- 生成的 Windows、Linux 临时/永久命令经过参数与浏览器测试，没有在实际客户端执行；NetworkManager及网卡差异需客户端验证。
- 浏览器重启、响应丢失与超时场景使用模拟 API；真实 CGI/网络断开到新管理地址的端到端过程需要设备联调。

部署时同步 ARM `output/bin/arm/linkg` 与整个 `www/`，保持页面与协议版本一致。无需因本功能单独替换内核模块；本次构建验证了其兼容性。上板前保留当前设备配置与程序副本，按上述场景逐项验收。

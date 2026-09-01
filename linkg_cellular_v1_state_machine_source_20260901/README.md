# LinkG Cellular V1 Runtime State Machine

本源码包完成 LinkG 蜂窝模块 V1 的运行闭环，不执行 Git 提交或远端仓库修改。

## 基线

设计和接口按 GitHub `wnd443724421-cell/linkg` 的 `main` 分支整理，开发时已知 HEAD：

```text
63a0a83c59bbc584b81de2dbb7dc842f56d7ed5f
```

`cellular_status` 已按当前 9 组事实 deadline 版本作为依赖，不在本包中重复替换。

## 生产文件

```text
modules/cellular/cellular_runtime.h
modules/cellular/cellular_runtime.c
modules/cellular/cellular_monitor.h
modules/cellular/cellular_monitor.c
modules/cellular/linkg_cellular.c
platform/cellular/rg255/rg255_runtime_urc.h
platform/cellular/rg255/rg255_runtime_urc.c
```

## 模块边界

```text
cellular_monitor
    RG255 URC -> 合并事件 -> eventfd唤醒Owner
    不执行同步AT，不修改Runtime，不做恢复

cellular_status
    Refresh Mask / Deadline -> 查询Truth -> 发布唯一快照
    不解析URC，不执行拨号动作

cellular_runtime
    保存Owner状态、状态期限、失败状态、SIM会话和PIN单次尝试保护
    不访问硬件，不决定恢复动作

linkg_cellular
    生命周期 + 唯一network-cell Owner状态机
    消费Monitor事件、驱动Status、执行AT动作、处理状态失败和统一退避
```

## 正常状态路径

```text
WAIT_SIM
    -> CHECK_SIM
    -> ENTER_PIN / WAIT_PIN / WAIT_PUK
    -> WAIT_REGISTRATION
    -> PREPARE_PDP
    -> ACTIVATE_PDP
    -> WAIT_PDP
    -> START_NETDEV
    -> WAIT_NETDEV
    -> WAIT_HOST
    -> VERIFY_CONNECTIVITY
    -> ONLINE
```

`ONLINE` 的 V1 条件为：

```text
IPv4地址、掩码、网关与RG255 Expected IPv4一致
Global IPv6属于RG255 Expected IPv6前缀
IPv6默认网关与RG255 Expected IPv6网关一致
IPv4公网探测成功
IPv6公网探测成功
```

当前公网目标为 `www.baidu.com`，通过 `usb0` 绑定执行 `ping` / `ping6`。

## Bootstrap

`linkg_cellular_start()` 只负责准备 Modem 运行环境：

```text
打开 /dev/ttyUSB1，115200 8N1
AT / ATE0 / CMEE / QSCLK
Query-before-write检查：
    USBNET = ECM
    Network Card = NIC
    Network Mode = 配置值
    QSIMDET = enabled + 硬件有效电平
    QSIMSTAT = enabled
全部不匹配项一次性写入
必要时只执行一次 CFUN=1,1
重建AT Channel并再次验证
注册Monitor
开启 CEREG / C5GREG / QCSQ URC
启动Status
```

## V1 失败策略

Runtime 只记录失败状态和错误码，不猜测欠费、运营商限制、APN权限等具体原因。

```text
SIM阶段失败             -> CHECK_SIM退避重试
注册阶段失败            -> WAIT_REGISTRATION退避重试
PDP阶段失败             -> 清理数据会话后从PREPARE_PDP重试
Netdev/Host/Verify失败   -> 清理数据会话后从WAIT_REGISTRATION重试
SIM拔出                 -> 清理数据会话、结束SIM会话、回WAIT_SIM
PIN未配置或尝试失败      -> WAIT_PIN，不自动重复输入PIN
PUK_REQUIRED             -> WAIT_PUK，不自动处理
```

## V1 明确限制

```text
单条AT命令仍为同步阻塞；尚未实现at_channel_cancel_current()
SIM拔出事件会在当前AT命令返回后由Owner消费
未实现分层高级Recovery和Modem自动重启恢复
未接入Linux Netlink，Host事实仍由Status watchdog维护
公网验证依赖目标板存在ping，并优先使用ping6验证IPv6
AT设备当前固定为/dev/ttyUSB1
```

## 应用

```bash
tar -xzf linkg_cellular_v1_state_machine_source_20260901.tar.gz
cd linkg_cellular_v1_state_machine_source_20260901

./apply_to_repo.sh ~/project/linkg_v3

cd ~/project/linkg_v3
git diff --check
./build.sh
```

应用脚本不会执行 `git add`、`git commit` 或 `git push`。

## Host检查

```bash
./validation/run_host_checks.sh
```

该检查使用 mock AT / Status / Monitor 验证状态机结构，不替代 ARM 交叉编译和 RG255 实机测试。

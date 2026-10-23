# LinkG GPIO 灯语与硬件使用说明

> 文档类型：产品手册素材 / 嵌入式实现说明
> 文档版本：V0.1（基于源码整理，待产品与实机验证后定版）
> 整理日期：2026-10-13
> 适用范围：LinkG AP / STA 设备 GPIO 管理模块
> 代码依据：`service/gpio/linkg_gpio_manager.c`、`include/linkg/service/gpio/linkg_gpio_manager.h`、`include/linkg/platform/linkg_system_resources.h`。
>
> **边界说明**：本文反映现有源码的运行逻辑，不代表已完成 LED、按键和 GPIO 硬件实测；实际物理引脚连接、MCU 间接控制逻辑及面板灯标识应以硬件原理图和实机验证为准。

---

## 1. 功能概述

LinkG GPIO 管理模块负责以下硬件功能：

1. **系统运行灯**：区分 AP / STA 工作角色，长按恢复出厂配置期间提供快速闪烁提示。
2. **Wi-Fi 信号 RGB 灯**：根据 Wi-Fi 宽带 / 窄带模式、对端关联状态及 RSSI 显示颜色与异常灯语。
3. **蜂窝网络信号 RGB 灯**：根据蜂窝数据可用性、IPv6 互联网可达性与 RSRP 显示颜色与异常灯语。
4. **复位按键**：检测有效的长按并在松手后触发恢复出厂配置和系统重启请求。
5. **SBUS 通路控制**：通过 T113-i 的 PD16 输出 `MCU_MODEL` 控制信号。
6. **UART 接收使能**：通过低有效的 `UART_EN#` 信号控制接收使能。
7. **硬件版本识别**：读取 PB8、PB9 两位硬件版本编码。

LED 管理与复位按键复用一个 GPIO 后台线程，不为不同信号灯单独建立线程。

## 2. GPIO 硬件引脚分配

| 硬件功能 | SoC GPIO | 信号/说明 | 方向 | 有效电平或用途 |
|---|---|---|---|---|
| 系统运行灯 | PD1 | RUN LED | 输出 | 低电平点亮 |
| Wi-Fi 红色通道 | PD5 | Wi-Fi RSSI R | 输出 | 低电平点亮 |
| Wi-Fi 绿色通道 | PD6 | Wi-Fi RSSI G | 输出 | 低电平点亮 |
| Wi-Fi 蓝色通道 | PD9 | Wi-Fi RSSI B | 输出 | 低电平点亮 |
| 蜂窝红色通道 | PD15 | Cellular RSSI R | 输出 | 低电平点亮 |
| 蜂窝绿色通道 | PD14 | Cellular RSSI G | 输出 | 低电平点亮 |
| 蜂窝蓝色通道 | PD13 | Cellular RSSI B | 输出 | 低电平点亮 |
| 复位按键 | PD8 | KEY_1 | 输入 | 低电平表示按下 |
| SBUS 通路控制 | PD16 | MCU_MODEL | 输出 | 高电平表示逻辑使能 |
| UART 接收使能 | PD21 | UART_EN# | 输出 | 低电平使能 |
| 硬件版本位 1 | PB8 | HWID1 | 输入 | 版本编码 bit1 |
| 硬件版本位 2 | PB9 | HWID2 | 输入 | 版本编码 bit0 |

## 3. 系统运行灯（RUN，PD1）

### 3.1 正常工作灯语

| 设备角色 | 指示灯表现 | 亮灯时间 | 熄灯时间 | 完整周期 | 含义 |
|---|---|---:|---:|---:|---|
| AP | 长亮短灭、周期闪烁 | 1500 ms | 500 ms | 2000 ms | 当前 GPIO 管理器按 AP 角色运行 |
| STA | 短亮长灭、周期闪烁 | 300 ms | 700 ms | 1000 ms | 当前 GPIO 管理器按 STA 角色运行 |

说明：该灯语表示设备角色及 GPIO 管理器的运行状态，**不能单独证明 Wi-Fi 已关联、蜂窝网络可用或业务链路正常**。

### 3.2 恢复出厂配置时的提示灯语

| 条件 | RUN LED 表现 | 含义 |
|---|---|---|
| 按键按下、有效计时尚未达到 5 秒 | 沿用当前 AP/STA 正常灯语 | 尚未达到恢复触发时间 |
| 按键有效持续按下达到 5 秒，且未达到 10 秒 | 快速闪烁：亮 100 ms、灭 100 ms（5 Hz） | 进入允许在松手后恢复的时间窗口 |
| 按键持续按下达到 10 秒或更长 | 恢复 AP/STA 正常灯语；本次按压失效 | 防止超长按压误触发恢复 |
| 5 秒 ≤ 有效按下时间 < 10 秒，并完成松手消抖 | 退出快速闪烁；发起恢复出厂配置操作 | 松手后才触发恢复，不是在满 5 秒时立即恢复 |

> 按键持续时长从**消抖确认按下**开始计算，而非从第一次物理触点变化开始。LED 闪烁也受 100 ms 管理周期影响，实际切换边沿存在调度偏差。

### 3.3 启动、停止与关闭

- `init()` 初始化 RUN LED 的输出高电平，默认熄灭。
- `start()` 开始按 AP/STA 角色执行周期灯语。
- `stop()` 将 RUN LED 恢复为熄灭状态。
- `deinit()` 先停止，再释放 GPIO 资源。

## 4. Wi-Fi RSSI 信号灯（三色 RGB）

### 4.1 灯色含义

Wi-Fi 使用三个 GPIO 驱动一个 RGB 指示单元，颜色由 R/G/B 通道组合得到：

| 显示颜色 | 红 R | 绿 G | 蓝 B | 直观含义 |
|---|---|---|---|---|
| 白色 | 亮 | 亮 | 亮 | 最高信号等级 |
| 蓝色 | 灭 | 灭 | 亮 | 较强信号 |
| 绿色 | 灭 | 亮 | 灭 | 中等信号 |
| 黄色 | 亮 | 亮 | 灭 | 偏弱信号 |
| 红色常亮 | 亮 | 灭 | 灭 | 弱信号，**或**信号状态不满足有效性条件 |
| 红色快闪 | 100 ms 亮 / 100 ms 灭 | 灭 | 灭 | 对端未在线或 Wi-Fi 未关联等情况 |
| 熄灭 | 灭 | 灭 | 灭 | 配置中 Wi-Fi 已禁用，或 GPIO 停止后的安全状态 |

**注意**：红色常亮既可能表示 RSSI 落入最低档，也可能表示状态快照异常、RSSI 无效或过期；不能仅凭红色常亮断言无线信号很弱。

### 4.2 宽带模式的 RSSI 阈值

依据 `LINKG_WIFI_WORK_MODE_WIDE`：

| RSSI（dBm） | 显示颜色 | 信号档位 |
|---|---|---|
| RSSI ≥ -50 | 白色 | 最强档 |
| -60 ≤ RSSI < -50 | 蓝色 | 较强档 |
| -75 ≤ RSSI < -60 | 绿色 | 中等档 |
| -85 ≤ RSSI < -75 | 黄色 | 较弱档 |
| RSSI < -85 | 红色常亮 | 最弱档 |

以上颜色分档只适用于 RSSI 通过有效性校验的情况。

### 4.3 窄带模式的 RSSI 阈值

依据 `LINKG_WIFI_WORK_MODE_NARROW`：

| RSSI（dBm） | 显示颜色 | 信号档位 |
|---|---|---|
| RSSI ≥ -60 | 白色 | 最强档 |
| -75 ≤ RSSI < -60 | 蓝色 | 较强档 |
| -85 ≤ RSSI < -75 | 绿色 | 中等档 |
| -95 ≤ RSSI < -85 | 黄色 | 较弱档 |
| RSSI < -95 | 红色常亮 | 最弱档 |

宽带和窄带阈值不同，因此**相同 RSSI 在两种模式下可能显示不同颜色**。这不是 LED 异常。

### 4.4 Wi-Fi 状态判定与异常灯语

本节按照源码中的实际判断顺序整理，前面的判断不通过时，不再继续执行后面的 RSSI 颜色判定。

| 检查场景 | 灯语 | 实际判定依据 |
|---|---|---|
| Wi-Fi 在配置中关闭 | 熄灭 | `config.enabled == false` |
| 获取 Wi-Fi 配置失败 | 红色常亮 | `linkg_config_get_wifi()` 失败 |
| 获取 Wi-Fi 状态快照失败 | 红色快闪 | `linkg_wifi_get_status()` 失败 |
| 本地角色不匹配或本地状态快照过期 | 红色常亮 | 角色不一致、快照超过 5 秒或时间戳无效 |
| Wi-Fi 模式不是宽带或窄带 | 红色常亮 | `work_mode` 无效 |
| 获取 LinkG 直接对端数量失败 | 红色常亮 | `linkg_node_get_peer_count()` 失败 |
| LinkG 当前没有在线的直接对端 | 红色快闪 | `node_peer_count == 0` |
| STA 的唯一 AP 未关联 | 红色快闪 | STA Peer 无效或非 `CONNECTED` |
| STA 已关联但 RSSI 统计无效或过期 | 红色常亮 | `statistics_valid`、时间戳或 RSSI 校验失败 |
| AP 没有任何已关联 STA | 红色快闪 | `connected_count == 0` |
| AP 有已关联 STA，但没有任何有效 RSSI 样本 | 红色常亮 | 所有候选 Peer 都未通过有效性校验 |
| 成功取得有效 RSSI | 按宽带/窄带阈值显示五档颜色 | 见 4.2、4.3 |

### 4.5 AP 与 STA 的取值差异

**STA**：读取唯一 AP 对端的 Wi-Fi RSSI，按当前本地宽带/窄带工作模式映射为颜色。

**AP**：在当前 Wi-Fi 状态快照的已关联 STA 列表中，遍历所有具备有效 RSSI 的条目，选择数值最小（信号最差）的 RSSI 作为全局灯色依据。因此，多 STA 场景下，一台信号较差的 STA 可能决定 AP 端的 Wi-Fi 信号灯颜色。

**RSSI 有效性条件（源码实际行为）**：

- Peer 有效且处于 `CONNECTED` 状态；
- 驱动统计 `statistics_valid` 为真；
- 统计更新时间非零、未超前，且距当前时间不超过 **5000 ms**；
- RSSI 严格满足 **-127 < RSSI < 0 dBm**。

> 源码注释称“RSSI 允许为 0 dBm”，但实际条件为 `peer->rssi_dbm < 0`，**0 dBm 会被判为无效**。这是待开发确认的注释/实现不一致点；本手册以当前实际执行条件为准。

## 5. 蜂窝网络信号灯（三色 RGB）

### 5.1 灯色与 RSRP 对应关系

蜂窝指示灯使用服务小区的 **RSRP（Reference Signal Received Power）**，不是 Wi-Fi RSSI。颜色仍使用白、蓝、绿、黄、红五档。

| RSRP（dBm） | 显示颜色 | 信号档位 |
|---|---|---|
| RSRP ≥ -80 | 白色 | 最强档 |
| -90 ≤ RSRP < -80 | 蓝色 | 较强档 |
| -100 ≤ RSRP < -90 | 绿色 | 中等档 |
| -110 ≤ RSRP < -100 | 黄色 | 较弱档 |
| RSRP < -110 | 红色常亮 | 最弱档 |

上述颜色映射以 RSRP 数据有效为前提；实际源码还要求 `-150 ≤ RSRP < 0 dBm`。不在此范围、缺少 RSRP 或统计过期的场景均显示红色常亮，而不是判定为正常的弱信号。

### 5.2 蜂窝状态及异常灯语

| 条件 | 蜂窝 RGB 灯语 | 含义与判定依据 |
|---|---|---|
| 蜂窝功能在配置中关闭 | 熄灭 | `config.enabled == false` |
| 获取蜂窝配置失败 | 红色常亮 | `linkg_config_get_cellular()` 失败 |
| 蜂窝数据服务未就绪或可用性查询失败 | 红色快闪 | `LINKG_CELLULAR_AVAILABLE_DATA` 不可用 |
| 蜂窝 IPv6 互联网不可用或查询失败 | 红色快闪 | `LINKG_CELLULAR_AVAILABLE_IPV6` 不可用 |
| 获取蜂窝状态快照失败 | 红色常亮 | `linkg_cellular_get_status()` 失败 |
| 服务小区、RSRP 无效或数据过期 | 红色常亮 | `serving_cell.valid`、`rsrp_valid`、更新时间校验失败 |
| RSRP 数值非法 | 红色常亮 | `< -150` 或 `>= 0 dBm` |
| 数据和 IPv6 均可用，RSRP 有效 | 五档颜色常亮 | 按 5.1 节阈值显示 |

**关键规则**：蜂窝灯要显示正常的五档信号颜色，源码首先要求**蜂窝数据可用且 IPv6 互联网可用**。IPv4 单独不可达不会直接触发红闪；但这不意味着蜂窝数据或 IPv6 检测可以省略。

蜂窝统计快照最大有效年龄为 **15000 ms**。

### 5.3 如何理解灯色

- **红色快闪**：重点排查蜂窝数据是否就绪、IPv6 互联网是否可用。
- **红色常亮**：可能是有效 RSRP 很弱，也可能是状态查询失败、快照无效或过期。
- **白 / 蓝 / 绿 / 黄常亮**：说明前置可用性条件通过，并且有有效的 RSRP 样本。

> RSRP 只能反映一个无线信号维度，灯色不是吞吐量、往返延时、丢包率、4G/5G 制式或整体业务质量的直接测量值。

## 6. 所有指示灯的速查表（产品手册可直接摘用）

| 指示灯 | 表现 | 含义 |
|---|---|---|
| 系统 RUN | 长亮 1.5 秒、灭 0.5 秒循环 | AP 角色运行 |
| 系统 RUN | 亮 0.3 秒、灭 0.7 秒循环 | STA 角色运行 |
| 系统 RUN | 亮 0.1 秒、灭 0.1 秒循环 | 复位键长按达到 5 秒、未满 10 秒，松手可触发恢复 |
| Wi-Fi | 白色常亮 | 当前模式下 Wi-Fi RSSI 最高档 |
| Wi-Fi | 蓝色常亮 | 当前模式下 Wi-Fi RSSI 较强 |
| Wi-Fi | 绿色常亮 | 当前模式下 Wi-Fi RSSI 中等 |
| Wi-Fi | 黄色常亮 | 当前模式下 Wi-Fi RSSI 较弱 |
| Wi-Fi | 红色常亮 | Wi-Fi RSSI 最弱档，或状态/统计异常 |
| Wi-Fi | 红色快闪 | 没有在线直接对端、未关联，或 Wi-Fi 状态查询失败 |
| Wi-Fi | 熄灭 | Wi-Fi 功能关闭或 GPIO 已停止 |
| 蜂窝 | 白色常亮 | RSRP 最高档，且数据及 IPv6 可用 |
| 蜂窝 | 蓝色常亮 | RSRP 较强，且数据及 IPv6 可用 |
| 蜂窝 | 绿色常亮 | RSRP 中等，且数据及 IPv6 可用 |
| 蜂窝 | 黄色常亮 | RSRP 较弱，且数据及 IPv6 可用 |
| 蜂窝 | 红色常亮 | RSRP 最弱档，或信号统计/状态异常 |
| 蜂窝 | 红色快闪 | 数据服务未就绪或 IPv6 互联网不可用 |
| 蜂窝 | 熄灭 | 蜂窝功能关闭或 GPIO 已停止 |

## 7. 复位按键使用说明（KEY_1，PD8）

### 7.1 用户操作步骤

1. 在设备已正常启动后，按住复位按键。
2. 持续按住约 5 秒，观察 RUN 指示灯进入**快速闪烁**模式（源码只控制亮灭，RUN 灯颜色由硬件确定）。
3. **在达到 10 秒之前松开按键**。
4. 系统确认松手并消抖后，尝试恢复出厂配置。
5. 只有恢复成功，才通过生命周期管理请求系统重启。

### 7.2 保护规则

- 按键低电平有效。
- 连续 **2 个一致采样**后确认按下或松开，管理周期标称为 100 ms。
- 有效长按区间：**5 秒（含）至 10 秒（不含）**。
- 按住不足 5 秒松开，不执行恢复。
- 按住达到 10 秒，该次按压作废；继续按住、再松开都不会执行本次恢复。
- 如果 GPIO 读取失败，会清空此次按键计时，并且在检测到按键释放前禁止再次触发，避免误恢复。
- 有效松手后最多尝试 **2 次** `linkg_config_restore_factory()`；两次失败不会请求重启。
- 恢复成功后调用 `linkg_lifecycle_request_exit(LINKG_EXIT_ACTION_REBOOT_SYSTEM)` 请求重启。

> 产品手册中应写“恢复出厂**配置**”，不应未经验证扩大为“清除用户全部数据”“重新刷写固件”等行为；这些均非本 GPIO 源码所实现。

## 8. 其他 GPIO 功能与使用

### 8.1 SBUS 通路（PD16 / MCU_MODEL）

接口：`linkg_gpio_manager_set_sbus_enabled(bool enabled)`。

| 调用参数 | PD16 输出 | 逻辑动作 |
|---|---|---|
| `true` | 高电平 | 请求启用 SBUS 通路 |
| `false` | 低电平 | 请求关闭 SBUS 通路 |

初始化与停止时默认输出低电平，即请求关闭 SBUS 通路。该 GPIO 到实际 SBUS 开关之间存在 MCU/外围硬件控制链，最终线路动作须结合 MCU 固件确认。


初始化及停止时输出高电平，默认关闭接收使能。此接口仅明确控制 `UART_EN#` 所代表的接收通路，不应扩展解释为关闭整个 UART 控制器。

### 8.3 硬件版本识别（PB8 / PB9）

接口：

- `linkg_gpio_manager_get_hardware_bits(bool *hwid1, bool *hwid2)`：分别读取两个输入电平。
- `linkg_gpio_manager_get_hardware_id(uint8_t *hardware_id)`：将两个输入组合为 2 位编码。

计算公式：`hardware_id = (HWID1 << 1) | HWID2`。

| HWID1（PB8） | HWID2（PB9） | 返回硬件 ID |
|---:|---:|---:|
| 0 | 0 | 0 |
| 0 | 1 | 1 |
| 1 | 0 | 2 |
| 1 | 1 | 3 |

**注意**：这只是 0～3 的原始编码。每个编号对应哪一个 PCB/硬件版本，需要另有产品版本映射定义，当前函数没有定义具体板卡型号。

## 9. 软件调度与资源管理

| 项目 | 当前实现 |
|---|---|
| GPIO 管理线程 | 单一 `linkg_gpio` 线程 |
| 按键检测 / LED 输出周期 | 标称 100 ms |
| Wi-Fi / 蜂窝状态读取周期 | 标称每 2000 ms，启动时立即请求首次刷新 |
| Wi-Fi 统计允许最大年龄 | 5000 ms |
| 蜂窝统计允许最大年龄 | 15000 ms |
| LED 输出错误日志限频 | 相邻相关告警至少间隔 5000 ms（基于统一 `last_log_ms`） |
| 运行灯亮灭 | 依单调时间计算 AP、STA 或复位提示周期 |
| LED GPIO 输出 | 与上次成功值相同则跳过 Sysfs 写入；失败时下次重新尝试 |
| 外部状态读取 | Wi-Fi / Cellular 查询在不持有 GPIO 管理锁时执行 |
| 资源停止 | 停止线程后将指示灯置灭、SBUS 置低、UART_EN# 置高 |

### 9.1 GPIO 管理模块生命周期

```text
UNINITIALIZED
    |
    | linkg_gpio_manager_init()
    v
INITIALIZED  -- linkg_gpio_manager_start() --> RUNNING
    ^                                          |
    |                                          | linkg_gpio_manager_stop()
    +-------------------- STOPPING <-----------+
    |
    | linkg_gpio_manager_deinit()
    v
UNINITIALIZED
```

- `init()` 申请 GPIO 资源、初始化线程对象，不立即启动工作线程。
- `start()` 读取设备 AP/STA 角色，启动后台线程和灯语管理。
- `stop()` 停止线程，将控制输出恢复为安全电平。
- `deinit()` 停止后释放线程与 GPIO 句柄。
- `set_role()` 更新 RUN 灯角色和相位，同时暂时将 Wi-Fi 灯语缓存置为红色快闪，随后由状态刷新更新实际灯色。

> `stop()` 后再 `start()` 属于模块设计支持的生命周期路径；具体启动停止组合是否由系统上层正确编排，仍需结合调用方确认。

### 9.2 主要对外接口索引

| API | 用途 |
|---|---|
| `linkg_gpio_manager_init()` | 初始化 GPIO 资源和线程对象 |
| `linkg_gpio_manager_start()` | 启动 GPIO 管理 |
| `linkg_gpio_manager_stop()` | 停止并恢复安全输出 |
| `linkg_gpio_manager_deinit()` | 释放所有 GPIO 管理资源 |
| `linkg_gpio_manager_set_role(role)` | 更改 AP/STA 运行灯角色 |
| `linkg_gpio_manager_set_sbus_enabled(enabled)` | 设置 SBUS 通路逻辑使能 |
| `linkg_gpio_manager_get_hardware_bits(...)` | 读取 HWID1、HWID2 原始位 |
| `linkg_gpio_manager_get_hardware_id(...)` | 读取组合后的硬件版本编码 |

## 10. 源码与产品手册定版前需确认的事项

以下内容**不是已经确认的运行故障**，而是从当前源码整理出、需要在实机验证或产品文案审核时确认的边界：

1. **Wi-Fi RSSI 0 dBm 注释矛盾**：注释说允许 0 dBm，实际校验是 `rssi_dbm < 0`，必须确认产品期望后统一。
2. **SBUS“可选”表述**：上下文指针注释包含“可选”，但 `init()` 直接申请 PD16，失败会 `goto fail`，当前实现并不将该引脚作为可选资源处理。
3. **灯语异常含义有复用**：红色常亮和红色快闪各包含多种运行状态。面向客户的手册应写“可能原因”，不要写成唯一故障定位结论。
4. **Wi-Fi AP 的 RSSI 取值**：代码选择有效关联 STA 中的最小 RSSI；它并未在这一处逐一校验每个 Wi-Fi STA 是否与 Node 在线列表逐项对应。文案应以“已关联且统计有效的 STA”表述。
5. **蜂窝 IPv6 依赖**：正常五档显示要求蜂窝数据和 IPv6 均可用；需确认该规则与产品部署环境一致，特别是仅 IPv4 可用的网络。
6. **面板灯颜色与标识**：RUN LED 的硬件颜色未由此源码定义，代码仅负责亮灭；复位操作章节不要擅自指定 RUN LED 颜色。
7. **恢复出厂配置范围**：请以 `linkg_config_restore_factory()` 的具体实现和产品需求确定恢复项目，本模块只负责触发。
8. **灯语时序**：100 ms GPIO 轮询和 2 秒状态刷新是软件标称周期；受 Linux 调度、Sysfs IO 和状态查询耗时影响，应实测 LED 边沿与按键长按边界。
9. **硬件版本编号**：HWID=0、1、2、3 只表示二进制编码，不自动等于 PCB Rev.A/B/C/D。
10. **GPIO 和 MCU 关系**：PD16 是 `MCU_MODEL`，实际 SBUS 切换仍需核对 MCU 固件及原理图，避免产品文档混淆直控和间控。

## 11. 源码追溯

- [`service/gpio/linkg_gpio_manager.c`](https://github.com/wnd443724421-cell/linkg/blob/main/service/gpio/linkg_gpio_manager.c)：LED 状态机、阈值、按键、线程及生命周期。
- [`include/linkg/service/gpio/linkg_gpio_manager.h`](https://github.com/wnd443724421-cell/linkg/blob/main/include/linkg/service/gpio/linkg_gpio_manager.h)：对外接口。
- [`include/linkg/platform/linkg_system_resources.h`](https://github.com/wnd443724421-cell/linkg/blob/main/include/linkg/platform/linkg_system_resources.h)：固定 GPIO 引脚映射。

---

**后续用于产品手册的建议**：正文优先保留第 2、3、4、5、6、7、8 节，配合实物照片标注位置；第 9、10、11 节作为内部开发、测试及售后说明附录保留。

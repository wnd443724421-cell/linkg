# LinkG MCU V2.0 — 编译与架构审核手册

> 目标：STM32L031G6U6（Cortex-M0+、32 KiB Flash、8 KiB SRAM）。整理日期：2026-10-14。  
> 本文依据仓库 `linkg-mcu/` 中的 **Makefile、Core/BSP/Service 与 tests** 编写，供后续代码审查使用。完整交付说明见 [README.md](README.md)，历史验证详情见 [validation/REPORT.md](validation/REPORT.md)。  
> 本文件描述**现有代码实际行为**，不是要求改变架构的开发任务书。

## 1. Ubuntu GCC 编译速查

当前 Ubuntu 环境中已准备好 ARM GCC；下面路径来自当前工作区，如果工程或工具链位置发生变化，请替换。

~~~bash
cd /home/dawn/project/linkg_v3/linkg-mcu

MCU_CROSS=/home/dawn/project/.codex-mcu-backups/20261014-v2/toolchain/gcc-arm-none-eabi-8-2019-q3-update/bin/arm-none-eabi-

# 核对编译器
"$MCU_CROSS"gcc --version

# 增量构建，生成 ELF / HEX / BIN
make -j4 CROSS_COMPILE="$MCU_CROSS"
~~~

`MCU_CROSS` 是**编译工具名称前缀**，末尾的 `arm-none-eabi-` 连字符必须保留。变量仅在当前终端会话有效，不需要修改系统 `PATH`。

~~~bash
# 强制全部重新编译
make -B -j4 CROSS_COMPILE="$MCU_CROSS"

# 保存完整编译日志（在 Bash 终端）
mkdir -p validation
set -o pipefail
make -B -j4 CROSS_COMPILE="$MCU_CROSS" 2>&1 | tee validation/build-manual.log

# 主机逻辑测试 + 源码格式/工程文件检查
make test check

# 验证实际 ELF/HEX/BIN 的向量表、架构、内存及符号
python3 tests/check_firmware.py "$MCU_CROSS"

# 需要时清理 GCC 构建产物和主机测试产物
make clean
~~~

`make test` 需要 Ubuntu 宿主 `gcc` 与 AddressSanitizer/UndefinedBehaviorSanitizer；`make check` 需要 Python 3。`check_firmware.py` 应在 ARM GCC 构建生成固件之后运行。`set -o pipefail` 可避免 `tee` 隐藏编译失败的退出状态。

### 编译输出

| 文件 | 用途 |
|---|---|
| `build/gcc/linkg_mcu_v2.elf` | 带符号的 ELF，供调试、反汇编和检查 |
| `build/gcc/linkg_mcu_v2.hex` | 含目标地址的 Intel HEX 固件 |
| `build/gcc/linkg_mcu_v2.bin` | 原始二进制固件，烧录 Flash 起点 `0x08000000` |
| `build/gcc/linkg_mcu_v2.map` | 链接映射、函数/数据段布局 |

构建入口为 [Makefile](Makefile)，GNU 启动文件为 [GCC/startup_stm32l031xx.s](GCC/startup_stm32l031xx.s)，链接脚本为 [GCC/stm32l031g6.ld](GCC/stm32l031g6.ld)。链接布局：Flash 32 KiB、RAM 8 KiB、独立保留 1024 B 栈、无动态堆。

**验证状态：** [validation/REPORT.md](validation/REPORT.md) 记录了一次 GCC 8.3.1 实际构建成功，镜像 Flash 为 **7,956 B**、静态 RAM 为 **504 B**，另预留 **1,024 B** 栈。这是已有记录，**不是本文件编写时重新编译测得的结果**。链接摘要中的 RAM 100% 是因为 `.stack` 固定在内存顶端，实际占用须查看固件检查脚本输出。

本节命令**仅编译与检查，不会烧录 MCU**。Keil 工程 [MDK-ARM/soft.uvprojx](MDK-ARM/soft.uvprojx) 已保留，但现有记录只证明其 XML/路径经过检查，**尚无 Keil 编译通过的证据**。

## 2. 模块职责与源码目录

~~~text
linkg-mcu/
├── Core/
│   ├── Inc/                           主程序、中断及HAL配置声明
│   └── Src/
│       ├── main.c                    上电入口、系统时钟、WFI主循环
│       ├── stm32l0xx_hal_msp.c      HAL公共 MSP 支持
│       ├── stm32l0xx_it.c           SysTick、UART和异常中断入口
│       └── system_stm32l0xx.c       CMSIS系统初始化
├── Drivers/                          STM32官方 CMSIS / HAL 驱动
├── bsp/
│   ├── bsp_gpio.c/.h                PA1模式输入、PB1开关控制
│   └── bsp_uart.c/.h                UART时钟、引脚、收发寄存器和IRQ
├── service/
│   └── sbus_bridge.c/.h             通路状态机、双FIFO、统计、IWDG
├── GCC/                              GCC启动汇编及链接脚本
├── MDK-ARM/                          Keil兼容工程
├── tests/                            主机逻辑、源码及镜像校验
├── validation/                       历史构建与检查日志
├── Makefile
├── README.md
└── ARCHITECTURE.md
~~~

~~~mermaid
flowchart TD
    A["Core/main.c<br/>启动、时钟、主循环"] --> B["service/sbus_bridge.c<br/>模式状态机、FIFO、IWDG"]
    I["Core/stm32l0xx_it.c<br/>IRQ入口"] --> B
    B --> G["bsp/bsp_gpio.c<br/>PA1、PB1"]
    B --> U["bsp/bsp_uart.c<br/>LPUART1、USART2"]
    G --> H["STM32 HAL / CMSIS"]
    U --> H
    A --> H
~~~

原则：**MCU 只负责通路切换、UART 电气极性转换、原始字节转发和基本错误恢复**。它不检查 SBUS 25 字节长度、不识别帧头帧尾、不修改通道、不生成替代帧、不按 7.6 ms 定时。协议帧处理、网络抖动缓冲与最终 SBUS 输出调度属于 Linux LinkG。

## 3. 硬件引脚与波特率：禁止配置反

参考 LinkG V2.0 原理图第 6 页及 [bsp_uart.c](bsp/bsp_uart.c)：

| 角色 | STM32硬件 | 管脚 | 配置 | 极性 |
|---|---|---|---|---|
| 外部 SBUS 接口 | **LPUART1** | **PA2 TX / PA3 RX** | **100000, 8E2** | **TX/RX 均反相** |
| T113 Host UART4 | **USART2** | **PA9 TX / PA10 RX** | **100000, 8E2** | **TX/RX 均不反相** |
| 模式请求 MCU_MODEL | GPIOA | PA1 | 数字输入，运行时下拉 | 高电平表示桥接 |
| U16 CTL | GPIOB | PB1 | 推挽输出 | 高电平直通、低电平隔离 |

两侧不启用 RTS/CTS 硬件流控。STM32 HAL 通过 `UART_WORDLENGTH_9B + UART_PARITY_EVEN` 实现实际的 8 数据位 + 1 偶校验位，停止位设为 2。

两侧使用 HSI16 经 PLL 得到 32 MHz，USART2/LPUART1 时钟来自 PCLK1。标称分频无量化误差，但实际时钟仍有 HSI 频率误差/温漂。100000/8E2 一字节占线时间约 **120 μs**。

原理图上部分 MCU 网络名称较旧，识别 Host/SBUS 两侧应以物理连线、STM32 引脚和当前 BSP 为准，而不是仅看网名。

**上电风险：** U16 的 CTL 上拉位置 R118 标为 NC，MCU 复位到固件配置 PB1 输出之间无法由软件保证 CTL 电平；必须实测上电、掉电及复位过程，不得将 GPIO 初始化后的安全电平等同于整个上电期间都安全。

## 4. 系统启动及主循环流程

~~~mermaid
flowchart TD
    R["Reset_Handler<br/>初始化数据段、BSS、调用SystemInit"] --> M["main()"]
    M --> H["HAL_Init<br/>启动SysTick时基"]
    H --> G["bsp_gpio_init<br/>PB1预置低电平断开U16<br/>PA1输入下拉"]
    G --> C["system_clock_configure<br/>HSI/PLL 32MHz<br/>两路UART时钟PCLK1"]
    C --> I["sbus_bridge_init<br/>停UART、清状态<br/>初始化IWDG并读PA1"]
    I --> D{"初始化成功？"}
    D -->|否| E["Error_Handler<br/>停UART并隔离"]
    D -->|是| P["sbus_bridge_process<br/>检查模式、故障、喂狗"]
    P --> W["__WFI 等待中断"]
    W -->|SysTick或UART IRQ| P
~~~

代码顺序：`HAL_Init() → bsp_gpio_init() → system_clock_configure() → sbus_bridge_init() → while(1){sbus_bridge_process(); __WFI();}`。

- SysTick IRQ 调用 `HAL_IncTick()` 提供毫秒时基；默认持续唤醒主循环。这里没有 MCU 的 7.6 ms SBUS 定时输出。
- IWDG 为 LSI/256、Reload 249、窗口禁用；标称 37 kHz 时约 **1.73 秒**；主循环每 **100 ms** 刷新。实际复位时限需实板测量。
- `Error_Handler()` 关闭中断、停止 UART、隔离 U16。早期 IWDG 尚未启动时发生的错误不保证自动复位。

## 5. 模式切换状态机

状态定义见 [sbus_bridge.h](service/sbus_bridge.h)：

| 状态 | 含义 |
|---|---|
| `ISOLATED` | UART未工作，U16断开、数据引脚释放 |
| `BYPASS` | U16直接连通外部UART和T113；MCU不驱动四根数据线 |
| `RUNNING` | U16断开、双UART正常工作、RX和错误中断启用 |
| `FAULT` | UART初始化/运行故障，维持隔离并计划重试 |

~~~mermaid
flowchart TD
    I["ISOLATED"] --> M{"PA1 MCU_MODEL = 高？"}
    M -->|否| B["BYPASS<br/>停双UART/四线高阻<br/>PB1置高，U16直通"]
    M -->|是| S["PB1置低，断开U16<br/>bsp_uart_start"]
    S --> D{"LPUART1和USART2<br/>初始化是否成功？"}
    D -->|否| F["FAULT<br/>保持隔离，记录失败"]
    D -->|是| Q{"再次读取PA1"}
    Q -->|高| R["RUNNING<br/>发布状态并开启UART RX IRQ"]
    Q -->|低| B
    R -->|PA1变低| B
    R -->|UART异常请求恢复| F
    F -->|PA1变低| B
    F -->|PA1仍高，达到重试时间| S
    B -->|PA1变高| S
~~~

实际控制顺序：
- **进入桥接：** PB1 低（U16断开）→ 重置/初始化两路 UART → 配置 PA2/PA3/PA9/PA10 为 UART AF → 发布 RUNNING → 启用 RXNE/PE/EIE IRQ。
- **进入直通：** 停止 UART IRQ 和 UART 外设 → 四根 UART 数据脚恢复为模拟输入高阻 → PB1 高，闭合 U16。先释放 MCU 输出，再允许直通，避免驱动冲突。
- **初始化失败：** 保持 PB1 低与数据脚高阻；`SBUS_BRIDGE_RETRY_MS = 100U` 控制 `FAULT` 期间的重试间隔。
- **运行异常：** ISR 设置 `recover_requested`，主循环清理旧队列、进入 FAULT 并按时重启。切换时在途 UART 字节可能丢失或截断，**不保证热切换无损、不重放旧字节**。

## 6. 双向 UART 中断收发流程

每个方向拥有独立的 **32 字节环形 FIFO**。队列按**源 UART**索引，目标端由 `sbus_bridge_peer()` 决定。

~~~mermaid
flowchart LR
    EX["外部SBUS<br/>反相信号"] --> LR["LPUART1 RX<br/>PA3"]
    LR --> F1["FIFO[SBUS]<br/>32字节"]
    F1 --> HT["USART2 TX<br/>PA9"]
    HT --> HOST["T113 Host"]

    HOST --> HR["USART2 RX<br/>PA10"]
    HR --> F2["FIFO[HOST]<br/>32字节"]
    F2 --> LT["LPUART1 TX<br/>PA2"]
    LT --> EX
~~~

~~~mermaid
flowchart TD
    IRQ["USART2_IRQHandler 或 LPUART1_IRQHandler"] --> RX["sbus_bridge_uart_irq(source)"]
    RX --> READ["bsp_uart_receive<br/>检查硬件PE/FE/NE/ORE<br/>最多从RDR取1字节"]
    READ --> V{"有有效字节？"}
    V -->|是| K["先尝试 kick 源方向队列<br/>释放可能的目标TDR空位"]
    K --> FULL{"FIFO仍满？"}
    FULL -->|是| DROP["丢弃新字节<br/>overflow_bytes++"]
    FULL -->|否| ENQ["入队并更新queued_bytes<br/>和high_water_bytes"]
    V -->|无字节或坏字节| ERR["只更新错误统计<br/>不构造任何字节"]
    ENQ --> TX["kick 两个方向队列"]
    DROP --> TX
    ERR --> TX
    TX --> TRY{"目标UART TXE就绪？"}
    TRY -->|是| SENT["写TDR后推进tail<br/>tx_bytes++"]
    TRY -->|否| WAIT["保留队首<br/>等待TXE中断"]
~~~

- **中断入口：** [stm32l0xx_it.c](Core/Src/stm32l0xx_it.c) 的 `LPUART1_IRQHandler` / `USART2_IRQHandler` 调用 `sbus_bridge_uart_irq(port)`；不调用 HAL UART 收发回调。
- **成功条件：** 目标 TDR 接受字节后才推进队列；TXE 暂时忙时不丢待发队首；队列空时关闭 TXE IRQ。
- **数据完整性：** 不按 25 字节判断。但若硬件标记当前 RDR 有 **PE/FE/NE 错误**，当前实现会丢弃该字节并统计 `bad_rx_bytes`；这是硬件错误过滤，不是 SBUS 帧校验。ORE 只记录溢出事件，不能据此精确推算丢失的字节数。
- **FIFO 满：** 先尝试发送旧队首，若仍满则丢弃**当前新字节**；不会覆盖已排队数据。
- **模式退出/重启：** 丢弃未提交软件队列数据，记录 `discarded_bytes`；对 `TC` 未完成只增加 `tx_abort_events`，不会猜测精确丢字节数。
- **IRQ 并发：** 两个 UART IRQ 同为优先级 1，互不抢占；主循环更新状态或复制统计时短暂屏蔽中断（`PRIMASK`）。
- **调试指标：** `tx_bytes` 仅表示写入 TDR，不代表串口线上完整发送，更不代表远端确认接收。

## 7. Debug 与观测方式

目前 MCU 没有占用业务 UART 的日志输出。通过 `sbus_bridge_get_stats()` 或 SWD 读取 `sbus_bridge_stats_t`，配合 Linux 侧 UART Debug 和逻辑分析仪排查。

| 字段 | 含义 |
|---|---|
| `state` / `mode_changes` / `start_failures` / `last_error` | 桥接运行状态、切换次数、启动失败、最近错误 |
| `direction[BSP_UART_SBUS]` | 外部 SBUS → T113 的统计 |
| `direction[BSP_UART_HOST]` | T113 → 外部 SBUS 的统计 |
| `rx_bytes` / `tx_bytes` | 读 RDR 的字节量 / 提交 TDR 的字节量 |
| `queued_bytes` / `high_water_bytes` | FIFO 当前深度 / 历史高水位 |
| `overflow_bytes` / `bad_rx_bytes` | 队列溢出丢字节 / UART 硬件标错字节 |
| `parity_errors` / `frame_errors` / `noise_errors` / `overrun_errors` | 物理 UART 错误事件 |
| `discarded_bytes` / `tx_abort_events` / `tx_failures` | 停止丢弃、未完成发送事件和非忙写入失败 |

## 8. 逐文件审核路线与待确认边界

| 顺序 | 目标文件 | 审核重点 |
|---|---|---|
| 1 | [Core/Src/main.c](Core/Src/main.c)、[Core/Src/stm32l0xx_it.c](Core/Src/stm32l0xx_it.c) | 启动时钟、安全态、SysTick、WFI 和 IRQ 入口 |
| 2 | [bsp/bsp_gpio.c](bsp/bsp_gpio.c) | PA1/PB1 电平、U16 直通/隔离、安全时序 |
| 3 | [bsp/bsp_uart.c](bsp/bsp_uart.c) | 100000/8E2、反相侧、GPIO AF、HAL 生命周期与寄存器收发 |
| 4 | [service/sbus_bridge.c](service/sbus_bridge.c) | FIFO 索引与所有权、ISR、模式状态机、错误恢复、看门狗 |
| 5 | [Makefile](Makefile)、[GCC/](GCC/)、[tests/](tests/) | 构建一致性、向量表、栈/Flash、测试覆盖范围 |

**实板待验证：** U16 CTL 在 MCU 复位期间的电平（R118 NC）；TX/RX 真实极性及空闲电平；双向 100 kbaud 长稳吞吐、ISR 延迟和 FIFO 水位；频繁切换时的在途字节/线路竞争；PE/FE/NE/ORE 故障恢复；IWDG 复位时间和实际栈水位。

**审核方式：** 先完整理解和记录问题，再集中修正。**已通过 ARM GCC 编译或主机 mock 测试，不等于已经实板验证。**

---

代码风格必须遵守仓库根目录 [LinkG_C_Code_Style_Guide.md](../LinkG_C_Code_Style_Guide.md)。

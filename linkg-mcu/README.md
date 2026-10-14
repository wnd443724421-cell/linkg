# LinkG MCU V2.0

目标芯片为 STM32L031G6U6，32 MHz Cortex-M0+、32 KiB Flash、8 KiB SRAM。
本目录是独立的裸机 C 固件工程，业务只包括通路切换、双向透明字节流桥接、UART 错误恢复和 IWDG。
SBUS 帧格式、通道解码、校验、补帧、重同步和发送周期全部由 T113 Linux 负责。

## 实际交付状态

2026-10-14 在 `/home/dawn/project/linkg_v3/linkg-mcu` 完成 ARM GCC 8.3.1 编译、链接及镜像生成。
GNU 启动文件、链接脚本、48 项向量表、数据段初始化、BSS 清零、栈边界和目标架构均做了检查。
Keil 工程通过 XML、源文件和头文件路径检查，但未执行 Keil 编译，当前机器没有可调用的 Keil/ARMCC。
未烧录设备，UART 电气行为、真实延迟、误码恢复和看门狗复位仍须实板验证。

运行结果见 `validation/gcc-build.log`、`validation/checks.log`、`validation/firmware.log`。
`validation/REPORT.md` 记录完整命令、验证边界与文件变更清单。

## 模块和目录

```text
linkg-mcu/
├── Core/
│   ├── Inc/{main.h,stm32l0xx_hal_conf.h,stm32l0xx_it.h}
│   └── Src/{main.c,stm32l0xx_hal_msp.c,stm32l0xx_it.c,system_stm32l0xx.c}
├── Drivers/                         # 保留仓库中的官方 CMSIS/HAL，未修改
├── bsp/{bsp_gpio.c,bsp_gpio.h,bsp_uart.c,bsp_uart.h}
├── service/{sbus_bridge.c,sbus_bridge.h}
├── MDK-ARM/{soft.uvprojx,soft.uvoptx,startup_stm32l031xx.s}
├── GCC/{startup_stm32l031xx.s,stm32l031g6.ld}
├── tests/{test_bridge.c,check_project.py,check_firmware.py,mocks/stm32l0xx_hal.h}
├── validation/                      # 构建、检查、完整性和交付记录
├── Makefile
├── .gitignore
└── README.md
```

Core 只负责系统时钟、HAL、异常/中断入口及应用启动。UART 的 MSP 回调放在 `bsp_uart.c`，保证 UART 的时钟、引脚、配置和停止属于同一模块。
BSP GPIO 只管理 PA1/PB1，UART BSP 管理四根数据引脚。Bridge 持有两个 FIFO、模式状态、累计统计及看门狗生命周期。
无 RTOS、动态内存、旧电源/充电/电池/蜂鸣器/LED/按键业务。

## 硬件配置

依据用户提供的 `linkg-v2.pdf` 第 6 页、旧工程 `mcu_soft/Core/Src/stm32l0xx_hal_msp.c` 和 `main.c` 的 `set_ctl()`。

| 功能 | 外设/引脚 | 复用 | 配置 |
|---|---|---|---|
| 外部 SBUS TX | LPUART1 / PA2，U15 引脚 8 | AF6 | TXINV=1 |
| 外部 SBUS RX | LPUART1 / PA3，U15 引脚 9 | AF6 | RXINV=1，接收脚弱下拉匹配反相空闲低电平 |
| T113 Host TX | USART2 / PA9，U15 引脚 19 | AF4 | TXINV=0 |
| T113 Host RX | USART2 / PA10，U15 引脚 20 | AF4 | RXINV=0，接收脚弱上拉匹配正常空闲高电平 |
| 模式请求 | PA1，U15 引脚 7 | GPIO 输入 | 高=桥接；低=直通；MCU 初始化后启用内部弱下拉 |
| U16 开关 | PB1，U15 引脚 15 | GPIO 推挽输出 | 低=断开；高=导通 |

两侧都是 **100000 baud、8 数据位、偶校验、2 停止位、无硬件流控**。
HAL 配置使用 `UART_WORDLENGTH_9B + UART_PARITY_EVEN`，9 位包含校验位，实际有效数据为 8 位。
系统使用 HSI16 × 4 ÷ 2 = 32 MHz，AHB/APB 均不分频，两个 UART 使用同一个 PCLK1。
标称 USART2 BRR=320；LPUART1 BRR=81920，分频量化误差为 0，实际误差仍受 HSI 精度影响。

原理图中 PA2/PA3 对应的网名是 `MCU_UART_TX/RX`，PA9/PA10 对应的网名是 `MCU_SBUS_TX/MCU_SUBS_RX`；名字与本次分工相反。
沿实际连线，前者接外部 UART0/SBUS 通路，后者接 T113；不能只按网名判断两侧。
图中 `PA11/PA9`、`PA12/PA10` 为原理图库标注；本芯片 UFQFPN28 的 19/20 脚按 ST 数据手册配置为 PA9/PA10，不添加 STM32G0 的引脚重映射操作。

参考原厂资料：[STM32L031x4/x6 数据手册](https://www.st.com/resource/en/datasheet/stm32l031c6.pdf)、[SN74LVC2G66](https://www.ti.com/product/SN74LVC2G66)。
U16 的控制输入高电平导通，与旧版 `set_ctl(0)` 输出高、`set_ctl(1)` 输出低一致。

## 上电与模式切换

**R118 标注 NC，不能假定 CTL 有外部上拉。** MCU 复位、掉电、烧录、IWDG 复位及执行早期启动代码期间，PB1 尚未由固件驱动，U16 状态不保证。
软件只能在进入 `main()` 并初始化 GPIO 后约束电平，不能消除上电前这段不确定窗口。若产品要求该窗口也具有确定通路，须由硬件提供合适的 CTL 偏置/门控，并验证 MCU/T113 供电时序。

启动流程：

1. `HAL_Init()` 后立即开启 GPIOA/B 时钟，将 PB1 输出锁存预置为低，再配置 PB1 为推挽输出，先隔离 U16。
2. PA1 配置为输入弱下拉；四根 UART 数据脚在启动桥接前保持高阻。
3. 配置 32 MHz 时钟，初始化 IWDG，根据 PA1 选择直通或桥接。

进入桥接（PA1 高）：先拉低 CTL，停止/复位 UART，初始化两路 UART 的波特率、字长、校验和反相；两路均成功后连接 AF 引脚，再发布 RUNNING 状态、开启 RXNE/PE/EIE 中断。
UART 初始化期间中断不被全局屏蔽，HAL 超时所需的 SysTick 可以运行。若期间 PA1 已变低，完成初始化后重新读取并立即释放 UART，再导通 U16。

退出桥接（PA1 低）：主循环短暂屏蔽中断，记录软件队列丢弃和 TC 未完成事件，关闭 UART IRQ、UE/TE/RE，将 PA2/PA3/PA9/PA10 配置为模拟输入无上下拉高阻，停掉并复位外设，最后 CTL 拉高。
退出不等待发送排空，因此可能截断在途字节；不会在下一次进入桥接时重复旧字节。切换必须由 Linux 在流量静止时协调，切换边界不承诺无损。

初始化失败保持 CTL 低、数据脚高阻，每 100 ms 至多开始一次重试。PA1 变低可退出故障隔离并进入直通。
正常稳态模式检测由主循环完成；空闲时 SysTick 每 1 ms 唤醒，数据 IRQ 也会唤醒主循环。1 ms 是软件调度设计值，不是实板测得的最坏切换时间；初始化/异常期间另受 HAL 超时影响。

## 双向字节流与错误恢复

```text
LPUART1 RXNE → 读取 RDR → FIFO[SBUS源] → USART2 TDR/TXE
USART2  RXNE → 读取 RDR → FIFO[HOST源] → LPUART1 TDR/TXE
```

两个方向各有 32 字节有界环形 FIFO。收到有效字节后立即尝试写目标 TDR；若 TXE 未就绪，保留队首并开启目标 TXE 中断。
只在 TDR 成功接受一个字节后移动 tail、减少长度、增加 `tx_bytes`。因此任意发送进度都对应明确的队列位置，不使用异步整帧缓冲，也没有重发已提交前缀的问题。
TXE 队列空后立即关闭；RXNE 接收持续使能，不存在 HAL 单字节接收回调重新挂接的空窗。
两路 UART IRQ 均为优先级 1，互不抢占；SysTick 为优先级 3。主循环切换/统计快照用短 PRIMASK 临界区保护。
中断中没有等待、日志、HAL 阻塞发送、协议识别或时间调度。HAL 句柄仅用于 Init/DeInit，应用从不修改 `gState` / `RxState`，也不与 HAL 数据收发状态机混用。

100000/8E2 的字符占线时间为 120 μs。空队列时首字节不等待凑帧，也没有 7.6 ms 定时输出。
目标起始位可在源字节完整接收并经过 ISR 后发出；目标完整字节还需要约 120 μs 占线时间。实际 ISR 延迟/抖动尚未测量。
32 字节满队列对应约 3.84 ms 的待发占线时间，这是拥塞上限，不是主动缓冲或目标延迟。
连续满带宽流量存在收发时钟偏差时，有限 FIFO 不能保证永久无丢失，必须通过实板长期测试确认使用条件。

| 条件 | 动作与统计 |
|---|---|
| TXE 未就绪 | 保留队首，等待 TXE；不重复提交 |
| FIFO 满 | 先尝试腾出目标 TDR 空位；仍满时丢弃新字节，增加 `overflow_bytes`，旧字节顺序不变 |
| PE / FE / NE | BSP 清除 ICR 标志，读出 RDR；Bridge 丢弃硬件已标错字节，记录各错误及 `bad_rx_bytes` |
| ORE | 清除 ORE，继续接收；若当前 RDR 没有 PE/FE/NE，仍转发它；`overrun_errors` 记录事件，不虚构丢失字节数 |
| 错误标志但没有 RXNE | 只记录错误，不生成字节 |
| 非忙 TX 错误 | 保留队首直到主循环停止；记录失败，清理并限频重启外设 |
| 退出/恢复重启 | 精确记录软件队列丢弃；TC 未完成只记 `tx_abort_events`，不把可能在移位寄存器/TDR 内的字节数猜成精确丢失数 |

`direction[BSP_UART_SBUS]` 表示外部 → T113；`direction[BSP_UART_HOST]` 表示 T113 → 外部。
`rx_bytes` 包含读出的硬件标错字节；`tx_bytes` 只表示 TDR 提交数，不能当成远端实际收到或线上完整发送数。
统计均为复位后累计，32 位无符号计数自然回绕；SWD 可查看 `g_bridge.stats`，代码接口为 `sbus_bridge_get_stats()`。不占用业务 UART 输出日志。

## 看门狗

IWDG 使用 LSI、256 分频、Reload=249、窗口关闭，在标称 37 kHz 下约 1.73 s 超时。
主循环每 100 ms 喂狗，中断不喂狗，主循环卡住或中断风暴会停止喂狗。
LSI 有器件/温度偏差，实际复位周期必须测量。故障处理会释放数据脚并隔离 U16，已启动的 IWDG 随后复位。
看门狗在系统时钟配置之后启动，因此尚未启动 IWDG 的早期时钟配置故障不保证自动复位。

## 编译与检查

使用已有 ARM GCC 时：

```sh
cd /home/dawn/project/linkg_v3/linkg-mcu
make -j4 CROSS_COMPILE=/absolute/path/to/arm-none-eabi-
make test check
python3 tests/check_firmware.py /absolute/path/to/arm-none-eabi-
```

`CROSS_COMPILE` 是带 `arm-none-eabi-` 的前缀，不是编译器目录本身。
GCC 使用自包含启动文件与链接脚本，Flash 起始 `0x08000000`，栈顶 `0x20002000`，栈预留 1024 字节，堆为 0。
GNU 启动文件从仓库 CMSIS 模板迁移，保留向量表、SystemInit、data/BSS 初始化；该纯 C 工程无构造函数，去掉了 C++/libc 构造器调用。
链接器断言检查栈与 BSS 不重叠、向量表正好 192 字节。使用与 Cortex-M0+ 匹配的 Thumb 裸机 C 库/libgcc。
所有应用文件使用 `-Wall -Wextra -Werror`；官方 HAL 仅额外关闭 `unused-parameter`，不改其源码。
HAL DMA 仅保留为 HAL UART 公共声明和链接依赖，不配置 DMA、不启用 DMA IRQ；ADC、TIM、I2C、SPI、EXTI 业务依赖已移除。

编译产物：`build/gcc/linkg_mcu_v2.{elf,hex,bin,map}`，该目录由 Git 忽略。
`.stack` 被放在 SRAM 顶部，因此 GNU 链接器摘要显示 RAM 地址跨度 100%；实际静态数据 504 B，加预留栈 1024 B，中间仍有 6664 B 间隔。以 `validation/firmware.log` 的段/符号结果为准。

Keil：打开 `MDK-ARM/soft.uvprojx`，目标 `soft`，芯片 `STM32L031G6Ux`（对应 G6U6），ARM Compiler 5/C99/microlib。
预计命令为 `UV4.exe -b soft.uvprojx -t soft -o build.log`，此命令本次未执行。
新 Keil 输出写入 `MDK-ARM/build/linkg_mcu_v2.*`。既有 `MDK-ARM/soft/` 是旧固件缓存，保留用于原始工程追溯，不是本次输出，不参与构建，也不放入独立交付压缩包。
`soft.uvoptx` 已清除旧业务分组、断点、观察项；调试器自动下载/运行关闭。工程没有烧录目标或构建后烧录命令。

## 实板验收

1. 用示波器/逻辑分析仪验证两侧 100000/8E2、PA2/PA3 反相、PA9/PA10 正常极性，以及空闲电平和实际字符时间。
2. PA1 高低切换时同时观测 PB1 和两组 TX，确认先断后通、高阻释放、无两条路径同时驱动，测量切换延迟。
3. 覆盖上电、复位、MCU/T113 分别掉电和 IWDG 复位，特别核查 R118 NC 引起的 CTL 浮空窗口。
4. 单向和双向发送随机流、全字节值、短突发和长连续流，逐字节比对，测首字节延迟与抖动，观察 FIFO 高水位和时钟偏差累积。
5. 注入 PE/FE/NE/ORE、长 BREAK 和 FIFO 压力，确认错误计数增加、后续字节恢复、Linux 能重新同步；测试在途模式切换的已记录丢失。
6. 阻塞主循环和制造中断负载，确认 IWDG 真实复位时限；检查电压/温度下 HSI 和 LSI 偏差；用栈水位验证 1 KiB 栈余量。

以上实板项目尚未执行，不因编译和主机测试通过而标记为成功。

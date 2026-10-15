# LinkG MCU V2.0 交付记录

日期：2026-10-14（Asia/Shanghai）。工作目录：`/home/dawn/project/linkg_v3/linkg-mcu`。
仓库基线为 `09b67b4`，变更保存在 Ubuntu 工作区，未 commit、未 push、未烧录。

## 验证结果

| 项目 | 结果与边界 |
|---|---|
| ARM 裸机完整编译、链接、ELF/HEX/BIN | 实际通过，GCC ARM Embedded 8.3.1，Cortex-M0+ / Thumb / soft-float |
| 独立工程包重建 | 将交付包解压到独立临时目录，重新编译成功；BIN 与工作区产物逐字节相同 |
| 真实镜像向量表与内存布局 | 实际检查通过：Flash 起点、48 项向量、初始 SP、IRQ 地址、data/BSS/栈边界 |
| 主机桥接逻辑测试 | 实际通过 7 组；ASan/UBSan；双向共 20,000 字节逐字节检查，另覆盖拥塞及异常路径 |
| 所有新增/修改 C/H 格式 | 15 个文件、64 个函数说明审核通过；包括两个测试文件；官方 Drivers 不作格式改写 |
| Keil 工程 | 仅 XML/路径/编译源一致性静态验证，22 个输入均存在且位于本目录；未执行 Keil 构建 |
| Drivers 保持原样 | 946 个文件与任务开始时 SHA256 一致，文件数量也一致 |
| 旧工作区状态 | 本次之前已有的 mcu_soft/MDK-ARM 删除状态保持不变；未删除原始工程 |
| 实板收发、切换时序、延迟、看门狗 | 尚未执行，不能由编译或 mock 测试代替 |

最终镜像 Flash 为 **7,956 / 32,768 B**，静态 RAM 为 **504 / 8,192 B**（data=12、BSS=492），栈另预留 1,024 B。
链接脚本把 `.stack` 固定在 SRAM 顶部，所以 ld 摘要的 RAM 100% 表示地址跨度，实际数据和栈之间仍有 6,664 B 间隔。
`size` 的 BSS=1516 包含 492 B 普通 BSS 和 1024 B 的 NOLOAD 栈保留。没有堆和动态分配。
编译通过不构成真实栈水位或 UART 时序验证。

## 实际执行命令

先检查 Windows 与 Ubuntu 的 Keil、ARMCC、ARMClang、arm-none-eabi-gcc；均未找到可调用安装。
检查 Tina SDK 后发现已有裸机 GCC 压缩包，用户明确同意临时解压。解压位置为：

```text
/home/dawn/project/.codex-mcu-backups/20261014-v2/toolchain/gcc-arm-none-eabi-8-2019-q3-update/
```

最终全量重编译：

```sh
cd /home/dawn/project/linkg_v3/linkg-mcu
set -o pipefail
make -B -j4 CROSS_COMPILE=/home/dawn/project/.codex-mcu-backups/20261014-v2/toolchain/gcc-arm-none-eabi-8-2019-q3-update/bin/arm-none-eabi- 2>&1 | tee validation/gcc-build.log
make test check 2>&1 | tee validation/checks.log
python3 tests/check_firmware.py /home/dawn/project/.codex-mcu-backups/20261014-v2/toolchain/gcc-arm-none-eabi-8-2019-q3-update/bin/arm-none-eabi- > validation/firmware.log
cd /home/dawn/project/linkg_v3
git diff --check
sha256sum -c /home/dawn/project/.codex-mcu-backups/20261014-v2/drivers-before.sha256
```

最终重新运行格式审核及完整性核对结果保存在 `final-audit.log`。
独立重建目录为 `/home/dawn/project/.codex-mcu-backups/20261014-v2/standalone-check/linkg-mcu`，该目录只有交付包内容，无 mcu_soft 目录。对该目录执行同样的 `make -j4 CROSS_COMPILE=...`，用 `cmp` 比较 BIN，结果完全一致。日志为 `standalone-build.log`。
应用编译使用 `-Wall -Wextra -Werror`。首次构建发现官方 HAL 的弱回调/仅 assert 使用的参数触发 unused-parameter；只为 HAL 编译单元增加 `-Wno-unused-parameter`，不修改官方源码。最终构建无警告、无错误。
初次失败日志保存在任务备份目录 `gcc-initial.log`，交付中的 `gcc-build.log` 是最终完整重编译日志。

## 新增和填充的文件

原先 BSP/Service 六个文件为空且未被 Git 跟踪，本次实际填入实现；下面以 Git 状态列为新增。

```text
.gitignore
Core/Inc/main.h
Core/Inc/stm32l0xx_hal_conf.h
Core/Inc/stm32l0xx_it.h
Core/Src/main.c
Core/Src/stm32l0xx_hal_msp.c
Core/Src/stm32l0xx_it.c
Core/Src/system_stm32l0xx.c
bsp/bsp_gpio.c
bsp/bsp_gpio.h
bsp/bsp_uart.c
bsp/bsp_uart.h
service/sbus_bridge.c
service/sbus_bridge.h
GCC/startup_stm32l031xx.s
GCC/stm32l031g6.ld
Makefile
README.md
tests/test_bridge.c
tests/mocks/stm32l0xx_hal.h
tests/check_project.py
tests/check_firmware.py
validation/REPORT.md
validation/gcc-build.log
validation/standalone-build.log
validation/checks.log
validation/firmware.log
validation/drivers-integrity.log
validation/drivers.sha256
validation/environment.txt
validation/final-audit.log
validation/changes.txt
validation/source-sha256.txt
```

## 修改的文件

- `MDK-ARM/soft.uvprojx`：加入全部 7 个应用 C 源文件，保留 14 个必要 HAL 单元及启动汇编；更新 include 路径、32 MHz 时钟、新输出位置、关闭构建后动作和自动烧录，去掉不必要的 RTE 外部 CMSIS 依赖。
- `MDK-ARM/soft.uvoptx`：移除旧业务分组缓存、断点、观察项，更新时钟，关闭调试自动下载/运行及 EventRecorder。Keil 可从 uvprojx 重建分组。
- `MDK-ARM/startup_stm32l031xx.s`：保留 STM32L031 向量和启动逻辑，栈仍为 1 KiB，堆从 512 B 改为 0，统一空白和换行。

## 移除项

**本次没有删除源文件，也未删除原始工程。** 从新工程的编译/调试引用中移除了：

- `user_main.c`、`usart_server.c`、`Regtarge.c` 和旧 user_app 分组。
- `stm32l0xx_hal_adc.c`、`stm32l0xx_hal_adc_ex.c`。
- `stm32l0xx_hal_i2c.c`、`stm32l0xx_hal_i2c_ex.c`。
- `stm32l0xx_hal_tim.c`、`stm32l0xx_hal_tim_ex.c`。
- `stm32l0xx_hal_spi.c`、`stm32l0xx_hal_exti.c`。
- 所有旧电源管理、充电、电池、蜂鸣器、LED、按键、25 字节 SBUS 处理和定时发送逻辑均未迁入新源码。

官方上述驱动源文件仍保留在 Drivers 内，这是保留官方驱动目录的要求，且不参与新固件业务构建。
原 MDK 的 `soft/` 旧镜像、个人 GUI 缓存、日志等仍留在 Ubuntu 原目录，未被新工程引用；独立交付压缩包仅收录新的必要 MDK 文件，不含这些历史产物。

## 产物与备份

- 新固件：`build/gcc/linkg_mcu_v2.elf`、`.hex`、`.bin`、`.map`，校验和见 `firmware.log`。
- 完整独立工程包：任务备份目录中的 `linkg-mcu-v2.0.tar.gz`，包含全部 Drivers、新 Core/BSP/Service、Keil/GCC 构建配置、测试、文档、日志和本次固件产物。
- 原 MCU 完整快照：`/home/dawn/project/.codex-mcu-backups/20261014-v2/linkg-mcu-before.tar.gz`。
- 初始 Git 状态：同目录 `status-before.txt`；原 Drivers 清单：`drivers-before.sha256`。

## 实板前必须了解的限制

1. R118 为 NC，复位期间 PB1 尚未输出时 CTL 状态不保证；固件只能在 GPIO 初始化后给出明确电平，无法替代硬件偏置。
2. 退出桥接会立即停止 UART，在途字节可能被截断；软件队列丢弃和 TC 未完成分别统计，不补发。Linux 应在静止流量时协调切换。
3. PE/FE/NE 标记字节被丢弃；ORE 不虚构丢失数据；FIFO 满丢新字节并统计。后续协议重同步由 Linux 负责。
4. 100000/8E2 每字节约 120 μs；实板转发延迟、抖动、同时双向吞吐、HSI 温漂和 FIFO 水位尚未测量。
5. IWDG 标称约 1.73 s，实际取决于 LSI；只保护启动后的执行，早期时钟初始化尚未启用 IWDG。尚未实测复位和栈水位。
6. Keil 仍需在具有 ARM Compiler 5 和 STM32L0 DFP 的机器上执行一次实际构建。

参数映射、GPIO 全部流程、FIFO 数据流和实板测试步骤见上级 `README.md`。

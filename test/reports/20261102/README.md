# LinkG V1 测试归档（2026-11-02）

- `TEST_REPORT.md`：本轮综合结论、详细用例、板端数据和验证边界。
- `TEST_MATRIX.md`：原测试清单 T01～T16、I01～I16 的覆盖映射。
- `logic_ctest.log`：x86 C17 严格告警、ASan/UBSan 单元测试原始日志。
- `supervisor_ctest.log`：Supervisor 进程与 Release 逻辑场景原始日志。
- `mcu_sbus.log`：MCU/SBUS 主机逻辑与工程一致性检查日志。
- `product_build.log`：定版源码树外 Release 构建日志。
- `board_short_check.json`：两块 LinkG 板和 RK 板 180 秒双快照原始数据。
- `uart0_loopback.json`：两块 LinkG 板 UART0 normal/SBUS 回环原始结果。
- `rk_video_dhcp.json`：RK DHCP 与 20 秒 RTP/H.264 数据面结果。
- `video_sender.log`、`video_receiver.log`：GStreamer 原始日志。
- `environment.txt`、`git_state.txt`：版本、工具链、工作区和二进制哈希证据。

压力、性能、长时间稳定性、外场和专用硬件测试由人工另行执行，不在本归档的通过率中。

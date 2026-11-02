# LinkG V1 独立测试套件

本目录只包含测试代码、测试脚本和测试报告，不修改定版产品源码。

本轮测试范围是核心逻辑、异常与边界条件、资源生命周期，以及板端分钟级短时运行检查。压力、性能、长时间稳定性和依赖专用外设的硬件验收不在本轮完成率中，由人工另行执行。

## Ubuntu 主机单元测试

```sh
cmake -S test -B /tmp/linkg-v1-logic -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLINKG_TEST_SANITIZERS=ON
cmake --build /tmp/linkg-v1-logic --parallel
ctest --test-dir /tmp/linkg-v1-logic --output-on-failure
```

也可以直接执行 `test/run_logic_tests.sh`；传入第一个参数可覆盖 `/tmp` 构建目录。单套件可用 `ctest --test-dir <build> -R <name> --output-on-failure` 执行。

测试目标使用严格编译告警，并默认启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。测试进程不链接或替换产品可执行文件。

## 目录

- `unit/`：基础设施、包池、Node/Path、Transport、Discovery/Probe、Switch Wire/Verify 和配置逻辑测试；Wire 解码包含固定种子的畸形报文语料。
- `common/`：轻量测试断言和日志替身。
- `integration/`：板端短时只读检查及明确授权的回环/网络测试脚本。
- `reports/`：带原始日志和验证边界的测试报告。

## 链路切换专项测试

`integration/switch_t0_baseline.py` 用于短时 Wi-Fi T0 正常基线：运行现有 RK VIDEO 数据面测试、按秒采集 AP/STA Switch/Wi-Fi/Cellular Web 状态，并临时采集 STA 的 HCC/LOSS trace。脚本只创建已知的 `/tmp` trace 文件，使用 `finally` 清理；不修改产品配置或 TC。

```sh
python3 test/integration/switch_t0_baseline.py \
  --password '<board-password>' \
  --duration 60 \
  --output test/reports/20261102_switch/t0_baseline.json
```

专项报告位于 `reports/20261102_switch/`。初始任务书不允许覆盖 QoS，因此安全 filter 能力缺失时曾标记为 `BLOCKED`；用户随后明确授权测试期间用 root netem 替换 QoS，正式 T1～T5 已在该追加授权下执行。报告必须同时保留原限制、追加授权、恢复证据和最终队列状态。

5G Path 开启后的补充脚本：

- `integration/switch_dual_baseline.py`：双链路正常 VIDEO 与计划稳定性。
- `integration/cellular_probe_monitor.py`：只读采集 Cellular Probe 可达性并按板侧连续状态分组。
- `integration/cellular_icmp_diagnostic.py`：两板 Cellular IPv6 双向 ICMP 对照。
- `integration/cellular_correlation.py`：LinkG Probe 监测与 ICMP 同窗 A/B。
- `integration/tc_restore_validation.py`：0% netem 安装/`mq + 8×fq_codel` 恢复演练。
- `integration/tc_loss_scenario.py`：单级 1/3/5/10/100% 场景；三类业务、Web 计划、TC、接口、UDP 接收和 PCAP 同窗采集；`finally` 回滚 qdisc。
- `integration/cellular_path_reset.py`：自动回切失败后的场景间人工复位；保存/恢复原 Cellular 配置并核对 `/app/linkgUser.json` SHA-256。该脚本不能作为自动回切通过证据。
- `integration/tc_campaign_analyze.py`：从五个原始 JSON 生成紧凑指标、TC 报告和实际事件时间线。

正式丢包示例：

```sh
python3 test/integration/tc_loss_scenario.py \
  --test-id T3_5P_STA --loss 5 --duration 60 \
  --password '<board-password>' \
  --output test/reports/20261102_switch/tc/T3_5P_STA/result.json
```

100% 双向场景必须额外传入 `--both`。运行前必须保证管理 SSH 不依赖 `wlan0`，且不得同时运行其他 TC 修改工具。

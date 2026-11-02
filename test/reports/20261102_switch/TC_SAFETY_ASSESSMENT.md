# TC 丢包注入安全评估与授权偏离记录

## 原始评估

AP/STA `wlan0` 均为 1 个 `mq` root + 8 个 `fq_codel` leaf。板端对 root/ingress/egress filter 和 gact 查询均返回 `Operation not supported`，因此不存在既保留原 QoS 又能挂载随机 drop 的 clsact/filter 路径。

原任务书要求“不覆盖或破坏现有 QoS”时，T1～T5 判定为 BLOCKED 是正确的；`sch_netem` 已加载并不等于挂载方式安全。

## 用户追加授权

用户随后明确授权：

- 可以直接对整个无线进行偶发丢包；
- 测试期间允许不按原 QoS；
- 除修改定版源码外，所有测试开关允许使用。

因此后续边界改为：允许临时 `tc qdisc replace dev wlan0 root ... netem`，但必须先验证自动恢复，所有正式场景均使用 `finally` 回滚，并在每级后复核完整队列语义。该授权偏离必须保留，不能把它描述成“满足原任务书不覆盖 QoS”。

## 恢复演练

初次 0% 演练发现两点：

1. `tc qdisc replace dev wlan0 root mq` 会让内核重新分配 root handle（如 `8001:`），leaf parent 也相应变为 `8001:1`～`:8`；但 8 个 `fq_codel` 的 limit/flows/quantum/target/interval/memory_limit/ECN 参数自动重建。因此恢复比较采用队列语义，不能错误要求 handle 必须仍为 `0:`。
2. 板端 iproute2 5.11 + Linux 5.4.61 的 `tc -d` 将显式 `delay 0ms` 显示成 `delay 275s 275s`。1% 微探针中，50 个 STA→AP 直连 ICMP 收到 49 个，RTT 1.289～2.648 ms，netem 统计 66 包中丢 1；证明 275s 是显示换算异常，不是实际排队延迟。

正式语法固定为：

```sh
tc qdisc replace dev wlan0 root handle 7ffe: netem delay 0ms loss random X%
```

恢复语法固定为：

```sh
tc qdisc replace dev wlan0 root mq
```

## 正式执行与恢复结果

- T1～T4：仅 STA `wlan0` 出口注入 1%/3%/5%/10%，各约 60 秒。
- T5：AP/STA `wlan0` 双向 100%，约 60 秒。
- 每级均在独立管理 `eth0` SSH 下运行；无线完全阻塞不影响回滚。
- 每级结束均观察到 1 个 `mq` root 和 8 个 `fq_codel` leaf。
- 最终 AP root handle 为 `8003:`，STA 为 `8008:`；handle 变化是重建结果，不是队列缺失。
- 最终无 netem、GStreamer、tcpdump、场景脚本、trace marker/output 残留。
- AP/STA Wi-Fi 与 Cellular Path 均可用，三类计划均为 Wi-Fi 主、Cellular 备。

## 结论

在原任务书“不覆盖 QoS”边界下，安全挂载能力仍然不存在；在用户追加授权的新边界下，root netem 注入、自动回滚和最终恢复 **PASS**。二者不矛盾，报告必须明确区分。


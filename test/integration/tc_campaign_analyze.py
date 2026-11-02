#!/usr/bin/env python3
"""Build compact metrics and Markdown reports from the five TC scenario JSON files."""

import json
import re
from pathlib import Path


SCENARIOS = (
    ("T1", "T1_1P_STA", 1),
    ("T2", "T2_3P_STA", 3),
    ("T3", "T3_5P_STA", 5),
    ("T4", "T4_10P_STA", 10),
    ("T5", "T5_100P_BOTH", 100),
)
CLASSES = ("data", "video", "realtime")


def tc_stats(text):
    packets = re.search(r"Sent \d+ bytes (\d+) pkt", text)
    dropped = re.search(r"\(dropped (\d+),", text)
    sent = int(packets.group(1)) if packets else 0
    drop = int(dropped.group(1)) if dropped else 0
    attempted = sent + drop
    return {
        "sent_packets": sent,
        "dropped_packets": drop,
        "attempted_packets": attempted,
        "observed_drop_percent": round(drop * 100.0 / attempted, 3) if attempted else None,
    }


def class_state(event, name):
    return event["plan"][name]


def all_primary(event, access):
    return all(event["plan"][name]["mode"] == "single" and
               event["plan"][name]["primary"] == access for name in CLASSES)


def event_delta(event, base):
    return round(event["elapsed_seconds"] - base, 3)


def analyze(path, test_id, configured):
    data = json.loads(path.read_text(encoding="utf-8"))
    injection_start = data["injection_started_elapsed_seconds"]
    restore_start = data["injection_restore_started_elapsed_seconds"]
    sta_events = [item for item in data["plan_events"] if item["side"] == "sta"]
    loss_events = [item for item in sta_events if item["phase"] == "loss"]
    recovery_events = [item for item in sta_events if item["phase"] == "recovery"]

    first_redundant = {}
    first_cellular = {}
    for name in CLASSES:
        redundant = next((event for event in loss_events
                          if class_state(event, name)["mode"] == "redundant"), None)
        cellular = next((event for event in loss_events
                         if class_state(event, name)["mode"] == "single" and
                         class_state(event, name)["primary"] == "cellular"), None)
        first_redundant[name] = event_delta(redundant, injection_start) if redundant else None
        first_cellular[name] = event_delta(cellular, injection_start) if cellular else None

    migration_order = [name for name, _ in sorted(
        ((name, value) for name, value in first_cellular.items() if value is not None),
        key=lambda item: item[1],
    )]
    oscillations = []
    for name in CLASSES:
        seen_cellular = False
        for event in loss_events:
            state = class_state(event, name)
            if state["mode"] == "single" and state["primary"] == "cellular":
                seen_cellular = True
            elif seen_cellular and state["mode"] == "single" and state["primary"] == "wifi":
                oscillations.append({"class": name, "at_seconds": event_delta(event, injection_start)})

    recovery_all_wifi = next((event for event in recovery_events if all_primary(event, "wifi")), None)
    recovery_attempts = [event for event in recovery_events if any(
        event["plan"][name]["mode"] == "redundant" for name in CLASSES
    )]

    observed_loss = []
    for sample in data["samples"]:
        if sample["phase"] != "loss":
            continue
        for side in ("ap", "sta"):
            for direction, item in (sample[side].get("loss") or {}).items():
                if not isinstance(item, dict) or item.get("valid") is not True:
                    continue
                if not isinstance(item.get("loss_permille"), int):
                    continue
                observed_loss.append({
                    "side": side,
                    "direction": direction,
                    "loss_permille": item["loss_permille"],
                    "lost_packets": item.get("lost_packets"),
                    "sample_packets": item.get("sample_packets"),
                    "captured_utc": sample["captured_utc"],
                    "seconds_after_injection": round(sample["elapsed_seconds"] - injection_start, 3),
                })
    max_any = max(observed_loss, key=lambda item: item["loss_permille"], default=None)
    eligible = [item for item in observed_loss if (item.get("sample_packets") or 0) >= 20]
    max_n20 = max(eligible, key=lambda item: item["loss_permille"], default=None)

    tc = {}
    final_tc_sample = data["tc_samples"][-1]
    for side, result in final_tc_sample["boards"].items():
        tc[side] = tc_stats(result["stdout"])
    tc_checkpoint_after = round(final_tc_sample["elapsed_seconds"] - injection_start, 3)

    receivers = {
        name: {
            "packets": value["packets"],
            "bytes": value["bytes"],
            "max_gap_ms": value["max_gap_ms"],
            "gaps_ge_100ms_count": value["gaps_ge_100ms_count"],
        }
        for name, value in data["receivers"].items()
    }
    errors = {}
    for side in ("ap", "sta"):
        errors[side] = {
            interface: {"rx_errors": values["rx_errors"], "tx_errors": values["tx_errors"]}
            for interface, values in data.get("interface_delta", {}).get(side, {}).items()
            if values["rx_errors"] or values["tx_errors"]
        }

    qdisc_restored = all("qdisc mq " in value["stdout"] and
                         value["stdout"].count("qdisc fq_codel ") == 8
                         for value in data.get("tc_after_restore", {}).values())
    trace_path = next(path.parent.glob("*switch_trace.log"), None)
    trace = {"lines": 0, "off_zero": 0, "off_invalid": 0, "off_nonzero": 0,
             "ovf_zero": 0, "ovf_invalid": 0, "ovf_nonzero": 0}
    if trace_path is not None:
        for line in trace_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if not line.strip():
                continue
            trace["lines"] += 1
            for token in line.split():
                if token == "OFF=0":
                    trace["off_zero"] += 1
                elif token == "OFF=--":
                    trace["off_invalid"] += 1
                elif token.startswith("OFF="):
                    trace["off_nonzero"] += 1
                if token == "OVF=0":
                    trace["ovf_zero"] += 1
                elif token == "OVF=--":
                    trace["ovf_invalid"] += 1
                elif token.startswith("OVF="):
                    trace["ovf_nonzero"] += 1
    result = {
        "test_id": test_id,
        "configured_loss_percent": configured,
        "injection_sides": data["injection_sides"],
        "injection_actual_seconds_including_restore_check": data.get("injection_actual_seconds"),
        "tc_checkpoint_seconds_after_injection": tc_checkpoint_after,
        "tc_checkpoint": tc,
        "linkg_loss_max_any": max_any,
        "linkg_loss_max_sample_ge_20": max_n20,
        "first_redundant_seconds": first_redundant,
        "first_cellular_primary_seconds": first_cellular,
        "migration_order": migration_order,
        "loss_phase_wifi_return_oscillations": oscillations,
        "automatic_recovery_confirmed": data.get("recovery_normal_confirmed") is True,
        "first_all_wifi_seconds_after_restore": event_delta(recovery_all_wifi, restore_start)
            if recovery_all_wifi else None,
        "recovery_redundant_attempts": len(recovery_attempts),
        "receiver": receivers,
        "sta_usb0_tx_bytes": data.get("interface_delta", {}).get("sta", {}).get("usb0", {}).get("tx_bytes"),
        "ap_usb0_rx_bytes": data.get("interface_delta", {}).get("ap", {}).get("usb0", {}).get("rx_bytes"),
        "interface_error_deltas": errors,
        "hcc_trace": trace,
        "qdisc_restored_semantically": qdisc_restored,
        "cleanup_ok": data.get("cleanup_ok") is True,
        "scenario_error": data.get("error"),
        "sta_plan_events": sta_events,
        "raw_result": str(path),
        "pcap": str(path.with_suffix(".pcap")),
    }
    return result


def value(value, digits=3):
    if value is None:
        return "—"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def main():
    base = Path(__file__).resolve().parents[2] / "test" / "reports" / "20261102_switch" / "tc"
    metrics = []
    for test_id, directory, configured in SCENARIOS:
        metrics.append(analyze(base / directory / "result.json", test_id, configured))
    compact_path = base / "TC_LOSS_METRICS.json"
    compact_path.write_text(json.dumps(metrics, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    lines = [
        "# TC 渐进丢包与链路切换实测报告",
        "",
        "## 1. 执行边界与方法",
        "",
        "本轮依据用户明确授权，临时以 `netem` 替换 `wlan0` root；测试期间不保留原 `mq/fq_codel` QoS。"
        "每级结束均执行 `tc qdisc replace dev wlan0 root mq`，并验证恢复为 1 个 `mq` root + 8 个 `fq_codel` leaf。"
        "该授权偏离原任务书的“不覆盖 QoS”限制，但没有修改定版产品源码或算法阈值。",
        "",
        "T1～T4 在 STA `wlan0` 出口单向注入，T5 在 AP/STA 两侧双向注入。每级非零丢包约 60 秒。"
        "同窗持续发送 REALTIME(UDP/14550)、摄像头 VIDEO(UDP/5600) 和 DATA(UDP/6200)，"
        "并以约 250 ms 间隔采集 AP/STA Send Plan。接收端记录逐秒包数与最大包间隙，AP `eth0` 同时保存 PCAP。",
        "",
        "注意：脚本最后一次 `tc -s` 检查点约在注入后 55 秒，不是 60 秒清理前的最终累计值；"
        "下表丢包数量是该检查点的原始计数，丢包比例可用于核对 netem 生效，但不能冒充完整 60 秒总量。",
        "",
        "## 2. TC 与算法结果汇总",
        "",
        "| 场景 | 配置 | 末次检查点成功/丢弃 | 实测比例 | 首次迁移顺序 | 自动回切 | 结论 |",
        "|---|---:|---:|---:|---|---|---|",
    ]
    conclusions = {
        "T1": "PASS：轻微丢包，无误切换",
        "T2": "迁移 PASS；恢复 FAIL",
        "T3": "迁移 PASS；振荡/恢复 FAIL",
        "T4": "迁移 PASS；振荡/恢复 FAIL",
        "T5": "功能切换/恢复 PASS；业务中断指标 FAIL",
    }
    for item in metrics:
        side = "sta"
        tc = item["tc_checkpoint"][side]
        order = " → ".join(name.upper() for name in item["migration_order"]) or "无迁移"
        recovered = ("不适用" if not item["migration_order"] else
                     ("是" if item["automatic_recovery_confirmed"] else "否"))
        lines.append(
            f"| {item['test_id']} | {item['configured_loss_percent']}% | "
            f"{tc['sent_packets']}/{tc['dropped_packets']} | {value(tc['observed_drop_percent'])}% | "
            f"{order} | {recovered} | {conclusions[item['test_id']]} |"
        )

    lines.extend([
        "",
        "## 3. LinkG LOSS 有效窗口对照",
        "",
        "下表使用 `sample_packets >= 20` 的 Web 有效窗口；它是 LinkG Wire 窗口值，不应与 netem qdisc 累计比例机械等同。",
        "",
        "| 场景 | 最大有效 LOSS | lost/sample | 侧别/方向 | 说明 |",
        "|---|---:|---:|---|---|",
    ])
    for item in metrics:
        observed = item["linkg_loss_max_sample_ge_20"]
        if observed is None:
            lines.append(f"| {item['test_id']} | — | — | — | 无有效窗口 |")
        else:
            note = "100% 黑洞后无新 Wire 样本，最大值来自故障刚开始前" if item["test_id"] == "T5" else "窗口统计"
            lines.append(
                f"| {item['test_id']} | {observed['loss_permille']}‰ | "
                f"{observed['lost_packets']}/{observed['sample_packets']} | "
                f"{observed['side']}/{observed['direction']} | {note} |"
            )

    lines.extend([
        "",
        "## 4. 计划迁移实测时间",
        "",
        "时间起点为完成本级 netem 安装后的实际采样时间；`—` 表示 60 秒内未观察到。",
        "",
        "| 场景 | DATA 冗余/5G主 (s) | VIDEO 冗余/5G主 (s) | REALTIME 冗余/5G主 (s) | 丢包期回 Wi-Fi 振荡 |",
        "|---|---:|---:|---:|---:|",
    ])
    for item in metrics:
        values = []
        for name in CLASSES:
            values.append(f"{value(item['first_redundant_seconds'][name])}/{value(item['first_cellular_primary_seconds'][name])}")
        lines.append(f"| {item['test_id']} | {values[0]} | {values[1]} | {values[2]} | "
                     f"{len(item['loss_phase_wifi_return_oscillations'])} |")

    lines.extend([
        "",
        "## 5. 实际业务路径与连续性",
        "",
        "| 场景 | STA usb0 TX | AP usb0 RX | DATA 最大间隙 | VIDEO 最大间隙 | REALTIME 最大间隙 |",
        "|---|---:|---:|---:|---:|---:|",
    ])
    for item in metrics:
        lines.append(
            f"| {item['test_id']} | {item['sta_usb0_tx_bytes']:,} B | {item['ap_usb0_rx_bytes']:,} B | "
            f"{item['receiver']['data']['max_gap_ms']:.3f} ms | {item['receiver']['video']['max_gap_ms']:.3f} ms | "
            f"{item['receiver']['realtime']['max_gap_ms']:.3f} ms |"
        )

    t5 = next(item for item in metrics if item["test_id"] == "T5")
    lines.extend([
        "",
        "## 6. 关键判定",
        "",
        "1. **T1 轻微丢包无误切换。** 1% 检查点比例与配置相符，三类计划始终 Wi-Fi 主。",
        "2. **T2 首次证明分业务迁移和 5G 用户承载。** DATA 先迁移，约 10 秒后 VIDEO 迁移，"
        "REALTIME 保留 Wi-Fi；`usb0` 的同方向大字节增量证明不是只改了策略字段。",
        "3. **迁移顺序像 LOAD 分级，但没有直接 HCC 负载证据。** T2～T4 trace 的所有有效 `OFF` 都为 0，"
        "所有有效 `OVF` 也为 0；DATA→VIDEO→REALTIME 的分级顺序与源码 LOAD 分支一致，而 LOSS 分支按源码会让参与类一起冗余。"
        "Web 不暴露 migration_reason，因此只能标记为“疑似 TC LOSS 被快速质量组合路径按 LOAD 风格迁移”，不能直接断言内部事件名。",
        "4. **T3/T4 在丢包仍存在时发生回 Wi-Fi 后再次迁出的振荡。** 这不是 TC 清除导致；"
        "事件均发生在 loss phase，完整计划序列保存在 JSON。",
        "5. **T2～T4 清除 TC 后 60 秒均未自动回切。** 内部迁移原因不可见；若原因是 LOSS，RSSI 需比迁出基线高至少 3 dB；"
        "若是 LOAD，RSSI 也必须不低于迁出基线。T2 迁出时约 -38/-39 dBm，恢复观察为 -47～-49 dBm，两种门槛都不满足。"
        "场景间通过临时关闭/恢复 STA Cellular Path 复位；复位前后 `/app/linkgUser.json` SHA-256 一致。"
        "人工复位不能算自动回切通过。",
        f"6. **T5 双向全丢包最终能够切到 5G 并自动恢复。** 清除 TC 后第一次冗余验证回退到 5G，"
        f"第二次才回 Wi-Fi；首次全 Wi-Fi 计划约为 {value(t5['first_all_wifi_seconds_after_restore'])} 秒。",
        "7. **业务中断不满足 500 ms 参考目标。** T5 最大包间隙 DATA/VIDEO/REALTIME 分别为 "
        f"{t5['receiver']['data']['max_gap_ms']:.3f}/"
        f"{t5['receiver']['video']['max_gap_ms']:.3f}/"
        f"{t5['receiver']['realtime']['max_gap_ms']:.3f} ms。",
        "8. **内部 Verify/LOSS_SCORE/LOAD_SCORE/Operation ID 仍不可直接验收。** Web 只能看到计划和 PlanSync 摘要，"
        "所以本报告能证明外部状态转换与实际数据路径，不能把某个内部结果名（WIFI/CELLULAR/MIXED/INCONCLUSIVE）当作已直接观察。",
        "9. **接口错误计数出现少量增量。** AP `usb0 rx_errors` 在 T2/T3/T5 分别增加 4/3/1，T1/T4 为 0；"
        "本轮无法仅凭 sysfs 计数唯一归因，但已保留在紧凑指标和原始 JSON，不能写成所有场景接口错误均为 0。",
        "",
        "## 7. 清理与最终状态",
        "",
        "五级场景均无产品源码修改。每级 qdisc 均恢复为 `mq + 8×fq_codel`；最终 AP/STA Wi-Fi 与 Cellular Path 均可用，"
        "三类计划均为 Wi-Fi single 主发、Cellular 备用；无 GStreamer、tcpdump、TC 场景脚本或 trace 临时文件残留。",
    ])
    (base / "TC_LOSS_CAMPAIGN_REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    timeline = [
        "# TC 场景 STA Send Plan 事件时间线",
        "",
        "以下时间均来自实际 Web 快照；相对时间以各场景 netem 安装完成为 0 秒。",
        "",
    ]
    for item in metrics:
        timeline.extend([f"## {item['test_id']}（{item['configured_loss_percent']}%）", ""])
        injection_start = json.loads(Path(item["raw_result"]).read_text(encoding="utf-8"))["injection_started_elapsed_seconds"]
        for event in item["sta_plan_events"]:
            if event["phase"] == "baseline":
                relative = "baseline"
            else:
                relative = f"{event['elapsed_seconds'] - injection_start:+.3f}s"
            plan = "; ".join(
                f"{name.upper()}={event['plan'][name]['mode']}/"
                f"{event['plan'][name]['primary']}+{event['plan'][name]['secondary']}"
                for name in ("data", "video", "realtime")
            )
            timeline.append(f"- `{event['captured_utc']}` / {relative} / {event['phase']}: {plan}")
        timeline.append("")
    (base / "TC_EVENT_TIMELINE.md").write_text("\n".join(timeline), encoding="utf-8")

    print(json.dumps({
        "metrics": str(compact_path),
        "report": str(base / "TC_LOSS_CAMPAIGN_REPORT.md"),
        "timeline": str(base / "TC_EVENT_TIMELINE.md"),
        "scenarios": len(metrics),
    }, ensure_ascii=False))


if __name__ == "__main__":
    main()

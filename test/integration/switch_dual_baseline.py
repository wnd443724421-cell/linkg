#!/usr/bin/env python3
"""Run a bounded dual-path normal baseline while keeping Wi-Fi primary."""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

from switch_t0_baseline import (
    AP_HOST,
    STA_HOST,
    connect,
    qdisc_structure,
    response_data,
    snapshot,
    utc_now,
    web_request,
)


def plan_from_switch(item, side):
    data = response_data(item)
    if data is None:
        return None
    if side == "sta":
        return data.get("sta", {}).get("plan")
    peers = data.get("ap", {}).get("peers", [])
    return peers[0].get("plan") if peers else None


def plan_is_wifi_primary_cellular_secondary(plan):
    classes = (plan or {}).get("classes", {})
    if set(classes) != {"realtime", "video", "data"}:
        return False
    return all(
        item.get("mode") == "single"
        and item.get("primary", {}).get("access") == "wifi"
        and item.get("secondary", {}).get("access") == "cellular"
        for item in classes.values()
    )


def cellular_probes(item):
    data = response_data(item)
    if data is None:
        return None
    peers = data.get("path", {}).get("peers", [])
    if len(peers) != 1 or not peers[0].get("active"):
        return None
    return peers[0].get("probe")


def probes_reachable(probes):
    if not isinstance(probes, dict) or set(probes) != {"realtime", "video", "data"}:
        return False
    return all(
        item.get("valid") is True
        and item.get("reachable") is True
        and isinstance(item.get("rtt_us"), int)
        and item.get("rtt_us") > 0
        for item in probes.values()
    )


def parse_interfaces(text):
    result = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 7 and all(value.isdigit() for value in fields[1:]):
            result[fields[0]] = {
                "rx_bytes": int(fields[1]),
                "tx_bytes": int(fields[2]),
                "rx_packets": int(fields[3]),
                "tx_packets": int(fields[4]),
                "rx_errors": int(fields[5]),
                "tx_errors": int(fields[6]),
            }
    return result


def interface_delta(before, after):
    result = {}
    for name, current in after.items():
        initial = before.get(name, {})
        result[name] = {key: value - initial.get(key, 0) for key, value in current.items()}
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--duration", type=int, default=60)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.duration < 5 or args.duration > 120:
        raise SystemExit("duration must be in [5, 120] seconds")

    output_path = Path(args.output).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    video_path = output_path.with_name("dual_video.json")
    repo_root = Path(__file__).resolve().parents[2]
    video_script = repo_root / "test" / "integration" / "rk_video_dhcp_test.py"
    report = {
        "test_id": "SW-DUAL-NORMAL-BASELINE",
        "started_utc": utc_now(),
        "duration_seconds": args.duration,
        "samples": [],
    }
    clients = {}
    video_process = None

    try:
        clients["ap"] = connect(AP_HOST, args.password)
        clients["sta"] = connect(STA_HOST, args.password)
        report["before"] = {name: snapshot(client) for name, client in clients.items()}

        video_process = subprocess.Popen(
            [sys.executable, str(video_script), "--password", args.password,
             "--duration", str(args.duration), "--output", str(video_path)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        deadline = time.monotonic() + args.duration + 20
        while video_process.poll() is None and time.monotonic() < deadline:
            started = time.monotonic()
            report["samples"].append({
                "captured_utc": utc_now(),
                "ap": {str(cmd): web_request(clients["ap"], cmd) for cmd in (1003, 1006, 1009)},
                "sta": {str(cmd): web_request(clients["sta"], cmd) for cmd in (1003, 1006, 1009)},
            })
            delay = 1.0 - (time.monotonic() - started)
            if delay > 0:
                time.sleep(delay)
        if video_process.poll() is None:
            video_process.terminate()
            raise RuntimeError("dual-path video baseline exceeded bounded deadline")

        video_stdout, _ = video_process.communicate(timeout=5)
        report["video_runner"] = {
            "exit": video_process.returncode,
            "stdout": video_stdout,
            "result": str(video_path),
        }
        if video_path.exists():
            report["video_result"] = json.loads(video_path.read_text(encoding="utf-8"))
        report["after"] = {name: snapshot(client) for name, client in clients.items()}

        web_errors = []
        bad_plans = []
        bad_cellular = []
        rtt = {side: {name: [] for name in ("realtime", "video", "data")} for side in ("ap", "sta")}
        for sample in report["samples"]:
            for side in ("ap", "sta"):
                for command in ("1003", "1006", "1009"):
                    if response_data(sample[side][command]) is None:
                        web_errors.append({"time": sample["captured_utc"], "side": side, "cmd": command})
                plan = plan_from_switch(sample[side]["1003"], side)
                if not plan_is_wifi_primary_cellular_secondary(plan):
                    bad_plans.append({"time": sample["captured_utc"], "side": side, "plan": plan})
                probes = cellular_probes(sample[side]["1009"])
                if not probes_reachable(probes):
                    bad_cellular.append({"time": sample["captured_utc"], "side": side, "probes": probes})
                elif probes is not None:
                    for name, item in probes.items():
                        rtt[side][name].append(item["rtt_us"])

        qdisc_unchanged = all(
            qdisc_structure(report["before"][side]) == qdisc_structure(report["after"][side])
            for side in ("ap", "sta")
        )
        deltas = {}
        errors_zero = True
        for side in ("ap", "sta"):
            before = parse_interfaces(report["before"][side]["interfaces"]["stdout"])
            after = parse_interfaces(report["after"][side]["interfaces"]["stdout"])
            deltas[side] = interface_delta(before, after)
            errors_zero = errors_zero and all(
                values.get("rx_errors", 0) == 0 and values.get("tx_errors", 0) == 0
                for values in after.values()
            )
        report["interface_delta"] = deltas
        report["cellular_rtt_us"] = {
            side: {
                name: {
                    "count": len(values),
                    "min": min(values) if values else 0,
                    "max": max(values) if values else 0,
                    "mean": (sum(values) / len(values)) if values else 0,
                }
                for name, values in classes.items()
            }
            for side, classes in rtt.items()
        }
        report["summary"] = {
            "sample_count": len(report["samples"]),
            "web_errors": web_errors,
            "bad_plans": bad_plans,
            "bad_cellular_probe_samples": bad_cellular,
            "plans_stable_wifi_primary_cellular_secondary": not bad_plans,
            "cellular_probes_reachable_all_samples": not bad_cellular,
            "qdisc_structure_unchanged": qdisc_unchanged,
            "interface_errors_zero": errors_zero,
            "video_passed": bool(report.get("video_result", {}).get("passed")),
        }
        report["passed"] = all((
            report["samples"],
            not web_errors,
            not bad_plans,
            not bad_cellular,
            qdisc_unchanged,
            errors_zero,
            report["summary"]["video_passed"],
        ))
    except Exception as exc:
        report["passed"] = False
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if video_process is not None and video_process.poll() is None:
            video_process.terminate()
            try:
                video_process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                video_process.kill()
        for client in clients.values():
            client.close()
        report["finished_utc"] = utc_now()
        output_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(json.dumps({
        "output": str(output_path),
        "passed": report.get("passed"),
        "samples": len(report.get("samples", [])),
        "error": report.get("error"),
    }, ensure_ascii=False))
    raise SystemExit(0 if report.get("passed") else 1)


if __name__ == "__main__":
    main()

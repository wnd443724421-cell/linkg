#!/usr/bin/env python3
"""Collect a bounded T0 Wi-Fi quality baseline without changing product config or TC."""

import argparse
import json
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import paramiko


AP_HOST = "192.168.1.1"
STA_HOST = "192.168.3.1"
TRACE_ENABLE = "/tmp/linkg_switch_trace.enable"
TRACE_OUTPUT = "/tmp/linkg_switch_wifi_250ms.log"


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def connect(host, password):
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(host, username="root", password=password, timeout=8,
                   auth_timeout=8, banner_timeout=8)
    return client


def execute(client, command, timeout=15):
    stdin, stdout, stderr = client.exec_command(command, timeout=timeout)
    del stdin
    stdout.channel.settimeout(timeout)
    output = stdout.read().decode("utf-8", "replace")
    error = stderr.read().decode("utf-8", "replace")
    return {"exit": stdout.channel.recv_exit_status(), "stdout": output, "stderr": error}


def web_request(client, command):
    payload = json.dumps({"cmd": command}, separators=(",", ":")).encode()
    channel = None
    try:
        channel = client.get_transport().open_channel(
            "direct-tcpip", ("127.0.0.1", 29608), ("127.0.0.1", 0), timeout=5
        )
        channel.settimeout(5)
        channel.sendall(payload)
        channel.shutdown_write()
        chunks = []
        while True:
            chunk = channel.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
        raw = b"".join(chunks).decode("utf-8", "replace")
        return {"response": json.loads(raw), "raw": raw}
    except Exception as exc:
        return {"error": f"{type(exc).__name__}: {exc}"}
    finally:
        if channel is not None:
            channel.close()


def snapshot(client):
    return {
        "switch": web_request(client, 1003),
        "wifi": web_request(client, 1006),
        "cellular": web_request(client, 1009),
        "interfaces": execute(
            client,
            "for i in eth0 wlan0 linkg0 usb0; do "
            "[ -d /sys/class/net/$i ] || continue; printf '%s ' $i; "
            "cat /sys/class/net/$i/statistics/rx_bytes "
            "/sys/class/net/$i/statistics/tx_bytes "
            "/sys/class/net/$i/statistics/rx_packets "
            "/sys/class/net/$i/statistics/tx_packets "
            "/sys/class/net/$i/statistics/rx_errors "
            "/sys/class/net/$i/statistics/tx_errors | tr '\\n' ' '; echo; done",
        ),
        "qdisc": execute(client, "tc qdisc show dev wlan0; tc -s qdisc show dev wlan0"),
        "wifi_link": execute(client, "iw dev wlan0 info 2>&1; iw dev wlan0 link 2>&1"),
    }


def response_data(item):
    response = item.get("response") if isinstance(item, dict) else None
    if not isinstance(response, dict) or response.get("code") != "ok":
        return None
    data = response.get("data")
    return data if isinstance(data, dict) else None


def plan_signature(switch_item):
    data = response_data(switch_item)
    if data is None:
        return None
    if data.get("role") == "sta":
        return data.get("sta", {}).get("plan")
    peers = data.get("ap", {}).get("peers", [])
    return peers[0].get("plan") if peers else None


def plan_is_wifi_single(plan):
    classes = (plan or {}).get("classes", {})
    if set(classes) != {"realtime", "video", "data"}:
        return False
    return all(
        item.get("mode") == "single"
        and item.get("primary", {}).get("access") == "wifi"
        and item.get("secondary", {}).get("access") == "none"
        for item in classes.values()
    )


def parse_trace(text):
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    pattern = re.compile(r"^(UL|DL) Q=([^ ]+)  OFF=([^ ]+)  OVF=([^ ]+)  LOSS=([^ ]+)$")
    parsed = []
    malformed = []
    for line in lines:
        match = pattern.match(line)
        if not match:
            malformed.append(line)
            continue
        direction, queues, off, overflow, loss = match.groups()
        parsed.append({"direction": direction, "queues": queues,
                       "off_delta": off, "overflow": overflow, "loss": loss})
    numeric_off = [int(item["off_delta"]) for item in parsed if item["off_delta"].isdigit()]
    queue_valid = [item for item in parsed if "--" not in item["queues"]]
    return {
        "nonempty_lines": len(lines),
        "parsed_lines": len(parsed),
        "malformed_lines": malformed,
        "valid_hcc_lines": len(queue_valid),
        "off_delta_sum": sum(numeric_off),
        "off_delta_max": max(numeric_off, default=0),
        "directions": sorted(set(item["direction"] for item in parsed)),
    }


def qdisc_structure(snapshot_item):
    text = snapshot_item.get("qdisc", {}).get("stdout", "")
    return [line.strip() for line in text.splitlines() if line.startswith("qdisc ")][:9]


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
    trace_path = output_path.with_name("t0_switch_trace.log")
    video_path = output_path.with_name("t0_video.json")
    repo_root = Path(__file__).resolve().parents[2]
    video_script = repo_root / "test" / "integration" / "rk_video_dhcp_test.py"
    report = {
        "test_id": "SW-T0-WIFI-BASELINE",
        "started_utc": utc_now(),
        "duration_seconds": args.duration,
        "scope": "Wi-Fi-only normal baseline; no product config or TC mutation",
        "samples": [],
    }
    clients = {}
    marker_created = False
    trace_created = False
    video_process = None

    try:
        clients["ap"] = connect(AP_HOST, args.password)
        clients["sta"] = connect(STA_HOST, args.password)
        report["before"] = {name: snapshot(client) for name, client in clients.items()}
        trace_preflight = execute(
            clients["sta"], f"test ! -e {TRACE_ENABLE} && test ! -e {TRACE_OUTPUT}"
        )
        report["trace_preflight"] = trace_preflight
        if trace_preflight["exit"] != 0:
            raise RuntimeError("refusing to overwrite an existing switch trace marker/output")

        create_marker = execute(clients["sta"], f"touch {TRACE_ENABLE} && test -f {TRACE_ENABLE}")
        report["trace_enable"] = create_marker
        if create_marker["exit"] != 0:
            raise RuntimeError("failed to enable bounded switch trace")
        marker_created = True

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
            raise RuntimeError("video baseline exceeded bounded deadline")

        video_stdout, _ = video_process.communicate(timeout=5)
        report["video_runner"] = {"exit": video_process.returncode,
                                  "stdout": video_stdout, "result": str(video_path)}
        if video_path.exists():
            report["video_result"] = json.loads(video_path.read_text(encoding="utf-8"))

        report["trace_disable"] = execute(clients["sta"], f"rm -f {TRACE_ENABLE}")
        marker_created = False
        time.sleep(1.5)
        trace_result = execute(clients["sta"], f"test -f {TRACE_OUTPUT} && cat {TRACE_OUTPUT}")
        if trace_result["exit"] != 0:
            raise RuntimeError("switch trace output was not produced")
        trace_created = True
        trace_path.write_text(trace_result["stdout"], encoding="utf-8")
        report["trace"] = {"saved_to": str(trace_path), "summary": parse_trace(trace_result["stdout"])}
        report["trace_output_cleanup"] = execute(clients["sta"], f"rm -f {TRACE_OUTPUT}")
        trace_created = False

        report["after"] = {name: snapshot(client) for name, client in clients.items()}
        report["cleanup_verify"] = execute(
            clients["sta"], f"test ! -e {TRACE_ENABLE} && test ! -e {TRACE_OUTPUT}"
        )
        plan_samples = []
        web_errors = []
        for sample in report["samples"]:
            for side in ("ap", "sta"):
                for cmd in ("1003", "1006", "1009"):
                    if response_data(sample[side][cmd]) is None:
                        web_errors.append({"captured_utc": sample["captured_utc"],
                                           "side": side, "cmd": cmd})
                plan_samples.append(plan_signature(sample[side]["1003"]))
        qdisc_unchanged = all(
            qdisc_structure(report["before"][side]) == qdisc_structure(report["after"][side])
            for side in ("ap", "sta")
        )
        plans_stable = bool(plan_samples) and all(plan_is_wifi_single(plan) for plan in plan_samples)
        trace_summary = report["trace"]["summary"]
        cleanup_passed = report["cleanup_verify"]["exit"] == 0 and report["trace_output_cleanup"]["exit"] == 0
        report["summary"] = {
            "sample_count": len(report["samples"]),
            "web_errors": web_errors,
            "plans_stable_wifi_single": plans_stable,
            "qdisc_structure_unchanged": qdisc_unchanged,
            "video_passed": bool(report.get("video_result", {}).get("passed")),
            "trace_valid": trace_summary["parsed_lines"] > 0
            and trace_summary["valid_hcc_lines"] > 0
            and trace_summary["directions"] == ["DL", "UL"],
            "trace_cleanup_passed": cleanup_passed,
        }
        report["passed"] = all((
            not web_errors,
            plans_stable,
            qdisc_unchanged,
            report["summary"]["video_passed"],
            report["summary"]["trace_valid"],
            cleanup_passed,
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
        sta = clients.get("sta")
        if sta is not None:
            if marker_created:
                report["emergency_marker_cleanup"] = execute(sta, f"rm -f {TRACE_ENABLE}")
            if trace_created:
                report["emergency_trace_cleanup"] = execute(sta, f"rm -f {TRACE_OUTPUT}")
        for client in clients.values():
            client.close()
        report["finished_utc"] = utc_now()
        output_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(json.dumps({"output": str(output_path), "passed": report.get("passed"),
                      "samples": len(report.get("samples", [])), "error": report.get("error")},
                     ensure_ascii=False))
    raise SystemExit(0 if report.get("passed") else 1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Run one bounded LinkG Wi-Fi netem loss scenario without changing product source."""

import argparse
import json
import re
import socket
import threading
import time
from collections import defaultdict
from pathlib import Path

from switch_t0_baseline import (
    AP_HOST,
    STA_HOST,
    connect,
    execute,
    response_data,
    snapshot,
    utc_now,
    web_request,
)


RK_HOST = "172.28.2.3"
DESTINATION = "172.28.1.100"
TRACE_ENABLE = "/tmp/linkg_switch_trace.enable"
TRACE_OUTPUT = "/tmp/linkg_switch_wifi_250ms.log"
CLASSES = ("realtime", "video", "data")
PORTS = {"realtime": 14550, "video": 5600, "data": 6200}


def qdisc_structure(text):
    result = []
    for line in text.splitlines():
        match = re.match(r"qdisc\s+(\S+)\s+\S+:\s+(root|parent\s+\S+)", line)
        if match is None:
            continue
        relation = match.group(2)
        if relation.startswith("parent "):
            relation = "parent :" + relation.rsplit(":", 1)[-1]
        result.append((match.group(1), relation))
    return result


def qdisc_is_original(text):
    expected = [("mq", "root")]
    expected.extend(("fq_codel", f"parent :{index}") for index in range(8, 0, -1))
    return qdisc_structure(text) == expected


def qdisc_is_netem(text, loss_percent):
    return (
        qdisc_structure(text) == [("netem", "root")]
        and f"loss {loss_percent}%" in text
    )


def plan_from_switch(item, side):
    data = response_data(item)
    if data is None:
        return None
    if side == "sta":
        return data.get("sta", {}).get("plan")
    peers = data.get("ap", {}).get("peers", [])
    return peers[0].get("plan") if peers else None


def compact_plan(item, side):
    plan = plan_from_switch(item, side)
    classes = (plan or {}).get("classes", {})
    return {
        name: {
            "mode": classes.get(name, {}).get("mode"),
            "primary": classes.get(name, {}).get("primary", {}).get("access"),
            "secondary": classes.get(name, {}).get("secondary", {}).get("access"),
        }
        for name in CLASSES
    }


def plan_is_normal(item, side):
    plan = compact_plan(item, side)
    return all(
        plan[name] == {"mode": "single", "primary": "wifi", "secondary": "cellular"}
        for name in CLASSES
    )


def plan_signature(item, side):
    plan = compact_plan(item, side)
    return tuple((name, plan[name]["mode"], plan[name]["primary"], plan[name]["secondary"])
                 for name in CLASSES)


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
    for name, values in after.items():
        initial = before.get(name, {})
        result[name] = {key: value - initial.get(key, 0) for key, value in values.items()}
    return result


def loss_values(item, side):
    data = response_data(item)
    if data is None:
        return None
    if side == "ap":
        peers = data.get("ap", {}).get("peers", [])
        value = peers[0].get("wifi_uplink_loss") if peers else None
        return {"uplink": value}
    wifi = data.get("sta", {}).get("wifi", {})
    return {"uplink": wifi.get("uplink_loss"), "downlink": wifi.get("downlink_loss")}


class UdpReceiver(threading.Thread):
    def __init__(self, traffic_class, port, started_monotonic):
        super().__init__(name=f"udp-{traffic_class}", daemon=True)
        self.traffic_class = traffic_class
        self.port = port
        self.started_monotonic = started_monotonic
        self.stop_event = threading.Event()
        self.ready_event = threading.Event()
        self.error = None
        self.count = 0
        self.bytes = 0
        self.first_utc = None
        self.last_utc = None
        self.last_monotonic = None
        self.max_gap_ms = 0.0
        self.gaps = []
        self.buckets = defaultdict(lambda: {"packets": 0, "bytes": 0})

    def run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("0.0.0.0", self.port))
            sock.settimeout(0.2)
            self.ready_event.set()
            while not self.stop_event.is_set():
                try:
                    payload, _ = sock.recvfrom(65535)
                except socket.timeout:
                    continue
                now = time.monotonic()
                wall = utc_now()
                if self.first_utc is None:
                    self.first_utc = wall
                if self.last_monotonic is not None:
                    gap_ms = (now - self.last_monotonic) * 1000.0
                    self.max_gap_ms = max(self.max_gap_ms, gap_ms)
                    if gap_ms >= 100.0:
                        self.gaps.append({"ended_utc": wall, "gap_ms": round(gap_ms, 3)})
                self.last_monotonic = now
                self.last_utc = wall
                self.count += 1
                self.bytes += len(payload)
                second = int(now - self.started_monotonic)
                self.buckets[second]["packets"] += 1
                self.buckets[second]["bytes"] += len(payload)
        except Exception as exc:
            self.error = f"{type(exc).__name__}: {exc}"
            self.ready_event.set()
        finally:
            sock.close()

    def stop(self):
        self.stop_event.set()
        self.join(timeout=2)

    def result(self):
        return {
            "class": self.traffic_class,
            "port": self.port,
            "error": self.error,
            "packets": self.count,
            "bytes": self.bytes,
            "first_utc": self.first_utc,
            "last_utc": self.last_utc,
            "max_gap_ms": round(self.max_gap_ms, 3),
            "gaps_ge_100ms_count": len(self.gaps),
            "largest_gaps": sorted(self.gaps, key=lambda item: item["gap_ms"], reverse=True)[:20],
            "per_second": {str(key): self.buckets[key] for key in sorted(self.buckets)},
        }


def start_background(client, command, log_path):
    result = execute(client, f"sh -c '{command} >{log_path} 2>&1 & echo $!'", timeout=12)
    if result["exit"] != 0 or not result["stdout"].strip().isdigit():
        raise RuntimeError(f"failed to start background command: {result}")
    pid = int(result["stdout"].strip())
    time.sleep(0.3)
    check = execute(client, f"kill -0 {pid}", timeout=8)
    if check["exit"] != 0:
        log = execute(client, f"cat {log_path}", timeout=8)
        raise RuntimeError(f"background process {pid} exited early: {log}")
    return pid


def stop_background(client, pid):
    if pid is None:
        return None
    result = execute(client, f"kill -INT {pid} 2>/dev/null || true", timeout=8)
    for _ in range(20):
        check = execute(client, f"kill -0 {pid} 2>/dev/null", timeout=8)
        if check["exit"] != 0:
            return result
        time.sleep(0.1)
    execute(client, f"kill -TERM {pid} 2>/dev/null || true", timeout=8)
    return result


def fetch_file(client, remote, local):
    local.parent.mkdir(parents=True, exist_ok=True)
    sftp = client.open_sftp()
    try:
        sftp.get(remote, str(local))
    finally:
        sftp.close()


def read_current_log(client):
    return execute(
        client,
        "f=$(sed -n 's/^file=//p' /app/logger/current_session.info); "
        "printf 'FILE=%s\\n' \"$f\"; cat /app/logger/$f",
        timeout=15,
    )


def record_plan_events(samples):
    events = []
    previous = {"ap": None, "sta": None}
    for sample in samples:
        for side in ("ap", "sta"):
            item = sample.get(side, {}).get("switch")
            signature = plan_signature(item, side)
            if signature != previous[side]:
                events.append({
                    "captured_utc": sample["captured_utc"],
                    "elapsed_seconds": sample["elapsed_seconds"],
                    "phase": sample["phase"],
                    "side": side,
                    "plan": compact_plan(item, side),
                })
                previous[side] = signature
    return events


def sample_state(clients, phase, started_monotonic, full=False):
    sample = {
        "captured_utc": utc_now(),
        "elapsed_seconds": round(time.monotonic() - started_monotonic, 3),
        "phase": phase,
        "ap": {"switch": web_request(clients["ap"], 1003)},
        "sta": {"switch": web_request(clients["sta"], 1003)},
    }
    if full:
        sample["ap"]["wifi"] = web_request(clients["ap"], 1006)
        sample["ap"]["cellular"] = web_request(clients["ap"], 1009)
        sample["sta"]["wifi"] = web_request(clients["sta"], 1006)
        sample["sta"]["cellular"] = web_request(clients["sta"], 1009)
    sample["ap"]["loss"] = loss_values(sample["ap"]["switch"], "ap")
    sample["sta"]["loss"] = loss_values(sample["sta"]["switch"], "sta")
    return sample


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-id", required=True)
    parser.add_argument("--loss", type=int, required=True, choices=(1, 3, 5, 10, 100))
    parser.add_argument("--duration", type=int, default=60)
    parser.add_argument("--both", action="store_true")
    parser.add_argument("--password", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.duration < 10 or args.duration > 120:
        raise SystemExit("duration must be in [10, 120]")
    if args.loss == 100 and not args.both:
        raise SystemExit("100% scenario must use --both")

    output_path = Path(args.output).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    safe_id = re.sub(r"[^A-Za-z0-9_-]", "_", args.test_id)
    report = {
        "test_id": args.test_id,
        "started_utc": utc_now(),
        "loss_percent": args.loss,
        "duration_seconds": args.duration,
        "injection_sides": ["sta", "ap"] if args.both else ["sta"],
        "scope": "whole wlan0 root netem, explicitly authorized by user; product source unchanged",
        "samples": [],
        "tc_samples": [],
        "trace_samples": [],
    }
    clients = {}
    receivers = []
    rk_pids = {}
    tcpdump_pid = None
    attempted_qdisc = []
    trace_marker = False
    trace_output = False
    remote_pcap = f"/tmp/linkg_{safe_id}.pcap"
    remote_tcpdump_log = f"/tmp/linkg_{safe_id}_tcpdump.log"
    cleanup_ok = True
    started_monotonic = time.monotonic()

    try:
        clients["ap"] = connect(AP_HOST, args.password)
        clients["sta"] = connect(STA_HOST, args.password)
        clients["rk"] = connect(RK_HOST, args.password)

        report["before"] = {side: snapshot(clients[side]) for side in ("ap", "sta")}
        report["preflight_qdisc"] = {}
        for side in ("ap", "sta"):
            current_qdisc = execute(clients[side], "tc -d qdisc show dev wlan0", timeout=12)
            report["preflight_qdisc"][side] = current_qdisc
            qdisc_text = current_qdisc["stdout"]
            if not qdisc_is_original(qdisc_text):
                raise RuntimeError(f"{side} qdisc is not mq + 8 fq_codel")
            switch = report["before"][side]["switch"]
            if not plan_is_normal(switch, side):
                raise RuntimeError(f"{side} plan is not Wi-Fi primary/Cellular secondary")

        trace_preflight = execute(
            clients["sta"], f"test ! -e {TRACE_ENABLE} && test ! -e {TRACE_OUTPUT}", timeout=8
        )
        report["trace_preflight"] = trace_preflight
        if trace_preflight["exit"] != 0:
            raise RuntimeError("existing switch trace marker/output prevents safe run")
        report["trace_enable"] = execute(clients["sta"], f"touch {TRACE_ENABLE}", timeout=8)
        if report["trace_enable"]["exit"] != 0:
            raise RuntimeError("failed to enable switch trace")
        trace_marker = True

        for name, port in PORTS.items():
            receiver = UdpReceiver(name, port, started_monotonic)
            receiver.start()
            if not receiver.ready_event.wait(2) or receiver.error is not None:
                raise RuntimeError(f"UDP receiver {name} failed: {receiver.error}")
            receivers.append(receiver)

        tcpdump_command = (
            f"tcpdump -i eth0 -s 96 -U -n -w {remote_pcap} "
            f"\"udp and (port {PORTS['realtime']} or port {PORTS['video']} or port {PORTS['data']})\""
        )
        tcpdump_pid = start_background(clients["ap"], tcpdump_command, remote_tcpdump_log)

        source_commands = {
            "realtime": (
                "gst-launch-1.0 -q fakesrc is-live=true sizetype=2 sizemax=256 "
                f"datarate=32000 sync=true ! udpsink host={DESTINATION} "
                f"port={PORTS['realtime']} sync=false async=false"
            ),
            "video": (
                "gst-launch-1.0 -q v4l2src device=/dev/video51 ! "
                "video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! "
                "mpph264enc bps=6000000 rc-mode=1 gop=10 qp-init=30 zero-copy-pkt=true max-reenc=0 ! "
                "h264parse config-interval=-1 ! rtph264pay aggregate-mode=zero-latency pt=96 mtu=1300 ! "
                f"udpsink host={DESTINATION} port={PORTS['video']} sync=false async=false"
            ),
            "data": (
                "gst-launch-1.0 -q fakesrc is-live=true sizetype=2 sizemax=512 "
                f"datarate=64000 sync=true ! udpsink host={DESTINATION} "
                f"port={PORTS['data']} sync=false async=false"
            ),
        }
        for name, command in source_commands.items():
            log_path = f"/tmp/linkg_{safe_id}_{name}.log"
            rk_pids[name] = start_background(clients["rk"], command, log_path)

        baseline_deadline = time.monotonic() + 5.0
        index = 0
        while time.monotonic() < baseline_deadline:
            report["samples"].append(sample_state(
                clients, "baseline", started_monotonic, full=(index % 4 == 0)
            ))
            index += 1
            time.sleep(0.5)
        if any(receiver.count == 0 for receiver in receivers):
            raise RuntimeError("one or more UDP classes had no baseline receive packets")

        selected = ["sta", "ap"] if args.both else ["sta"]
        report["injection_started_utc"] = utc_now()
        report["injection_started_elapsed_seconds"] = round(time.monotonic() - started_monotonic, 3)
        for side in selected:
            attempted_qdisc.append(side)
            command = (
                "tc qdisc replace dev wlan0 root handle 7ffe: "
                f"netem delay 0ms loss random {args.loss}%"
            )
            install = execute(clients[side], command, timeout=12)
            report.setdefault("tc_install", {})[side] = install
            current = execute(clients[side], "tc -s -d qdisc show dev wlan0", timeout=12)
            report.setdefault("tc_after_install", {})[side] = current
            if install["exit"] != 0 or not qdisc_is_netem(current["stdout"], args.loss):
                raise RuntimeError(f"{side} failed to install requested netem loss")

        loss_deadline = time.monotonic() + args.duration
        index = 0
        next_tc = time.monotonic()
        next_trace = time.monotonic()
        while time.monotonic() < loss_deadline:
            cycle = time.monotonic()
            report["samples"].append(sample_state(
                clients, "loss", started_monotonic, full=(index % 4 == 0)
            ))
            index += 1
            now = time.monotonic()
            if now >= next_tc:
                report["tc_samples"].append({
                    "captured_utc": utc_now(),
                    "elapsed_seconds": round(now - started_monotonic, 3),
                    "boards": {
                        side: execute(clients[side], "tc -s -d qdisc show dev wlan0", timeout=10)
                        for side in selected
                    },
                })
                next_tc = now + 5.0
            if now >= next_trace:
                trace = execute(
                    clients["sta"], f"test -f {TRACE_OUTPUT} && cat {TRACE_OUTPUT}", timeout=10
                )
                report["trace_samples"].append({
                    "captured_utc": utc_now(),
                    "elapsed_seconds": round(now - started_monotonic, 3),
                    "result": trace,
                })
                next_trace = now + 1.0
            delay = 0.25 - (time.monotonic() - cycle)
            if delay > 0:
                time.sleep(delay)

        report["injection_restore_started_utc"] = utc_now()
        report["injection_restore_started_elapsed_seconds"] = round(
            time.monotonic() - started_monotonic, 3
        )
        for side in reversed(attempted_qdisc):
            restore = execute(clients[side], "tc qdisc replace dev wlan0 root mq", timeout=12)
            report.setdefault("tc_restore", {})[side] = restore
        time.sleep(1.0)
        for side in selected:
            current = execute(clients[side], "tc -s -d qdisc show dev wlan0", timeout=12)
            report.setdefault("tc_after_restore", {})[side] = current
            if not qdisc_is_original(current["stdout"]):
                raise RuntimeError(f"{side} qdisc did not restore to mq + 8 fq_codel")
        attempted_qdisc.clear()
        report["injection_restored_utc"] = utc_now()
        report["injection_actual_seconds"] = round(
            time.monotonic() - started_monotonic
            - report["injection_started_elapsed_seconds"], 3
        )

        recovery_deadline = time.monotonic() + 60.0
        consecutive_normal = 0
        index = 0
        while time.monotonic() < recovery_deadline:
            sample = sample_state(clients, "recovery", started_monotonic, full=(index % 2 == 0))
            report["samples"].append(sample)
            index += 1
            if plan_is_normal(sample["ap"]["switch"], "ap") and plan_is_normal(
                sample["sta"]["switch"], "sta"
            ):
                consecutive_normal += 1
            else:
                consecutive_normal = 0
            if consecutive_normal >= 4:
                report["recovery_completed_utc"] = sample["captured_utc"]
                report["recovery_completed_elapsed_seconds"] = sample["elapsed_seconds"]
                break
            time.sleep(0.5)
        report["recovery_normal_confirmed"] = consecutive_normal >= 4

        report["after"] = {side: snapshot(clients[side]) for side in ("ap", "sta")}
        report["logs"] = {side: read_current_log(clients[side]) for side in ("ap", "sta")}

    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        for side in reversed(attempted_qdisc):
            if side in clients:
                restore = execute(clients[side], "tc qdisc replace dev wlan0 root mq", timeout=12)
                report.setdefault("emergency_tc_restore", {})[side] = restore
                time.sleep(0.5)
                current = execute(clients[side], "tc -s -d qdisc show dev wlan0", timeout=12)
                report.setdefault("emergency_tc_after_restore", {})[side] = current
                cleanup_ok = cleanup_ok and qdisc_is_original(current["stdout"])

        if "rk" in clients:
            for name, pid in rk_pids.items():
                report.setdefault("source_stop", {})[name] = stop_background(clients["rk"], pid)
                log_path = f"/tmp/linkg_{safe_id}_{name}.log"
                try:
                    fetch_file(clients["rk"], log_path, output_path.with_name(f"{safe_id}_{name}.log"))
                except Exception as exc:
                    report.setdefault("source_log_errors", {})[name] = f"{type(exc).__name__}: {exc}"
                execute(clients["rk"], f"rm -f {log_path}", timeout=8)

        for receiver in receivers:
            receiver.stop()
        report["receivers"] = {receiver.traffic_class: receiver.result() for receiver in receivers}

        if "ap" in clients and tcpdump_pid is not None:
            report["tcpdump_stop"] = stop_background(clients["ap"], tcpdump_pid)
            time.sleep(0.5)
            report["tcpdump_log"] = execute(clients["ap"], f"cat {remote_tcpdump_log}", timeout=8)
            try:
                fetch_file(clients["ap"], remote_pcap, output_path.with_suffix(".pcap"))
                report["pcap"] = str(output_path.with_suffix(".pcap"))
            except Exception as exc:
                report["pcap_error"] = f"{type(exc).__name__}: {exc}"
            execute(clients["ap"], f"rm -f {remote_pcap} {remote_tcpdump_log}", timeout=8)

        if "sta" in clients:
            if trace_marker:
                report["trace_disable"] = execute(clients["sta"], f"rm -f {TRACE_ENABLE}", timeout=8)
                trace_marker = False
                time.sleep(1.5)
            trace = execute(clients["sta"], f"test -f {TRACE_OUTPUT} && cat {TRACE_OUTPUT}", timeout=12)
            if trace["exit"] == 0:
                trace_output = True
                trace_path = output_path.with_name(f"{safe_id}_switch_trace.log")
                trace_path.write_text(trace["stdout"], encoding="utf-8")
                report["trace"] = {"path": str(trace_path), "lines": len(trace["stdout"].splitlines())}
            if trace_output:
                report["trace_cleanup"] = execute(clients["sta"], f"rm -f {TRACE_OUTPUT}", timeout=8)
            verify = execute(
                clients["sta"], f"test ! -e {TRACE_ENABLE} && test ! -e {TRACE_OUTPUT}", timeout=8
            )
            report["trace_cleanup_verify"] = verify
            cleanup_ok = cleanup_ok and verify["exit"] == 0

        if "before" in report and "after" in report:
            report["interface_delta"] = {}
            for side in ("ap", "sta"):
                before = parse_interfaces(report["before"][side]["interfaces"]["stdout"])
                after = parse_interfaces(report["after"][side]["interfaces"]["stdout"])
                report["interface_delta"][side] = interface_delta(before, after)

        report["plan_events"] = record_plan_events(report["samples"])
        report["cleanup_ok"] = cleanup_ok
        report["execution_passed"] = (
            "error" not in report
            and cleanup_ok
            and report.get("recovery_normal_confirmed") is True
            and len(report.get("receivers", {})) == 3
            and all(item.get("packets", 0) > 0 and item.get("error") is None
                    for item in report.get("receivers", {}).values())
        )
        report["finished_utc"] = utc_now()
        output_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        for client in clients.values():
            client.close()

    print(json.dumps({
        "output": str(output_path),
        "execution_passed": report.get("execution_passed"),
        "error": report.get("error"),
        "samples": len(report.get("samples", [])),
        "plan_events": len(report.get("plan_events", [])),
        "recovered": report.get("recovery_normal_confirmed"),
        "receiver_packets": {
            name: item.get("packets") for name, item in report.get("receivers", {}).items()
        },
    }, ensure_ascii=False))
    raise SystemExit(0 if report.get("execution_passed") else 1)


if __name__ == "__main__":
    main()

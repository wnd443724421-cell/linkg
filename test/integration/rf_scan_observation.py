#!/usr/bin/env python3
"""Bounded, non-configuration-changing survey/scan observation for LinkG."""

import argparse
import json
import re
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

from switch_t0_baseline import connect, execute


AP_HOST = "192.168.1.1"
STA_HOST = "192.168.3.1"


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def command_snapshot(client):
    return {
        "captured_utc": utc_now(),
        "info": execute(client, "iw dev wlan0 info 2>&1", 10),
        "link": execute(client, "iw dev wlan0 link 2>&1", 10),
        "survey": execute(client, "iw dev wlan0 survey dump 2>&1", 15),
        "operstate": execute(
            client,
            "cat /sys/class/net/wlan0/operstate; "
            "cat /sys/class/net/wlan0/carrier; "
            "ip -4 addr show dev wlan0",
            10,
        ),
        "iw_pids": execute(client, "pidof iw 2>/dev/null || true", 5),
    }


def bounded_scan_command(label):
    output = f"/tmp/linkg_rf_{label}_scan.out"
    pidfile = f"/tmp/linkg_rf_{label}_scan.pid"
    return (
        f"out={output}; pidf={pidfile}; rm -f $out $pidf; "
        "iw dev wlan0 scan >$out 2>&1 & p=$!; echo $p >$pidf; "
        "i=0; while kill -0 $p 2>/dev/null && [ $i -lt 3 ]; do "
        "sleep 1; i=$((i+1)); done; timed=0; "
        "if kill -0 $p 2>/dev/null; then timed=1; kill $p 2>/dev/null; "
        "sleep 1; kill -9 $p 2>/dev/null; fi; "
        "wait $p; rc=$?; cat $out; "
        "echo __LINKG_SCAN_RC=$rc; echo __LINKG_SCAN_TIMED_OUT=$timed; "
        "rm -f $out $pidf"
    )


def parse_ping(text):
    match = re.search(
        r"(\d+) packets transmitted, (\d+) packets received, "
        r"(\d+(?:\.\d+)?)% packet loss",
        text,
    )
    if not match:
        return {"parsed": False, "raw_tail": text[-1000:]}
    transmitted = int(match.group(1))
    received = int(match.group(2))
    loss = float(match.group(3))
    return {
        "parsed": True,
        "transmitted": transmitted,
        "received": received,
        "lost": transmitted - received,
        "loss_percent": loss,
    }


def parse_link(text):
    connected = "Connected to " in text
    bssid = re.search(r"Connected to ([0-9a-f:]+)", text, re.I)
    frequency = re.search(r"\bfreq:\s*(\d+)", text)
    signal = re.search(r"\bsignal:\s*(-?\d+(?:\.\d+)?)\s*dBm", text)
    return {
        "connected": connected,
        "bssid": bssid.group(1) if bssid else None,
        "frequency_mhz": int(frequency.group(1)) if frequency else None,
        "signal_dbm": float(signal.group(1)) if signal else None,
        "not_connected_text": "Not connected." in text,
    }


def parse_scan(text):
    return {
        "bss_count": len(re.findall(r"^BSS ", text, re.M)),
        "frequencies_mhz": sorted({int(v) for v in re.findall(r"^\s*freq:\s*(\d+)", text, re.M)}),
        "signals_dbm": [float(v) for v in re.findall(r"^\s*signal:\s*(-?\d+(?:\.\d+)?)\s*dBm", text, re.M)],
        "ssids": [v.strip() for v in re.findall(r"^\s*SSID:\s*(.*)$", text, re.M)],
        "return_code": int(re.search(r"__LINKG_SCAN_RC=(-?\d+)", text).group(1))
        if re.search(r"__LINKG_SCAN_RC=(-?\d+)", text)
        else None,
        "timed_out": re.search(r"__LINKG_SCAN_TIMED_OUT=(\d+)", text).group(1) == "1"
        if re.search(r"__LINKG_SCAN_TIMED_OUT=(\d+)", text)
        else None,
    }


def scan_with_ping(label, scan_client, ping_client, ping_source, ping_target):
    before = {
        "scan_side_link": execute(scan_client, "iw dev wlan0 link 2>&1", 10),
        "ping_side_link": execute(ping_client, "iw dev wlan0 link 2>&1", 10),
    }
    ping_command = f"ping -I {ping_source} -c 200 -i 0.1 -W 1 {ping_target}"
    with ThreadPoolExecutor(max_workers=1) as pool:
        ping_future = pool.submit(execute, ping_client, ping_command, 30)
        time.sleep(1.0)
        started_utc = utc_now()
        started = time.monotonic()
        scan = execute(scan_client, bounded_scan_command(label), 25)
        scan_duration = time.monotonic() - started
        finished_utc = utc_now()
        ping = ping_future.result(timeout=35)
    immediate = {
        "scan_side_link": execute(scan_client, "iw dev wlan0 link 2>&1", 10),
        "ping_side_link": execute(ping_client, "iw dev wlan0 link 2>&1", 10),
    }
    time.sleep(2.0)
    settled = {
        "scan_side_link": execute(scan_client, "iw dev wlan0 link 2>&1", 10),
        "ping_side_link": execute(ping_client, "iw dev wlan0 link 2>&1", 10),
    }
    return {
        "label": label,
        "scan_started_utc": started_utc,
        "scan_finished_utc": finished_utc,
        "scan_duration_seconds": scan_duration,
        "before": before,
        "scan": scan,
        "scan_summary": parse_scan(scan.get("stdout", "")),
        "ping": ping,
        "ping_summary": parse_ping(ping.get("stdout", "")),
        "immediate_after": immediate,
        "settled_after_2s": settled,
        "link_summary": {
            "before": parse_link(before["scan_side_link"].get("stdout", "")),
            "immediate": parse_link(immediate["scan_side_link"].get("stdout", "")),
            "settled": parse_link(settled["scan_side_link"].get("stdout", "")),
        },
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    clients = {}
    report = {"started_utc": utc_now(), "passed": False}
    try:
        clients["ap"] = connect(AP_HOST, args.password)
        clients["sta"] = connect(STA_HOST, args.password)
        report["baseline"] = {
            "ap": command_snapshot(clients["ap"]),
            "sta": command_snapshot(clients["sta"]),
        }
        report["sta_active_scan"] = scan_with_ping(
            "sta", clients["sta"], clients["ap"], "wlan0", "11.21.191.2"
        )
        report["ap_active_scan"] = scan_with_ping(
            "ap", clients["ap"], clients["sta"], "wlan0", "11.21.191.1"
        )
        report["final"] = {
            "ap": command_snapshot(clients["ap"]),
            "sta": command_snapshot(clients["sta"]),
        }
        report["passed"] = all(
            report[key]["scan_summary"]["timed_out"] is False
            and report[key]["ping_summary"].get("received", 0) > 0
            for key in ("sta_active_scan", "ap_active_scan")
        )
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        for client in clients.values():
            try:
                execute(
                    client,
                    "for f in /tmp/linkg_rf_*_scan.pid; do "
                    "[ -f $f ] || continue; p=$(cat $f); kill $p 2>/dev/null; "
                    "rm -f $f ${f%.pid}.out; done; pidof iw 2>/dev/null || true",
                    10,
                )
            except Exception:
                pass
            client.close()
        report["finished_utc"] = utc_now()
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "output": str(output),
        "passed": report.get("passed"),
        "error": report.get("error"),
        "sta_scan": report.get("sta_active_scan", {}).get("scan_summary"),
        "sta_ping": report.get("sta_active_scan", {}).get("ping_summary"),
        "ap_scan": report.get("ap_active_scan", {}).get("scan_summary"),
        "ap_ping": report.get("ap_active_scan", {}).get("ping_summary"),
    }, ensure_ascii=False))
    raise SystemExit(0 if report.get("passed") else 1)


if __name__ == "__main__":
    main()

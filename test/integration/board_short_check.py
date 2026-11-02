#!/usr/bin/env python3
"""Collect bounded LinkG/RK board evidence without changing board state."""

import argparse
import json
import socket
import time
from datetime import datetime, timezone

import paramiko


BOARD_COMMANDS = {
    "identity": "uname -a; printf '\\n'; cat /proc/cmdline 2>/dev/null || true",
    "clock_uptime": "date -Iseconds 2>/dev/null || date; uptime",
    "release": "readlink -f /app/current 2>/dev/null || true; "
               "sha256sum /app/current/bin/linkg 2>/dev/null || true; "
               "ls -ld /app/current /app/releases/* 2>/dev/null || true",
    "processes": "pidof supervisor 2>/dev/null || true; pidof linkg 2>/dev/null || true; "
                 "ps w | grep -E '[s]upervisor|[/]linkg( |$)|[l]inkg_watchdog' || true",
    "network": "ip addr show 2>/dev/null || ifconfig -a; printf '\\nROUTES\\n'; ip route show 2>/dev/null || route -n",
    "wifi_info": "iw dev wlan0 info 2>&1 || true; printf '\\nLINK\\n'; iw dev wlan0 link 2>&1 || true",
    "listeners": "ss -lntup 2>/dev/null || netstat -lntup 2>/dev/null || true",
    "interfaces": "for n in /sys/class/net/*; do i=${n##*/}; "
                  "printf '%s ' \"$i\"; cat \"$n/statistics/rx_bytes\" \"$n/statistics/tx_bytes\" "
                  "\"$n/statistics/rx_errors\" \"$n/statistics/tx_errors\" 2>/dev/null | tr '\\n' ' '; "
                  "printf '\\n'; done",
    "resources": "p=$(pidof linkg 2>/dev/null | awk '{print $1}'); "
                 "if [ -n \"$p\" ]; then grep -E '^(Name|State|VmRSS|VmSize|Threads):' /proc/$p/status; "
                 "printf 'fd_count='; ls /proc/$p/fd 2>/dev/null | wc -l; fi; "
                 "grep -E '^(MemTotal|MemFree|MemAvailable):' /proc/meminfo",
    "config_candidates": "for f in /app/current/etc/linkgUser.json /app/config/linkgUser.json "
                         "/etc/linkg/linkgUser.json; do if [ -f \"$f\" ]; then echo FILE=$f; cat \"$f\"; fi; done",
    "recent_logs": "for f in /tmp/linkg.log /tmp/linkg_watchdog.log /var/log/linkg.log; do "
                   "if [ -f \"$f\" ]; then echo FILE=$f; tail -n 80 \"$f\"; fi; done; "
                   "dmesg 2>/dev/null | tail -n 60",
}

RK_COMMANDS = {
    "identity": "uname -a; printf '\\n'; cat /proc/cmdline 2>/dev/null || true",
    "clock_uptime": "date -Iseconds 2>/dev/null || date; uptime",
    "network": "ip addr show 2>/dev/null || ifconfig -a; printf '\\nROUTES\\n'; ip route show 2>/dev/null || route -n",
    "camera": "ls -l /dev/video51 2>&1; "
              "v4l2-ctl -d /dev/video51 --get-fmt-video 2>&1 || true; "
              "gst-launch-1.0 --version 2>&1 | head -n 3",
    "processes": "ps w | grep -E '[g]st-launch|[u]dhcp|[d]nsmasq' || true",
    "interfaces": BOARD_COMMANDS["interfaces"],
    "resources": "grep -E '^(MemTotal|MemFree|MemAvailable):' /proc/meminfo",
    "recent_logs": "dmesg 2>/dev/null | tail -n 60",
}

WEB_GET_COMMANDS = (1001, 1002, 1003, 1004, 1006, 1007, 1009, 1010, 1012, 1014, 1015, 1018, 1022, 1026)


def connect(host, password):
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(host, username="root", password=password, timeout=8, auth_timeout=8, banner_timeout=8)
    return client


def execute(client, command):
    try:
        stdin, stdout, stderr = client.exec_command(command, timeout=12)
        del stdin
        stdout.channel.settimeout(12)
        output = stdout.read().decode("utf-8", "replace")
        error = stderr.read().decode("utf-8", "replace")
        return {"exit": stdout.channel.recv_exit_status(), "stdout": output, "stderr": error}
    except Exception as exc:
        return {"error": f"{type(exc).__name__}: {exc}"}


def web_request(client, command, param=None):
    request = {"cmd": command}
    if param is not None:
        request["param"] = param
    payload = json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode()
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
        try:
            return {"response": json.loads(raw), "raw": raw}
        except json.JSONDecodeError as exc:
            return {"error": f"invalid JSON response: {exc}", "raw": raw}
    except Exception as exc:
        return {"error": f"{type(exc).__name__}: {exc}"}
    finally:
        if channel is not None:
            channel.close()


def collect(client, commands, include_web):
    result = {name: execute(client, command) for name, command in commands.items()}
    if include_web:
        result["web"] = {str(command): web_request(client, command) for command in WEB_GET_COMMANDS}
        result["web_invalid"] = {
            "unknown_command": web_request(client, 999999),
            "invalid_uart_diag_job": web_request(client, 1026, {"job_id": 4294967295}),
        }
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--interval", type=int, default=180)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    targets = {
        "linkg_ap": ("192.168.1.1", BOARD_COMMANDS, True),
        "linkg_sta": ("192.168.3.1", BOARD_COMMANDS, True),
        "rk_camera": ("172.28.2.3", RK_COMMANDS, False),
    }
    report = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "interval_seconds": args.interval,
        "snapshots": [],
    }
    clients = {}
    try:
        for name, (host, _, _) in targets.items():
            try:
                clients[name] = connect(host, args.password)
            except Exception as exc:
                clients[name] = None
                report.setdefault("connection_errors", {})[name] = f"{type(exc).__name__}: {exc}"

        for index in range(2):
            snapshot = {
                "index": index,
                "captured_utc": datetime.now(timezone.utc).isoformat(),
                "targets": {},
            }
            for name, (_, commands, include_web) in targets.items():
                client = clients.get(name)
                if client is None:
                    snapshot["targets"][name] = {"error": "not connected"}
                else:
                    snapshot["targets"][name] = collect(client, commands, include_web)
            report["snapshots"].append(snapshot)
            if index == 0 and args.interval > 0:
                time.sleep(args.interval)
    finally:
        for client in clients.values():
            if client is not None:
                client.close()

    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    with open(args.output, "w", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
        output.write("\n")
    print(json.dumps({"output": args.output, "snapshots": len(report["snapshots"]), "connection_errors": report.get("connection_errors", {})}))


if __name__ == "__main__":
    main()

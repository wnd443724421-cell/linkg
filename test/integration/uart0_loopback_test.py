#!/usr/bin/env python3
"""Run the production UART0 diagnostic and verify automatic service restoration."""

import argparse
import json
import time
from datetime import datetime, timezone

import paramiko


TERMINAL_STATES = {"completed", "cancelled", "failed"}


def connect(host, password):
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(host, username="root", password=password, timeout=8, auth_timeout=8, banner_timeout=8)
    return client


def request(client, command, param=None):
    message = {"cmd": command}
    if param is not None:
        message["param"] = param
    payload = json.dumps(message, separators=(",", ":")).encode()
    channel = client.get_transport().open_channel(
        "direct-tcpip", ("127.0.0.1", 29608), ("127.0.0.1", 0), timeout=5
    )
    try:
        channel.settimeout(5)
        channel.sendall(payload)
        channel.shutdown_write()
        chunks = []
        while True:
            chunk = channel.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
        return json.loads(b"".join(chunks).decode("utf-8", "replace"))
    finally:
        channel.close()


def run_target(host, password):
    client = connect(host, password)
    try:
        result = {
            "host": host,
            "started_utc": datetime.now(timezone.utc).isoformat(),
            "uart_status_before": request(client, 1014),
            "invalid_start": request(client, 1025, {"mode": "single", "uart_num": 3}),
        }
        start = request(client, 1025, {"mode": "single", "uart_num": 0})
        result["start"] = start
        if start.get("code") != "ok":
            result["passed"] = False
            result["failure"] = "diagnostic start rejected"
            return result

        job_id = int(start["data"]["job_id"])
        deadline = time.monotonic() + 20.0
        polls = []
        final = None
        while time.monotonic() < deadline:
            status = request(client, 1026, {"job_id": job_id})
            polls.append(status)
            if status.get("code") == "ok" and status.get("data", {}).get("state") in TERMINAL_STATES:
                final = status
                break
            time.sleep(0.1)

        result["poll_count"] = len(polls)
        result["final"] = final if final is not None else polls[-1] if polls else None
        result["uart_status_after"] = request(client, 1014)
        result["finished_utc"] = datetime.now(timezone.utc).isoformat()

        data = (result["final"] or {}).get("data", {})
        tests = data.get("tests", [])
        before = result["uart_status_before"].get("data", {})
        after = result["uart_status_after"].get("data", {})
        result["passed"] = (
            data.get("state") == "completed"
            and data.get("tests_passed") is True
            and data.get("uart_restored") is True
            and len(tests) > 0
            and all(item.get("state") == "passed" for item in tests)
            and before.get("state") == after.get("state")
            and before.get("worker_running") == after.get("worker_running")
        )
        if not result["passed"]:
            result["failure"] = "loopback data or UART service restoration check failed"
        return result
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    report = {
        "test": "production UART0 diagnostic",
        "targets": [
            run_target("192.168.1.1", args.password),
            run_target("192.168.3.1", args.password),
        ],
    }
    report["passed"] = all(target.get("passed") for target in report["targets"])
    with open(args.output, "w", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
        output.write("\n")
    print(json.dumps({"output": args.output, "passed": report["passed"],
                      "targets": [{"host": item["host"], "passed": item.get("passed")} for item in report["targets"]]}))
    raise SystemExit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()

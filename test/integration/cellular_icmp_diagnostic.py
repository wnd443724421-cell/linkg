#!/usr/bin/env python3
"""Compare LinkG Cellular Probe behavior with direct bidirectional IPv6 ICMP."""

import argparse
import json
from pathlib import Path

from switch_t0_baseline import AP_HOST, STA_HOST, connect, utc_now, web_request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--count", type=int, default=60)
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.count < 1 or args.count > 300:
        raise SystemExit("count must be in [1, 300]")
    if args.interval < 1.0 or args.interval > 2.0 or not args.interval.is_integer():
        raise SystemExit("this BusyBox target requires an integer interval in [1, 2]")

    output_path = Path(args.output).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    report = {"test_id": "SW-CELLULAR-ICMP-DIAGNOSTIC", "started_utc": utc_now()}
    clients = {}
    try:
        clients["ap"] = connect(AP_HOST, args.password)
        clients["sta"] = connect(STA_HOST, args.password)
        report["status_before"] = {side: web_request(client, 1009) for side, client in clients.items()}
        addresses = {
            side: item["response"]["data"]["data"]["ipv6"]
            for side, item in report["status_before"].items()
        }
        report["addresses"] = addresses
        interval = int(args.interval)
        commands = {
            "ap": f"ping6 -I usb0 -c {args.count} -i {interval} -W 1 {addresses['sta']}",
            "sta": f"ping6 -I usb0 -c {args.count} -i {interval} -W 1 {addresses['ap']}",
        }
        report["commands"] = commands
        channels = {}
        timeout = int(args.count * args.interval + 20)
        for side, client in clients.items():
            stdin, stdout, stderr = client.exec_command(commands[side], timeout=timeout)
            stdin.close()
            stdout.channel.settimeout(timeout)
            channels[side] = (stdout, stderr)
        report["results"] = {}
        for side, (stdout, stderr) in channels.items():
            output = stdout.read().decode("utf-8", "replace")
            error = stderr.read().decode("utf-8", "replace")
            report["results"][side] = {
                "exit": stdout.channel.recv_exit_status(),
                "stdout": output,
                "stderr": error,
            }
        report["status_after"] = {side: web_request(client, 1009) for side, client in clients.items()}
        report["completed"] = True
    except Exception as exc:
        report["completed"] = False
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        for client in clients.values():
            client.close()
        report["finished_utc"] = utc_now()
        output_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(json.dumps({
        "output": str(output_path),
        "completed": report.get("completed"),
        "results": {
            side: {"exit": item["exit"], "tail": item["stdout"].splitlines()[-4:]}
            for side, item in report.get("results", {}).items()
        },
        "error": report.get("error"),
    }, ensure_ascii=False))
    raise SystemExit(0 if report.get("completed") else 1)


if __name__ == "__main__":
    main()

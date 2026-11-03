#!/usr/bin/env python3
"""Collect a non-iw snapshot after a Wi-Fi noise-value freeze."""

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path

from switch_t0_baseline import connect, execute, web_request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    sta = connect("192.168.3.1", args.password)
    report = {"captured_utc": datetime.now(timezone.utc).isoformat()}
    try:
        report["web_samples"] = [web_request(sta, 1006) for _ in range(3)]
        report["uptime"] = execute(sta, "cat /proc/uptime", 5)
        report["processes"] = execute(sta, "ps -ww", 10)
        report["linkg_fds"] = execute(
            sta,
            "for p in $(pidof linkg 2>/dev/null); do ls -l /proc/$p/fd 2>/dev/null; done",
            8,
        )
        report["log_listing"] = execute(
            sta,
            "ls -la /tmp /var/log /var/run/linkg /var/run/linkg/wifi 2>/dev/null",
            10,
        )
        report["iw_pids"] = execute(sta, "pidof iw 2>/dev/null || true", 5)
        report["dmesg_tail"] = execute(sta, "dmesg | tail -n 600", 20)
        report["proc_interrupts"] = execute(sta, "cat /proc/interrupts", 10)
    finally:
        sta.close()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "captured_utc": report["captured_utc"]}), flush=True)


if __name__ == "__main__":
    main()

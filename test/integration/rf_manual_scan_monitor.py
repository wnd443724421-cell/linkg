#!/usr/bin/env python3
"""Observe one user-triggered scan without starting a scan itself."""

import argparse
import json
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

from switch_t0_baseline import connect, execute, response_data, web_request


def now():
    return datetime.now(timezone.utc).isoformat()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--duration", type=int, default=120)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    ap = connect("192.168.1.1", args.password)
    sta = connect("192.168.3.1", args.password)
    report = {"started_utc": now(), "duration_seconds": args.duration, "samples": []}
    print(json.dumps({"ready_utc": report["started_utc"], "duration": args.duration}), flush=True)
    try:
        with ThreadPoolExecutor(max_workers=1) as pool:
            ping_future = pool.submit(
                execute,
                ap,
                f"ping -I wlan0 -c {args.duration * 10} -i 0.1 -W 1 11.21.191.2",
                args.duration + 20,
            )
            started = time.monotonic()
            next_process_check = 0.0
            while time.monotonic() - started < args.duration:
                tick = time.monotonic()
                item = {"elapsed_seconds": tick - started, "utc": now()}
                web = web_request(sta, 1006)
                data = response_data(web)
                item["web_ok"] = data is not None
                if data is not None:
                    local = data.get("local", {})
                    radio = local.get("radio", {})
                    peer = data.get("sta", {}).get("peer", {})
                    item.update({
                        "updated_ms": local.get("updated_ms"),
                        "interface_state": local.get("interface_state"),
                        "frequency_mhz": radio.get("frequency_mhz"),
                        "noise_valid": radio.get("noise_valid"),
                        "noise_dbm": radio.get("noise_dbm"),
                        "rssi_dbm": peer.get("rssi_dbm"),
                        "connected_time_s": peer.get("connected_time_s"),
                    })
                else:
                    item["web_error"] = web.get("error") or web.get("raw")
                elapsed = time.monotonic() - started
                if elapsed >= next_process_check:
                    status = execute(
                        sta,
                        "printf 'IW='; pidof iw 2>/dev/null || true; "
                        "iw dev wlan0 link 2>&1 | sed -n '1,8p'",
                        8,
                    )
                    item["iw_status"] = status
                    next_process_check = elapsed + 1.0
                report["samples"].append(item)
                delay = 0.25 - (time.monotonic() - tick)
                if delay > 0:
                    time.sleep(delay)
            report["ping"] = ping_future.result(timeout=args.duration + 30)
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        report["final_sta"] = {
            "web": web_request(sta, 1006),
            "link": execute(sta, "iw dev wlan0 link 2>&1", 10),
            "iw_pids": execute(sta, "pidof iw 2>/dev/null || true", 5),
        }
        report["finished_utc"] = now()
        ap.close()
        sta.close()
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "output": str(output),
        "samples": len(report["samples"]),
        "error": report.get("error"),
        "finished_utc": report["finished_utc"],
    }, ensure_ascii=False), flush=True)
    raise SystemExit(1 if report.get("error") else 0)


if __name__ == "__main__":
    main()

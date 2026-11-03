#!/usr/bin/env python3
"""Observe LinkG Wi-Fi status cache and optionally add one get_station caller.

The baseline mode deliberately does not execute iw, scan, ping or iperf.
"""

import argparse
import json
import time
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

from switch_t0_baseline import connect, execute, response_data, web_request


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def summarize(samples):
    valid = [item for item in samples if item.get("web_ok")]
    noises = [item.get("noise_dbm") for item in valid]
    rssis = [item.get("rssi_dbm") for item in valid]
    updated = [item.get("updated_ms") for item in valid]
    noise_transitions = sum(a != b for a, b in zip(noises, noises[1:]))
    updated_transitions = sum(a != b for a, b in zip(updated, updated[1:]))
    trailing_same = 0
    if noises:
        last = noises[-1]
        for value in reversed(noises):
            if value != last:
                break
            trailing_same += 1
    return {
        "sample_count": len(samples),
        "valid_sample_count": len(valid),
        "web_failure_count": len(samples) - len(valid),
        "noise_counts": dict(Counter(noises)),
        "noise_min": min(noises) if noises else None,
        "noise_max": max(noises) if noises else None,
        "noise_transition_count": noise_transitions,
        "trailing_same_noise_samples": trailing_same,
        "trailing_same_noise_seconds_approx": trailing_same * args_global.interval,
        "rssi_min": min(rssis) if rssis else None,
        "rssi_max": max(rssis) if rssis else None,
        "updated_ms_transition_count": updated_transitions,
    }


def main():
    global args_global
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--duration", type=float, default=90.0)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--output", required=True)
    parser.add_argument(
        "--trigger",
        choices=("none", "link", "station_get", "pm_off", "pm_cancel_timer"),
        default="none",
    )
    parser.add_argument("--trigger-at", type=float, default=30.0)
    parser.add_argument("--peer-mac", default="00:01:02:f7:da:4e")
    args = parser.parse_args()
    args_global = args

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    sta = connect("192.168.3.1", args.password)
    report = {
        "started_utc": utc_now(),
        "duration_seconds": args.duration,
        "interval_seconds": args.interval,
        "trigger": args.trigger,
        "trigger_at_seconds": args.trigger_at if args.trigger != "none" else None,
        "samples": [],
    }
    print(json.dumps({
        "ready_utc": report["started_utc"],
        "duration": args.duration,
        "interval": args.interval,
        "trigger": args.trigger,
    }), flush=True)

    try:
        report["preflight"] = execute(
            sta,
            "printf 'uptime='; cat /proc/uptime; "
            "printf 'iw_pids='; pidof iw 2>/dev/null || true",
            8,
        )
        started = time.monotonic()
        trigger_done = args.trigger == "none"
        while time.monotonic() - started < args.duration:
            tick = time.monotonic()
            elapsed = tick - started

            if not trigger_done and elapsed >= args.trigger_at:
                if args.trigger == "link":
                    command = "iw dev wlan0 link"
                elif args.trigger == "station_get":
                    command = f"iw dev wlan0 station get {args.peer_mac}"
                elif args.trigger == "pm_off":
                    command = (
                        "iw dev wlan0 set power_save off; "
                        "printf 'wlan0 set_sta_pm_on 0\\n' > /sys/hisys/hipriv"
                    )
                else:
                    command = (
                        "printf 'wlan0 set_sta_pm_on 1\\n' > /sys/hisys/hipriv; "
                        "sleep 1; "
                        "printf 'wlan0 set_sta_pm_on 0\\n' > /sys/hisys/hipriv; "
                        "iw dev wlan0 set power_save off"
                    )
                report["trigger_event"] = {
                    "elapsed_seconds": elapsed,
                    "utc": utc_now(),
                    "command": command,
                    "result": execute(sta, command, 10),
                }
                print(json.dumps({
                    "triggered_at": elapsed,
                    "command": command,
                }), flush=True)
                trigger_done = True

            item = {"elapsed_seconds": elapsed, "utc": utc_now()}
            web = web_request(sta, 1006)
            data = response_data(web)
            item["web_ok"] = data is not None
            if data is None:
                item["web_error"] = web.get("error") or web.get("raw")
            else:
                local = data.get("local", {})
                radio = local.get("radio", {})
                peer = data.get("sta", {}).get("peer", {})
                driver = peer.get("driver", {})
                item.update({
                    "updated_ms": local.get("updated_ms"),
                    "interface_state": local.get("interface_state"),
                    "frequency_mhz": radio.get("frequency_mhz"),
                    "noise_valid": radio.get("noise_valid"),
                    "noise_dbm": radio.get("noise_dbm"),
                    "rssi_dbm": peer.get("rssi_dbm"),
                    "connected_time_s": peer.get("connected_time_s"),
                    "statistics_updated_ms": peer.get("statistics_updated_ms"),
                    "driver_tx_bytes": driver.get("tx_bytes"),
                    "driver_rx_bytes": driver.get("rx_bytes"),
                    "driver_tx_packets": driver.get("tx_packets"),
                    "driver_rx_packets": driver.get("rx_packets"),
                    "driver_tx_failed": driver.get("tx_failed"),
                })
            report["samples"].append(item)

            delay = args.interval - (time.monotonic() - tick)
            if delay > 0:
                time.sleep(delay)

        report["postflight"] = execute(
            sta,
            "printf 'uptime='; cat /proc/uptime; "
            "printf 'iw_pids='; pidof iw 2>/dev/null || true",
            8,
        )
        report["summary"] = summarize(report["samples"])
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        report["finished_utc"] = utc_now()
        sta.close()
        output.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    print(json.dumps({
        "output": str(output),
        "error": report.get("error"),
        "summary": report.get("summary"),
        "finished_utc": report["finished_utc"],
    }, ensure_ascii=False), flush=True)
    raise SystemExit(1 if report.get("error") else 0)


args_global = None

if __name__ == "__main__":
    main()

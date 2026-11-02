#!/usr/bin/env python3
"""Run low-rate LinkG Cellular Probe monitoring and bidirectional ICMP concurrently."""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

from switch_t0_baseline import utc_now


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--duration", type=int, default=120)
    parser.add_argument("--monitor-interval", type=float, default=1.0)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    if args.duration < 30 or args.duration > 300:
        raise SystemExit("duration must be in [30, 300] seconds")
    if args.monitor_interval < 0.1 or args.monitor_interval > 2.0:
        raise SystemExit("monitor interval must be in [0.1, 2.0] seconds")

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    repo_root = Path(__file__).resolve().parents[2]
    monitor_script = repo_root / "test" / "integration" / "cellular_probe_monitor.py"
    icmp_script = repo_root / "test" / "integration" / "cellular_icmp_diagnostic.py"
    monitor_output = output_dir / "correlation_probe_monitor.json"
    icmp_output = output_dir / "correlation_icmp.json"
    runner_output = output_dir / "correlation_runner.json"
    report = {"test_id": "SW-CELLULAR-CORRELATION", "started_utc": utc_now()}
    processes = {}
    try:
        processes["monitor"] = subprocess.Popen(
            [sys.executable, str(monitor_script), "--password", args.password,
             "--duration", str(args.duration), "--interval", str(args.monitor_interval),
             "--output", str(monitor_output)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        time.sleep(0.5)
        processes["icmp"] = subprocess.Popen(
            [sys.executable, str(icmp_script), "--password", args.password,
             "--count", str(args.duration), "--interval", "1",
             "--output", str(icmp_output)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        report["processes"] = {}
        for name, process in processes.items():
            output, _ = process.communicate(timeout=args.duration + 40)
            report["processes"][name] = {"exit": process.returncode, "stdout": output}
        report["monitor_result"] = json.loads(monitor_output.read_text(encoding="utf-8"))
        report["icmp_result"] = json.loads(icmp_output.read_text(encoding="utf-8"))
        report["completed"] = True
    except Exception as exc:
        report["completed"] = False
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        for process in processes.values():
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
        report["finished_utc"] = utc_now()
        runner_output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    monitor = report.get("monitor_result", {}).get("summary", {})
    icmp = report.get("icmp_result", {}).get("results", {})
    print(json.dumps({
        "output": str(runner_output),
        "completed": report.get("completed"),
        "monitor_samples": monitor.get("sample_count"),
        "monitor_failure_samples": monitor.get("failure_sample_count"),
        "monitor_failure_groups": len(monitor.get("failure_groups", [])),
        "icmp_exit": {side: item.get("exit") for side, item in icmp.items()},
        "error": report.get("error"),
    }, ensure_ascii=False))
    raise SystemExit(0 if report.get("completed") else 1)


if __name__ == "__main__":
    main()

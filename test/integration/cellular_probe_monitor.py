#!/usr/bin/env python3
"""Monitor dual-board Cellular Probe reachability without changing board state."""

import argparse
import json
import time
from pathlib import Path

from switch_t0_baseline import AP_HOST, STA_HOST, connect, response_data, utc_now, web_request


CLASSES = ("realtime", "video", "data")


def path_state(item):
    data = response_data(item)
    if data is None:
        return {"valid_response": False}
    peers = data.get("path", {}).get("peers", [])
    if len(peers) != 1:
        return {"valid_response": True, "active": False, "peer_count": len(peers), "probes": {}}
    peer = peers[0]
    return {
        "valid_response": True,
        "node_id": peer.get("node_id"),
        "active": peer.get("active") is True,
        "probes": peer.get("probe", {}),
    }


def plan_state(item, side):
    data = response_data(item)
    if data is None:
        return None
    if side == "sta":
        return data.get("sta", {}).get("plan")
    peers = data.get("ap", {}).get("peers", [])
    return peers[0].get("plan") if peers else None


def failures(state):
    if not state.get("valid_response") or not state.get("active"):
        return ["path"]
    probes = state.get("probes", {})
    failed = []
    for name in CLASSES:
        item = probes.get(name, {})
        if item.get("valid") is not True or item.get("reachable") is not True:
            failed.append(name)
    return failed


def wifi_primary_cellular_secondary(plan):
    classes = (plan or {}).get("classes", {})
    if set(classes) != set(CLASSES):
        return False
    return all(
        item.get("mode") == "single"
        and item.get("primary", {}).get("access") == "wifi"
        and item.get("secondary", {}).get("access") == "cellular"
        for item in classes.values()
    )


def build_failure_groups(samples, interval):
    groups = []
    for side in ("ap", "sta"):
        current = None
        for sample in samples:
            failed = failures(sample[side]["path"])
            signature = tuple(failed)
            if not failed:
                if current is not None:
                    groups.append(current)
                    current = None
                continue
            if current is not None and current["signature"] == signature \
                    and sample["sequence"] == current["last_sequence"] + 1:
                current["last_sequence"] = sample["sequence"]
                current["last_utc"] = sample["captured_utc"]
                current["samples"] += 1
            else:
                if current is not None:
                    groups.append(current)
                current = {
                    "signature": signature,
                    "side": side,
                    "failed": failed,
                    "first_sequence": sample["sequence"],
                    "last_sequence": sample["sequence"],
                    "first_utc": sample["captured_utc"],
                    "last_utc": sample["captured_utc"],
                    "samples": 1,
                }
        if current is not None:
            groups.append(current)
    for group in groups:
        group.pop("signature", None)
        group["observed_span_seconds"] = (group["samples"] - 1) * interval
    return sorted(groups, key=lambda item: (item["first_sequence"], item["side"]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--duration", type=int, default=180)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.duration < 10 or args.duration > 600:
        raise SystemExit("duration must be in [10, 600] seconds")
    if args.interval < 0.1 or args.interval > 2.0:
        raise SystemExit("interval must be in [0.1, 2.0] seconds")

    output_path = Path(args.output).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "test_id": "SW-CELLULAR-PROBE-MONITOR",
        "started_utc": utc_now(),
        "duration_seconds": args.duration,
        "requested_interval_seconds": args.interval,
        "samples": [],
    }
    clients = {}
    try:
        clients["ap"] = connect(AP_HOST, args.password)
        clients["sta"] = connect(STA_HOST, args.password)
        deadline = time.monotonic() + args.duration
        sequence = 0
        while time.monotonic() < deadline:
            started = time.monotonic()
            sample = {"sequence": sequence, "captured_utc": utc_now()}
            for side in ("ap", "sta"):
                cellular = web_request(clients[side], 1009)
                switch = web_request(clients[side], 1003)
                sample[side] = {
                    "cellular": cellular,
                    "switch": switch,
                    "path": path_state(cellular),
                    "plan": plan_state(switch, side),
                }
            report["samples"].append(sample)
            sequence += 1
            delay = args.interval - (time.monotonic() - started)
            if delay > 0:
                time.sleep(delay)

        failure_samples = []
        plan_failures = []
        response_failures = []
        rtt = {side: {name: [] for name in CLASSES} for side in ("ap", "sta")}
        for sample in report["samples"]:
            for side in ("ap", "sta"):
                state = sample[side]["path"]
                failed = failures(state)
                if failed:
                    failure_samples.append({
                        "sequence": sample["sequence"],
                        "captured_utc": sample["captured_utc"],
                        "side": side,
                        "failed": failed,
                        "path": state,
                    })
                if not state.get("valid_response") or response_data(sample[side]["switch"]) is None:
                    response_failures.append({"sequence": sample["sequence"], "side": side})
                if not wifi_primary_cellular_secondary(sample[side]["plan"]):
                    plan_failures.append({
                        "sequence": sample["sequence"],
                        "captured_utc": sample["captured_utc"],
                        "side": side,
                        "plan": sample[side]["plan"],
                    })
                for name, item in state.get("probes", {}).items():
                    if name in CLASSES and item.get("valid") and item.get("reachable") and item.get("rtt_us"):
                        rtt[side][name].append(item["rtt_us"])

        groups = build_failure_groups(report["samples"], args.interval)

        report["summary"] = {
            "sample_count": len(report["samples"]),
            "response_failures": response_failures,
            "failure_sample_count": len(failure_samples),
            "failure_groups": groups,
            "plan_failures": plan_failures,
            "cellular_probes_reachable_all_samples": not failure_samples,
            "plans_stable_wifi_primary_cellular_secondary": not plan_failures,
            "rtt_us": {
                side: {
                    name: {
                        "count": len(values),
                        "min": min(values) if values else 0,
                        "max": max(values) if values else 0,
                        "mean": (sum(values) / len(values)) if values else 0,
                    }
                    for name, values in classes.items()
                }
                for side, classes in rtt.items()
            },
        }
        report["passed"] = not response_failures and not failure_samples and not plan_failures
    except Exception as exc:
        report["passed"] = False
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        for client in clients.values():
            client.close()
        report["finished_utc"] = utc_now()
        output_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(json.dumps({
        "output": str(output_path),
        "passed": report.get("passed"),
        "samples": len(report.get("samples", [])),
        "failure_samples": report.get("summary", {}).get("failure_sample_count"),
        "failure_groups": len(report.get("summary", {}).get("failure_groups", [])),
        "error": report.get("error"),
    }, ensure_ascii=False))
    raise SystemExit(0 if report.get("passed") else 1)


if __name__ == "__main__":
    main()

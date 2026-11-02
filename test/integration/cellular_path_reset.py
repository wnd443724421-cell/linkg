#!/usr/bin/env python3
"""Reset STA Cellular Path between TC scenarios and restore the original config."""

import argparse
import json
import time
from pathlib import Path

from switch_t0_baseline import AP_HOST, STA_HOST, connect, execute, utc_now
from tc_loss_scenario import CLASSES, compact_plan, plan_is_normal


def request(client, command, param=None):
    payload = {"cmd": command}
    if param is not None:
        payload["param"] = param
    encoded = json.dumps(payload, separators=(",", ":")).encode()
    channel = None
    try:
        channel = client.get_transport().open_channel(
            "direct-tcpip", ("127.0.0.1", 29608), ("127.0.0.1", 0), timeout=5
        )
        channel.settimeout(8)
        channel.sendall(encoded)
        channel.shutdown_write()
        chunks = []
        while True:
            chunk = channel.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
        raw = b"".join(chunks).decode("utf-8", "replace")
        return {"response": json.loads(raw), "raw": raw}
    except Exception as exc:
        return {"error": f"{type(exc).__name__}: {exc}"}
    finally:
        if channel is not None:
            channel.close()


def ok(item):
    response = item.get("response") if isinstance(item, dict) else None
    return isinstance(response, dict) and response.get("code") == "ok"


def config_data(item):
    if not ok(item):
        return None
    data = item["response"].get("data")
    return data if isinstance(data, dict) else None


def path_active(item):
    data = config_data(item)
    peers = (data or {}).get("path", {}).get("peers", [])
    if len(peers) != 1 or peers[0].get("active") is not True:
        return False
    probes = peers[0].get("probe", {})
    return set(probes) == set(CLASSES) and all(
        value.get("valid") is True and value.get("reachable") is True
        for value in probes.values()
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    report = {"test_id": "TC-SCENARIO-RESET-CELLULAR-PATH", "started_utc": utc_now(), "samples": []}
    clients = {}
    original = None
    enable_attempted = False
    try:
        clients["ap"] = connect(AP_HOST, args.password)
        clients["sta"] = connect(STA_HOST, args.password)
        report["before_config"] = request(clients["sta"], 1007)
        original = config_data(report["before_config"])
        if original is None or original.get("path_enabled") is not True:
            raise RuntimeError("STA original Cellular Path is not enabled")
        if not isinstance(original.get("cellular"), dict):
            raise RuntimeError("STA Cellular config payload is incomplete")
        report["config_sha256_before"] = execute(clients["sta"], "sha256sum /app/linkgUser.json", 10)

        disabled = json.loads(json.dumps(original))
        disabled["path_enabled"] = False
        report["disable"] = request(clients["sta"], 1008, disabled)
        if not ok(report["disable"]):
            raise RuntimeError(f"Cellular Path disable failed: {report['disable']}")

        deadline = time.monotonic() + 75
        wifi_primary_seen = False
        while time.monotonic() < deadline:
            ap_switch = request(clients["ap"], 1003)
            sta_switch = request(clients["sta"], 1003)
            sta_config = request(clients["sta"], 1007)
            sample = {
                "captured_utc": utc_now(),
                "phase": "disabled",
                "ap_switch": ap_switch,
                "sta_switch": sta_switch,
                "sta_config": sta_config,
            }
            report["samples"].append(sample)
            ap_plan = compact_plan(ap_switch, "ap")
            sta_plan = compact_plan(sta_switch, "sta")
            wifi_primary_seen = all(
                ap_plan[name]["mode"] == "single" and ap_plan[name]["primary"] == "wifi"
                and sta_plan[name]["mode"] == "single" and sta_plan[name]["primary"] == "wifi"
                for name in CLASSES
            )
            cfg = config_data(sta_config)
            if wifi_primary_seen and cfg is not None and cfg.get("path_enabled") is False:
                break
            time.sleep(1)
        report["wifi_primary_seen_while_disabled"] = wifi_primary_seen
        if not wifi_primary_seen:
            raise RuntimeError("plans did not return to Wi-Fi while Cellular Path was disabled")

        enable_attempted = True
        report["enable"] = request(clients["sta"], 1008, original)
        if not ok(report["enable"]):
            raise RuntimeError(f"Cellular Path re-enable failed: {report['enable']}")

        deadline = time.monotonic() + 150
        normal_count = 0
        while time.monotonic() < deadline:
            ap_switch = request(clients["ap"], 1003)
            sta_switch = request(clients["sta"], 1003)
            ap_cell = request(clients["ap"], 1009)
            sta_cell = request(clients["sta"], 1009)
            sta_config = request(clients["sta"], 1007)
            sample = {
                "captured_utc": utc_now(),
                "phase": "reenabled",
                "ap_switch": ap_switch,
                "sta_switch": sta_switch,
                "ap_cellular": ap_cell,
                "sta_cellular": sta_cell,
                "sta_config": sta_config,
            }
            report["samples"].append(sample)
            cfg = config_data(sta_config)
            normal = (
                cfg is not None and cfg.get("path_enabled") is True
                and plan_is_normal(ap_switch, "ap") and plan_is_normal(sta_switch, "sta")
                and path_active(ap_cell) and path_active(sta_cell)
            )
            normal_count = normal_count + 1 if normal else 0
            if normal_count >= 3:
                report["ready_utc"] = sample["captured_utc"]
                break
            time.sleep(1)
        report["normal_confirmed"] = normal_count >= 3
        report["after_config"] = request(clients["sta"], 1007)
        report["config_sha256_after"] = execute(clients["sta"], "sha256sum /app/linkgUser.json", 10)
        report["passed"] = report["normal_confirmed"] and config_data(report["after_config"]) == original
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        report["passed"] = False
    finally:
        if original is not None and not report.get("passed") and "sta" in clients:
            report["emergency_enable"] = request(clients["sta"], 1008, original)
            enable_attempted = True
        report["enable_attempted"] = enable_attempted
        report["finished_utc"] = utc_now()
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        for client in clients.values():
            client.close()
    print(json.dumps({
        "output": str(output), "passed": report.get("passed"), "error": report.get("error"),
        "samples": len(report["samples"]), "normal_confirmed": report.get("normal_confirmed"),
    }, ensure_ascii=False))
    raise SystemExit(0 if report.get("passed") else 1)


if __name__ == "__main__":
    main()

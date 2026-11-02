#!/usr/bin/env python3
"""Validate replacing wlan0 root with 0% netem and restoring mq on both boards."""

import argparse
import json
import re
import time
from pathlib import Path

from switch_t0_baseline import AP_HOST, STA_HOST, connect, execute, utc_now, web_request


def structure(text):
    result = []
    for line in text.splitlines():
        match = re.match(r"qdisc\s+(\S+)\s+\S+:\s+(root|parent\s+\S+)", line)
        if match:
            relation = match.group(2)
            if relation.startswith("parent "):
                relation = "parent :" + relation.rsplit(":", 1)[-1]
            result.append((match.group(1), relation))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    output_path = Path(args.output).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    report = {"test_id": "SW-TC-RESTORE-VALIDATION", "started_utc": utc_now(), "boards": {}}
    all_passed = True

    for side, host in (("ap", AP_HOST), ("sta", STA_HOST)):
        client = None
        board = {}
        restore_attempted = False
        try:
            client = connect(host, args.password)
            board["before"] = execute(client, "tc -d qdisc show dev wlan0")
            board["before_stats"] = execute(client, "tc -s qdisc show dev wlan0")
            board["switch_before"] = web_request(client, 1003)
            before_structure = structure(board["before"]["stdout"])
            expected = [("mq", "root")] + [("fq_codel", f"parent :{index}") for index in range(8, 0, -1)]
            if before_structure != expected:
                raise RuntimeError(f"unexpected original qdisc structure: {before_structure}")

            board["netem_add"] = execute(
                client, "tc qdisc replace dev wlan0 root handle 7ffe: netem loss random 0%", timeout=12
            )
            if board["netem_add"]["exit"] != 0:
                raise RuntimeError("failed to install 0% netem")
            board["during"] = execute(client, "tc -d qdisc show dev wlan0")
            if not any(item[0] == "netem" and item[1] == "root" for item in structure(board["during"]["stdout"])):
                raise RuntimeError("netem root was not observed")
            if "loss 0%" not in board["during"]["stdout"]:
                raise RuntimeError(f"netem did not report loss 0%: {board['during']['stdout']!r}")
            time.sleep(2)
        except Exception as exc:
            board["error"] = f"{type(exc).__name__}: {exc}"
        finally:
            if client is not None:
                restore_attempted = True
                board["restore"] = execute(client, "tc qdisc replace dev wlan0 root mq", timeout=12)
                time.sleep(2)
                board["after"] = execute(client, "tc -d qdisc show dev wlan0")
                board["after_stats"] = execute(client, "tc -s qdisc show dev wlan0")
                board["switch_after"] = web_request(client, 1003)
                after_structure = structure(board["after"]["stdout"])
                expected = [("mq", "root")] + [("fq_codel", f"parent :{index}") for index in range(8, 0, -1)]
                board["restored_structure"] = after_structure == expected
                board["passed"] = (
                    "error" not in board
                    and restore_attempted
                    and board["restore"]["exit"] == 0
                    and board["restored_structure"]
                )
                all_passed = all_passed and board["passed"]
                client.close()
            else:
                board["passed"] = False
                all_passed = False
        report["boards"][side] = board

    report["passed"] = all_passed
    report["finished_utc"] = utc_now()
    output_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "output": str(output_path),
        "passed": report["passed"],
        "boards": {
            side: {"passed": item.get("passed"), "restored": item.get("restored_structure"),
                   "error": item.get("error")}
            for side, item in report["boards"].items()
        },
    }, ensure_ascii=False))
    raise SystemExit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()

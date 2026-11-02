#!/usr/bin/env python3
"""Validate the RK DHCP lease and a bounded RTP/H.264 path through both LinkG boards."""

import argparse
import json
import os
import re
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path

import paramiko


def run(command, timeout=15, env=None):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout, env=env, check=False)
    return {"command": command, "exit": result.returncode, "stdout": result.stdout, "stderr": result.stderr}


def connect(host, password):
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(host, username="root", password=password, timeout=8, auth_timeout=8, banner_timeout=8)
    return client


def remote(client, command, timeout=20):
    stdin, stdout, stderr = client.exec_command(command, timeout=timeout)
    del stdin
    stdout.channel.settimeout(timeout)
    output = stdout.read().decode("utf-8", "replace")
    error = stderr.read().decode("utf-8", "replace")
    return {"command": command, "exit": stdout.channel.recv_exit_status(), "stdout": output, "stderr": error}


def interface_counters(client):
    command = (
        "for i in eth0 wlan0 linkg0; do if [ -d /sys/class/net/$i ]; then "
        "printf '%s ' $i; cat /sys/class/net/$i/statistics/rx_bytes "
        "/sys/class/net/$i/statistics/tx_bytes /sys/class/net/$i/statistics/rx_errors "
        "/sys/class/net/$i/statistics/tx_errors | tr '\\n' ' '; echo; fi; done"
    )
    return remote(client, command)


def parse_counter_lines(text):
    values = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 5 and all(item.isdigit() for item in fields[1:]):
            values[fields[0]] = {
                "rx_bytes": int(fields[1]),
                "tx_bytes": int(fields[2]),
                "rx_errors": int(fields[3]),
                "tx_errors": int(fields[4]),
            }
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--duration", type=int, default=20)
    args = parser.parse_args()

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    receiver_log = output_path.with_name("video_receiver.log")
    sender_log = output_path.with_name("video_sender.log")
    report = {"started_utc": datetime.now(timezone.utc).isoformat(), "duration_seconds": args.duration}
    clients = {}
    alias_added = False
    receiver = None

    try:
        for name, host in (("ap", "192.168.1.1"), ("sta", "192.168.3.1"), ("rk", "172.28.2.3")):
            clients[name] = connect(host, args.password)

        rk = clients["rk"]
        report["dhcp"] = {
            "device": remote(rk, "nmcli -f GENERAL.STATE,IP4.ADDRESS,IP4.GATEWAY,IP4.ROUTE,IP4.DNS device show end0 2>&1"),
            "address": remote(rk, "ip -4 addr show dev end0; ip -4 route show"),
            "gateway_ping": remote(rk, "ping -c 3 -W 1 192.168.3.1"),
            "virtual_ping": remote(rk, "ping -c 3 -W 1 172.28.1.1"),
        }
        dhcp_text = report["dhcp"]["device"]["stdout"] + report["dhcp"]["address"]["stdout"]
        report["dhcp"]["passed"] = (
            re.search(r"192\.168\.3\.\d+/24", dhcp_text) is not None
            and "172.28.0.0/16" in dhcp_text
            and "192.168.3.1" in dhcp_text
            and report["dhcp"]["gateway_ping"]["exit"] == 0
            and report["dhcp"]["virtual_ping"]["exit"] == 0
        )

        report["preflight"] = {
            "receiver_gstreamer": run(["gst-launch-1.0", "--version"]),
            "decoder": run(["gst-inspect-1.0", "avdec_h264"]),
            "ubuntu_address": run(["ip", "-4", "addr", "show", "dev", "ens34"]),
            "sudo": run(["sudo", "-n", "true"]),
            "destination_collision": run(["ping", "-c", "1", "-W", "1", "192.168.1.2"]),
            "rk_encoder": remote(rk, "gst-inspect-1.0 mpph264enc 2>&1 | head -n 20"),
            "rk_camera": remote(rk, "v4l2-ctl -d /dev/video51 --get-fmt-video 2>&1"),
        }

        ubuntu_addresses = report["preflight"]["ubuntu_address"]["stdout"]
        address_match = re.search(r"inet 192\.168\.1\.(\d+)/", ubuntu_addresses)
        if address_match is not None:
            destination_ip = f"172.28.1.{address_match.group(1)}"
        else:
            if report["preflight"]["destination_collision"]["exit"] == 0:
                raise RuntimeError("192.168.1.2 already replies; refusing to create a conflicting test address")
            add_result = run(["sudo", "-n", "ip", "address", "add", "192.168.1.2/32", "dev", "ens34"])
            report["address_add"] = add_result
            if add_result["exit"] != 0:
                raise RuntimeError("cannot add the temporary 192.168.1.2 test address")
            alias_added = True
            destination_ip = "172.28.1.2"

        report["video_destination"] = destination_ip
        report["preflight"]["virtual_destination_ping"] = remote(
            rk, f"ping -c 3 -W 1 {destination_ip}"
        )
        if report["preflight"]["virtual_destination_ping"]["exit"] != 0:
            raise RuntimeError(f"RK cannot reach selected receiver {destination_ip}")

        before = {name: parse_counter_lines(interface_counters(clients[name])["stdout"]) for name in ("ap", "sta")}
        report["interface_before"] = before

        receiver_command = [
            "timeout", "-s", "INT", str(args.duration + 5),
            "gst-launch-1.0", "-v", "udpsrc", "port=5600",
            "caps=application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96",
            "!", "identity", "silent=false", "!", "fakesink", "sync=false",
        ]
        receiver_environment = os.environ.copy()
        receiver_environment["GST_DEBUG"] = "2"
        receiver = subprocess.Popen(receiver_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, env=receiver_environment)
        time.sleep(1.0)

        sender_command = (
            f"timeout -s INT {args.duration} gst-launch-1.0 -v "
            "v4l2src device=/dev/video51 ! "
            "video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! "
            "mpph264enc bps=6000000 rc-mode=1 gop=10 qp-init=30 zero-copy-pkt=true max-reenc=0 ! "
            "h264parse config-interval=-1 ! rtph264pay aggregate-mode=zero-latency pt=96 mtu=1300 ! "
            f"udpsink host={destination_ip} port=5600 sync=false async=false"
        )
        sender_result = remote(rk, sender_command, timeout=args.duration + 15)
        sender_log.write_text(sender_result["stdout"] + sender_result["stderr"], encoding="utf-8")
        report["sender"] = {"exit": sender_result["exit"], "log": str(sender_log)}

        receiver_output, _ = receiver.communicate(timeout=args.duration + 12)
        receiver_log.write_text(receiver_output, encoding="utf-8")
        buffer_messages = receiver_output.count("chain   ******") + receiver_output.count("last-message = chain")
        report["receiver"] = {
            "exit": receiver.returncode,
            "mode": "RTP/H.264 packet sink; decoding unavailable on Ubuntu because h264parse/avdec_h264 are absent",
            "rtp_buffer_messages": buffer_messages,
            "log": str(receiver_log),
        }

        after = {name: parse_counter_lines(interface_counters(clients[name])["stdout"]) for name in ("ap", "sta")}
        report["interface_after"] = after
        report["interface_delta"] = {}
        for name in ("ap", "sta"):
            report["interface_delta"][name] = {}
            for interface, final in after[name].items():
                initial = before[name].get(interface, {key: 0 for key in final})
                report["interface_delta"][name][interface] = {
                    key: final[key] - initial.get(key, 0) for key in final
                }

        error_text = (sender_result["stdout"] + sender_result["stderr"] + receiver_output).lower()
        report["video_passed"] = (
            buffer_messages >= 100
            and "error from element" not in error_text
            and report["interface_delta"]["sta"].get("eth0", {}).get("rx_bytes", 0) > 100000
            and report["interface_delta"]["sta"].get("linkg0", {}).get("tx_bytes", 0) > 100000
            and report["interface_delta"]["ap"].get("linkg0", {}).get("rx_bytes", 0) > 100000
            and report["interface_delta"]["ap"].get("eth0", {}).get("tx_bytes", 0) > 100000
            and all(
                counters.get("rx_errors", 0) == 0 and counters.get("tx_errors", 0) == 0
                for target in after.values() for counters in target.values()
            )
        )
        report["passed"] = report["dhcp"]["passed"] and report["video_passed"]
    except Exception as exc:
        report["passed"] = False
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if receiver is not None and receiver.poll() is None:
            receiver.terminate()
            try:
                receiver.wait(timeout=3)
            except subprocess.TimeoutExpired:
                receiver.kill()
        if alias_added:
            report["address_remove"] = run(
                ["sudo", "-n", "ip", "address", "delete", "192.168.1.2/32", "dev", "ens34"]
            )
            if report["address_remove"]["exit"] != 0:
                report["passed"] = False
                report["cleanup_error"] = "temporary Ubuntu test address removal failed"
        for client in clients.values():
            client.close()

    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    with output_path.open("w", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
        output.write("\n")
    print(json.dumps({"output": str(output_path), "passed": report.get("passed"),
                      "dhcp_passed": report.get("dhcp", {}).get("passed"),
                      "video_passed": report.get("video_passed"), "error": report.get("error")}))
    raise SystemExit(0 if report.get("passed") else 1)


if __name__ == "__main__":
    main()

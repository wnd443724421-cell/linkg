"""Validate the flashed LinkG MCU through the production UART Web API.

The script preserves the board configuration in a local JSON snapshot and
restores the exact UART configuration in a finally block.
"""

from __future__ import annotations

from pathlib import Path
from urllib.request import Request, urlopen
import copy
import json
import socket
import statistics
import time


BOARD_A = "192.168.1.1"
BOARD_B = "192.168.3.1"
VIRTUAL_B = "172.28.2.1"
ARTIFACT_DIR = Path(__file__).resolve().parents[2] / "validation"
SNAPSHOT_PATH = ARTIFACT_DIR / "board-mcu-pretest-snapshot.json"
RESULT_PATH = ARTIFACT_DIR / "board-mcu-validation.json"


def request(host: str, cmd: int, uart=None):
    body = {"cmd": cmd, "param": {} if uart is None else {"uart": uart}}
    encoded = json.dumps(body, separators=(",", ":")).encode()
    http_request = Request(
        f"http://{host}/cgi-bin/cmain",
        data=encoded,
        headers={"Content-Type": "application/json"},
    )
    with urlopen(http_request, timeout=5) as response:
        value = json.loads(response.read())
    assert value["code"] == "ok", value
    return value


def get_version(host: str) -> str:
    with urlopen(f"http://{host}/version.json", timeout=5) as response:
        return json.loads(response.read())["version"]


def apply(host: str, uart, expected: str = "applied"):
    response = request(host, 1013, uart)
    state = response["data"]["apply"]
    if state == "none":
        assert expected == "none", response
        return request(host, 1014)["data"]

    assert state == "applying", response
    job_id = int(response["data"]["job_id"])
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        status = request(host, 1014)["data"]
        apply_status = status["apply"]
        if apply_status["state"] != "applying":
            assert int(apply_status["job_id"]) == job_id, status
            assert apply_status["state"] == expected, status
            return status
        time.sleep(0.05)
    raise AssertionError(f"UART apply timed out: host={host} job={job_id}")


def uart0(status):
    return next(port for port in status["ports"] if int(port["uart_num"]) == 0)


def assert_running(status, sbus: bool):
    port = uart0(status)
    assert status["state"] == "running", status
    assert status["worker_running"] is True, status
    assert port["device_open"] is True, port
    assert port["sbus_running"] is sbus, port
    assert int(port["errors"]) == 0, port
    assert int(port["drops"]) == 0, port
    return port


def config(port_type: str, port: int):
    if port_type == "sbus":
        physical = {
            "type": "sbus",
            "baudrate": 100000,
            "data_bits": 8,
            "stop_bits": 2,
            "parity": "e",
        }
    else:
        physical = {
            "type": "normal",
            "baudrate": 115200,
            "data_bits": 8,
            "stop_bits": 1,
            "parity": "n",
        }
    return [
        {
            "uart_num": 0,
            "port_config": physical,
            "routes": [
                {
                    "proto": "udp",
                    "endpoint": "local",
                    "dir": "rxtx",
                    "ip": "0.0.0.0",
                    "port": port,
                }
            ],
        }
    ]


def receive_udp_bytes(sock: socket.socket, expected_length: int):
    chunks = []
    received = 0
    while received < expected_length:
        chunk, _source = sock.recvfrom(4096)
        chunks.append(chunk)
        received += len(chunk)
        assert received <= expected_length, (received, expected_length)
    return b"".join(chunks), len(chunks)


def normal_payload(index: int, length: int) -> bytes:
    return bytes(((byte_index * 37) + index * 53 + length) & 0xFF for byte_index in range(length))


def sbus_frame(sequence: int) -> bytes:
    tails = (0x00, 0x04, 0x14)
    payload = bytes(((sequence * 23) + index) & 0xFF for index in range(23))
    return b"\x0f" + payload + bytes([tails[sequence % len(tails)]])


def verify_counter_delta(before, after, byte_count: int):
    for name in ("uart_rx_bytes", "uart_tx_bytes", "net_rx_bytes", "net_tx_bytes"):
        delta = int(after[name]) - int(before[name])
        assert delta == byte_count, (name, delta, byte_count, before, after)
    assert int(after["errors"]) == int(before["errors"]), (before, after)
    assert int(after["drops"]) == int(before["drops"]), (before, after)


def normal_loopback(port: int, label: str):
    status = apply(BOARD_B, config("normal", port))
    before = assert_running(status, sbus=False)
    lengths = (1, 2, 3, 7, 16, 25, 64, 127, 255, 512)
    total = 0
    datagrams = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.settimeout(3)
        for index, length in enumerate(lengths):
            payload = normal_payload(index, length)
            client.sendto(payload, (BOARD_B, port))
            echoed, count = receive_udp_bytes(client, len(payload))
            assert echoed == payload, (label, length, echoed.hex(), payload.hex())
            total += len(payload)
            datagrams += count

    after_status = request(BOARD_B, 1014)["data"]
    after = assert_running(after_status, sbus=False)
    verify_counter_delta(before, after, total)
    assert after["routes"] == ["udp_ready"], after
    return {
        "label": label,
        "bytes": total,
        "payloads": len(lengths),
        "returned_datagrams": datagrams,
        "status": after,
    }


def sbus_loopback(port: int, frame_count: int, label: str):
    status = apply(BOARD_B, config("sbus", port))
    before = assert_running(status, sbus=True)
    baud = before["baud"]
    assert baud["verified"] is True, baud
    assert abs(float(baud["actual"]) - 100000.0) <= 3000.0, baud
    assert abs(float(baud["error_pct"])) <= 3.0, baud

    frames = [sbus_frame(index) for index in range(frame_count)]
    covered = {value for frame in frames for value in frame[1:24]}
    latencies_ms = []
    total = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.settimeout(2)
        for frame in frames:
            started = time.perf_counter()
            client.sendto(frame, (BOARD_B, port))
            echoed, _count = receive_udp_bytes(client, len(frame))
            latencies_ms.append((time.perf_counter() - started) * 1000.0)
            assert echoed == frame, (label, echoed.hex(), frame.hex())
            total += len(frame)

        burst = b"".join(sbus_frame(frame_count + index) for index in range(4))
        client.sendto(burst, (BOARD_B, port))
        echoed, returned_datagrams = receive_udp_bytes(client, len(burst))
        assert echoed == burst, (label, echoed.hex(), burst.hex())
        assert returned_datagrams == 4, returned_datagrams
        total += len(burst)

    after_status = request(BOARD_B, 1014)["data"]
    after = assert_running(after_status, sbus=True)
    verify_counter_delta(before, after, total)
    assert after["routes"] == ["udp_ready"], after
    return {
        "label": label,
        "bytes": total,
        "frames": frame_count + 4,
        "payload_octets_covered": len(covered),
        "latency_ms": {
            "minimum": min(latencies_ms),
            "median": statistics.median(latencies_ms),
            "maximum": max(latencies_ms),
        },
        "baud": baud,
        "status": after,
    }


def main():
    original_a = request(BOARD_A, 1012)["data"]["uart"]
    original_b = request(BOARD_B, 1012)["data"]["uart"]
    status_a_before = request(BOARD_A, 1014)["data"]
    status_b_before = request(BOARD_B, 1014)["data"]
    snapshot = {
        "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "version_a": get_version(BOARD_A),
        "version_b": get_version(BOARD_B),
        "config_a": original_a,
        "config_b": original_b,
        "status_a": status_a_before,
        "status_b": status_b_before,
    }
    SNAPSHOT_PATH.write_text(json.dumps(snapshot, indent=2, ensure_ascii=False), encoding="utf-8")

    results = {"normal": [], "sbus": [], "switches": []}
    restored = False
    try:
        results["normal"].append(normal_loopback(32120, "post-flash normal baseline"))
        results["sbus"].append(sbus_loopback(32121, 64, "MCU SBUS sustained loopback"))

        for cycle in range(3):
            normal_result = normal_loopback(32130 + cycle * 2, f"switch cycle {cycle + 1} normal")
            sbus_result = sbus_loopback(32131 + cycle * 2, 1, f"switch cycle {cycle + 1} SBUS")
            results["switches"].append(
                {
                    "cycle": cycle + 1,
                    "normal_bytes": normal_result["bytes"],
                    "sbus_bytes": sbus_result["bytes"],
                    "sbus_latency_ms": sbus_result["latency_ms"],
                }
            )

        results["normal"].append(normal_loopback(32140, "final normal after SBUS switches"))

        apply(BOARD_B, original_b)
        restored = True
        assert request(BOARD_B, 1012)["data"]["uart"] == original_b
        apply(BOARD_B, copy.deepcopy(original_b), expected="none")

        final_a_config = request(BOARD_A, 1012)["data"]["uart"]
        final_a_status = request(BOARD_A, 1014)["data"]
        final_b_status = request(BOARD_B, 1014)["data"]
        assert final_a_config == original_a
        assert final_a_status["state"] == "running" and final_a_status["worker_running"]
        assert final_b_status["state"] == "running" and final_b_status["worker_running"]
        a_before_port = uart0(status_a_before)
        a_after_port = uart0(final_a_status)
        assert int(a_after_port["errors"]) == int(a_before_port["errors"]), (a_before_port, a_after_port)
        assert int(a_after_port["drops"]) == int(a_before_port["drops"]), (a_before_port, a_after_port)

        results["restored_config"] = original_b
        results["final_status_a"] = final_a_status
        results["final_status_b"] = final_b_status
        results["completed_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        RESULT_PATH.write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8")

        sustained = results["sbus"][0]
        print(
            "PASS MCU Normal baseline: "
            f"{results['normal'][0]['bytes']} exact bytes across "
            f"{results['normal'][0]['payloads']} payload sizes"
        )
        print(
            "PASS MCU SBUS sustained: "
            f"{sustained['frames']} exact frames/{sustained['bytes']} bytes, "
            f"payload coverage={sustained['payload_octets_covered']}/256, "
            f"latency min/median/max="
            f"{sustained['latency_ms']['minimum']:.3f}/"
            f"{sustained['latency_ms']['median']:.3f}/"
            f"{sustained['latency_ms']['maximum']:.3f} ms"
        )
        print("PASS MCU mode switching: 4 complete Normal->SBUS->Normal cycles with exact loopback")
        print("PASS restore: board B exact original UART config; unchanged apply=none; board A errors/drops unchanged")
        print(f"EVIDENCE {RESULT_PATH}")
    finally:
        if not restored:
            try:
                apply(BOARD_B, original_b)
                print("RESTORED board B original UART configuration after test failure")
            except Exception as error:
                print("CRITICAL: failed to restore board B:", repr(error))


if __name__ == "__main__":
    main()

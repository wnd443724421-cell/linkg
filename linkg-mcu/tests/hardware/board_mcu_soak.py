"""Run a sustained SBUS loopback soak against the flashed MCU."""

from pathlib import Path
import copy
import json
import time

from board_mcu_validation import BOARD_A, BOARD_B, apply, request, sbus_loopback, uart0


RESULT_PATH = Path(__file__).resolve().parents[2] / "validation" / "board-mcu-soak.json"


def main():
    original_a = request(BOARD_A, 1012)["data"]["uart"]
    original_b = request(BOARD_B, 1012)["data"]["uart"]
    status_a_before = request(BOARD_A, 1014)["data"]
    restored = False
    started = time.monotonic()

    try:
        result = sbus_loopback(32150, 2000, "MCU SBUS 2000-frame soak")
        duration = time.monotonic() - started

        apply(BOARD_B, original_b)
        restored = True
        assert request(BOARD_B, 1012)["data"]["uart"] == original_b
        apply(BOARD_B, copy.deepcopy(original_b), expected="none")

        final_a_config = request(BOARD_A, 1012)["data"]["uart"]
        final_a_status = request(BOARD_A, 1014)["data"]
        final_b_status = request(BOARD_B, 1014)["data"]
        assert final_a_config == original_a
        before_a = uart0(status_a_before)
        after_a = uart0(final_a_status)
        assert int(after_a["errors"]) == int(before_a["errors"]), (before_a, after_a)
        assert int(after_a["drops"]) == int(before_a["drops"]), (before_a, after_a)
        assert final_b_status["state"] == "running" and final_b_status["worker_running"]

        evidence = {
            "completed_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "duration_seconds": duration,
            "result": result,
            "restored_config": original_b,
            "final_status_a": final_a_status,
            "final_status_b": final_b_status,
        }
        RESULT_PATH.write_text(json.dumps(evidence, indent=2, ensure_ascii=False), encoding="utf-8")
        latency = result["latency_ms"]
        print(
            f"PASS MCU SBUS soak: {result['frames']} exact frames/{result['bytes']} bytes, "
            f"coverage={result['payload_octets_covered']}/256, duration={duration:.3f}s"
        )
        print(
            "PASS soak latency min/median/max="
            f"{latency['minimum']:.3f}/{latency['median']:.3f}/{latency['maximum']:.3f} ms"
        )
        print("PASS soak counters: UART RX/TX and network RX/TX exact; errors=0; drops=0")
        print("PASS soak restore: exact board B config; unchanged apply=none; board A errors/drops unchanged")
        print(f"EVIDENCE {RESULT_PATH}")
    finally:
        if not restored:
            try:
                apply(BOARD_B, original_b)
                print("RESTORED board B original UART configuration after soak failure")
            except Exception as error:
                print("CRITICAL: failed to restore board B:", repr(error))


if __name__ == "__main__":
    main()

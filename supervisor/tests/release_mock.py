#!/usr/bin/env python3
"""Isolated LinkG/ENV doubles; no real device access."""

import fcntl
import json
import os
import pathlib
import signal
import sys
import time

root = pathlib.Path(os.environ["LINKG_RELEASE_TEST_ROOT"])
kind = pathlib.Path(sys.argv[0]).name

if kind == "printenv":
    if (root / "read-fail").exists():
        sys.exit(1)
    environment = json.loads((root / "env.json").read_text())
    for name in sys.argv[1:]:
        if name not in environment:
            sys.exit(1)
        print(name + "=" + str(environment[name]))
    sys.exit(0)

if kind == "setenv":
    if (root / "write-fail").exists():
        sys.exit(1)
    if (root / "write-hang").exists():
        time.sleep(10)
    if (root / "write-delay").exists():
        time.sleep(float((root / "write-delay").read_text()))
    environment = json.loads((root / "env.json").read_text())
    batch = {}
    for line in pathlib.Path(sys.argv[2]).read_text().splitlines():
        name, value = line.split(" ", 1)
        batch[name] = value
    assert set(batch) == {"boot_partition", "stable_boot_partition", "bootcount", "upgrade_available"}
    with (root / "env-writes").open("a") as stream:
        stream.write(json.dumps(batch) + "\n")
    environment.update(batch)
    temporary = root / "env-new"
    temporary.write_text(json.dumps(environment))
    os.replace(temporary, root / "env.json")
    if (root / "write-noack").exists():
        sys.exit(1)
    sys.exit(0)

release = pathlib.Path(__file__).resolve().parents[1].name
mode_path = root / (release + "-mode")
mode = mode_path.read_text() if mode_path.exists() else "healthy"
production = os.environ.get("LINKG_RELEASE_TEST_PRODUCTION") == "1"
lock = (root / "child.lock").open("a")
try:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    print("RELEASE_OVERLAP", flush=True)
    sys.exit(99)

with (root / "starts").open("a") as stream:
    stream.write(f"{release} {os.getpid()} {time.monotonic()}\n")
descriptor = int(os.environ["LINKG_SUPERVISOR_FD"])

if mode == "ignore-term":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
if mode != "no-ready":
    os.write(descriptor, b"\x01")

started = time.monotonic()
restarted = False
while True:
    elapsed = time.monotonic() - started
    if mode == "exit" and elapsed >= (50 if production else 0.82):
        sys.exit(17)
    if mode == "exit-during-confirm" and elapsed >= 1.22:
        sys.exit(18)
    if mode == "post-confirm-exit" and elapsed >= 1.65:
        mode_path.write_text("healthy")
        sys.exit(17)
    if mode == "restart" and not restarted and elapsed >= 0.7:
        os.write(descriptor, b"\x03")
        time.sleep(0.3)
        os.write(descriptor, b"\x01")
        restarted = True
    if mode == "restart-timeout" and not restarted and elapsed >= 0.15:
        os.write(descriptor, b"\x03")
        restarted = True
    if mode == "restart-during-confirm" and not restarted and elapsed >= 1.2:
        os.write(descriptor, b"\x03")
        time.sleep(0.2)
        os.write(descriptor, b"\x01")
        restarted = True
    if mode in ("heartbeat", "ignore-term") and elapsed >= (10 if production else 0.17):
        time.sleep(0.05)
        continue
    if mode not in ("no-ready", "restart-timeout"):
        os.write(descriptor, b"\x02")
    time.sleep(1 if production else 0.06)

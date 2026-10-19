#!/usr/bin/env python3
"""Exercise managed Supervisor with isolated release trees and ENV/reboot doubles."""

import concurrent.futures
import json
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SUPERVISOR = pathlib.Path(sys.argv[1]).resolve()
DOUBLE = pathlib.Path(__file__).with_name("release_mock.py")
FAULTS = pathlib.Path(sys.argv[2]).resolve()
PROCESS_FAULTS = pathlib.Path(sys.argv[3]).resolve()
PRODUCTION = "--production" in sys.argv


def require(condition, detail):
    if not condition:
        raise AssertionError(detail)


class Fixture:
    def __init__(self, candidate=True, different=False, actual=None, mode="healthy"):
        self.temporary = tempfile.TemporaryDirectory(prefix="linkg-release-")
        self.root = pathlib.Path(self.temporary.name)
        self.app = self.root / "app"
        self.web = self.root / "www"
        self.state = self.app / "version_state.json"
        self.journal = self.app / "version_state.json.confirming"
        for release in ["v1", "v2"]:
            for base in [self.app, self.web]:
                (base / "releases" / release / "bin").mkdir(parents=True)
            program = self.app / "releases" / release / "bin/linkg"
            shutil.copy2(DOUBLE, program)
            program.chmod(0o700)
        for base in [self.app, self.web]:
            (base / "current").symlink_to("releases/v1")
        self.document = {
            "schema_version": 1,
            "release_partition": "v2" if candidate else "v1",
            "stable_release_partition": "v1",
            "releases": {"v1": {"kernel_slot": "boot_a"}, "v2": {"kernel_slot": "boot_b" if different else "boot_a"}},
        }
        self.state.write_text(json.dumps(self.document))
        slot = actual or ("boot_b" if different and candidate else "boot_a")
        (self.root / "cmdline").write_text("console=ttyS0 kernel_slot=" + slot + "\n")
        environment = {
            "boot_partition": slot,
            "stable_boot_partition": "boot_a",
            "bootcount": "2" if different else "0",
            "upgrade_available": "1" if different else "0",
            "bootlimit": "5",
            "bootcmd": "run setargs_nor boot_normal",
            "boot_normal": "sunxi_flash read 43000000 ${boot_partition};bootm 43000000",
            "altbootcmd": "setenv boot_partition ${stable_boot_partition}; setenv upgrade_available 0; setenv bootcount 0; saveenv; run setargs_nor boot_normal",
        }
        (self.root / "env.json").write_text(json.dumps(environment))
        for name in ["printenv", "setenv"]:
            tool = self.root / name
            shutil.copy2(DOUBLE, tool)
            tool.chmod(0o700)
        (self.root / "v2-mode").write_text(mode)
        self.process = None
        self.log = None

    def start(self, extra_environment=None):
        self.log = (self.root / "log").open("a")
        environment = dict(os.environ, LINKG_RELEASE_TEST_ROOT=str(self.root))
        environment["LD_PRELOAD"] = str(FAULTS)
        if PRODUCTION:
            environment["LINKG_RELEASE_TEST_PRODUCTION"] = "1"
        environment.update(extra_environment or {})
        for name, file in [("CMDLINE", "cmdline"), ("PRINTENV", "printenv"), ("SETENV", "setenv"), ("REBOOT", "reboot")]:
            environment["LINKG_SUPERVISOR_TEST_" + name] = str(self.root / file)
        self.process = subprocess.Popen(
            [str(SUPERVISOR), "--lock-file", str(self.root / "lock"), "--state-file", str(self.state),
             "--app-root", str(self.app), "--web-root", str(self.web)],
            stdout=self.log, stderr=self.log, env=environment,
        )

    def output(self):
        path = self.root / "log"
        return path.read_text() if path.exists() else ""

    def wait(self, predicate, timeout=None):
        timeout = timeout or (85 if PRODUCTION else 5)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            require(self.process.poll() is None, self.output())
            time.sleep(0.015)
        raise AssertionError(self.output())

    def read_state(self):
        return json.loads(self.state.read_text())

    def environment(self):
        return json.loads((self.root / "env.json").read_text())

    def starts(self):
        path = self.root / "starts"
        return [line.split() for line in path.read_text().splitlines()] if path.exists() else []

    def selected(self, release):
        return all((base / "current").resolve() == base / "releases" / release for base in [self.app, self.web])

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=9 if PRODUCTION else 4)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise
        if self.log is not None:
            self.log.close()
        require("RELEASE_OVERLAP" not in self.output(), self.output())
        for _, pid, _ in self.starts():
            require(not pathlib.Path("/proc", pid).exists(), "Child still alive: " + self.output())

    def close(self):
        try:
            self.stop()
        finally:
            self.temporary.cleanup()


def scenario(function):
    fixture = Fixture(**getattr(function, "options", {}))
    try:
        function(fixture)
    finally:
        fixture.close()


def options(**values):
    def decorate(function):
        function.options = values
        return function
    return decorate


@options(candidate=False)
def stable_start(f):
    f.start()
    f.wait(lambda: len(f.starts()) == 1 and "state=RUNNING" in f.output())
    require(f.starts()[0][0] == "v1" and f.selected("v1"), f.output())
    require(not (f.root / "env-writes").exists(), f.output())


def candidate_confirmation(f):
    f.start()
    f.wait(lambda: "state=RUNNING" in f.output())
    require(f.read_state()["stable_release_partition"] == "v1", f.output())
    f.wait(lambda: "release=CONFIRMED" in f.output())
    require(f.read_state()["stable_release_partition"] == "v2" and f.selected("v2"), f.output())
    require(len(f.starts()) == 1 and not f.journal.exists(), f.output())


@options(different=True)
def kernel_confirmation(f):
    candidate_confirmation(f)
    e = f.environment()
    require(e["boot_partition"] == e["stable_boot_partition"] == "boot_b", str(e))
    require(e["bootcount"] == "0" and e["upgrade_available"] == "0" and e["bootlimit"] == "5", str(e))
    require(not (f.root / "reboot").exists(), f.output())


def failed_candidate(f):
    f.start()
    f.wait(lambda: any(row[0] == "v1" for row in f.starts()))
    require(f.read_state()["release_partition"] == f.read_state()["stable_release_partition"] == "v1", f.output())
    require(f.selected("v1") and not (f.root / "reboot").exists(), f.output())
    require(sum(row[0] == "v2" for row in f.starts()) == 1, f.output())


@options(mode="no-ready")
def ready_timeout(f):
    failed_candidate(f)
    require("timeout=READY" in f.output(), f.output())


@options(mode="heartbeat")
def heartbeat_timeout(f):
    failed_candidate(f)
    require("timeout=HEARTBEAT" in f.output(), f.output())


@options(mode="exit")
def late_exit(f):
    failed_candidate(f)
    require("exited code=17" in f.output(), f.output())


@options(mode="restart-timeout")
def restart_timeout(f):
    failed_candidate(f)
    require("timeout=RESTARTING" in f.output(), f.output())


@options(mode="ignore-term")
def forced_kill(f):
    failed_candidate(f)
    require("signal=SIGKILL" in f.output(), f.output())


@options(different=True, mode="heartbeat")
def different_kernel_rollback(f):
    f.start()
    f.wait(lambda: (f.root / "reboot").exists())
    require(all(row[0] == "v2" for row in f.starts()), f.output())
    require(f.read_state()["release_partition"] == "v1", f.output())
    e = f.environment()
    require(e["boot_partition"] == e["stable_boot_partition"] == "boot_a" and e["upgrade_available"] == e["bootcount"] == "0", str(e))
    f.stop()
    (f.root / "cmdline").write_text("kernel_slot=boot_a\n")
    (f.root / "reboot").unlink()
    f.start()
    f.wait(lambda: any(row[0] == "v1" for row in f.starts()))
    require(f.selected("v1"), f.output())


@options(different=True, actual="boot_a")
def uboot_already_rolled_back(f):
    f.start()
    f.wait(lambda: len(f.starts()) > 0)
    require(f.starts()[0][0] == "v1" and f.read_state()["release_partition"] == "v1", f.output())
    require(not (f.root / "reboot").exists(), f.output())


def interrupted_trial(f):
    f.start()
    f.wait(lambda: "state=RUNNING" in f.output())
    time.sleep(0.45)
    f.stop()
    require(f.read_state()["stable_release_partition"] == "v1" and not f.journal.exists(), f.output())
    f.start()
    f.wait(lambda: len(f.starts()) == 2)
    time.sleep(0.65)
    require(f.read_state()["stable_release_partition"] == "v1", f.output())
    f.wait(lambda: f.read_state()["stable_release_partition"] == "v2")


@options(mode="restart")
def restart_resets_window(f):
    f.start()
    f.wait(lambda: "state=RESTARTING" in f.output())
    f.wait(lambda: f.output().count("state=RUNNING") == 2)
    time.sleep(0.65)
    require(f.read_state()["stable_release_partition"] == "v1", f.output())
    f.wait(lambda: "release=CONFIRMED" in f.output())


@options(mode="post-confirm-exit")
def stable_after_confirmation(f):
    f.start()
    f.wait(lambda: len(f.starts()) >= 2)
    require(all(row[0] == "v2" for row in f.starts()), f.output())
    require(f.read_state()["stable_release_partition"] == "v2" and "release=ROLLBACK" not in f.output(), f.output())


@options(candidate=False)
def stable_recovers(f):
    (f.root / "v1-mode").write_text("heartbeat")
    f.start()
    f.wait(lambda: len(f.starts()) >= 2)
    require("retry_ms=100" in f.output() and all(row[0] == "v1" for row in f.starts()), f.output())


def corrupt_json(f):
    f.state.write_text("{bad")
    f.start()
    time.sleep(0.4)
    require(f.process.poll() is None and not f.starts(), f.output())


def uninstalled_candidate(f):
    f.document["releases"]["v2"] = {}
    f.state.write_text(json.dumps(f.document))
    f.start()
    f.wait(lambda: len(f.starts()) > 0)
    require(f.starts()[0][0] == "v1", f.output())


def missing_release(f):
    (f.app / "releases/v2/bin/linkg").unlink()
    f.start()
    f.wait(lambda: len(f.starts()) > 0)
    require(f.starts()[0][0] == "v1", f.output())


def invalid_link(f):
    (f.web / "current").unlink()
    (f.web / "current").mkdir()
    f.start()
    time.sleep(0.6)
    require(not f.starts() and (f.web / "current").is_dir() and f.selected("v1") is False, f.output())
    require((f.app / "current").resolve() == f.app / "releases/v1", f.output())


@options(different=True)
def env_write_failure(f):
    (f.root / "write-fail").touch()
    f.start()
    f.wait(lambda: "confirmation pending" in f.output())
    require(f.read_state()["stable_release_partition"] == "v1" and not f.journal.exists(), f.output())
    require(f.environment()["upgrade_available"] == "1", f.output())
    (f.root / "write-fail").unlink()
    f.wait(lambda: f.read_state()["stable_release_partition"] == "v2")


@options(different=True)
def pending_decision_recovery(f):
    environment = f.environment()
    environment.update(boot_partition="boot_b", stable_boot_partition="boot_b", bootcount="0", upgrade_available="0")
    (f.root / "env.json").write_text(json.dumps(environment))
    f.start()
    f.wait(lambda: len(f.starts()) > 0)
    require(f.read_state()["stable_release_partition"] == "v2" and not f.journal.exists(), f.output())
    require("ENV confirmation recovering" in f.output(), f.output())


@options(different=True, actual="boot_a")
def pending_decision_but_uboot_rollback(f):
    uboot_already_rolled_back(f)
    require(not f.journal.exists(), f.output())


def mixed_links_after_power_loss(f):
    (f.app / "current").unlink()
    (f.app / "current").symlink_to("releases/v2")
    candidate_confirmation(f)


def unknown_actual_kernel(f):
    (f.root / "cmdline").write_text("boot_partition=boot_a\n")
    f.start()
    time.sleep(0.4)
    require(not f.starts() and not (f.root / "env-writes").exists(), f.output())
    require(f.selected("v1"), f.output())


def json_schema_validation(f):
    documents = [
        dict(f.document, schema_version=2),
        dict(f.document, release_partition="v3"),
        dict(f.document, stable_release_partition="v3"),
        dict(f.document, extra=True),
        dict(f.document, releases={"v1": {"kernel_slot": "boot_a"}, "v2": {"kernel_slot": "boot_c"}}),
    ]
    for document in documents:
        f.state.write_text(json.dumps(document))
        f.start()
        time.sleep(0.16)
        require(not f.starts(), f.output())
        f.stop()


def coordinated_link_failure(f):
    (f.root / "fail-link").touch()
    f.start()
    f.wait(lambda: any(row[0] == "v1" for row in f.starts()))
    require(f.selected("v1") and all(row[0] == "v1" for row in f.starts()), f.output())
    require("candidate preparation failed" in f.output(), f.output())


@options(different=True)
def json_commit_failure_and_recovery(f):
    (f.root / "fail-json").touch()
    f.start()
    f.wait(lambda: "confirmation pending" in f.output())
    require(f.read_state()["stable_release_partition"] == "v1", f.output())
    e = f.environment()
    require(e["boot_partition"] == e["stable_boot_partition"] == "boot_b" and e["bootcount"] == e["upgrade_available"] == "0", str(e))
    f.stop()
    (f.root / "fail-json").unlink()
    f.start()
    f.wait(lambda: "ENV confirmation recovering" in f.output())
    f.wait(lambda: len(f.starts()) == 2)
    require(f.read_state()["stable_release_partition"] == "v2", f.output())


def app_only_json_failure_retries(f):
    (f.root / "fail-json").touch()
    f.start()
    f.wait(lambda: "confirmation pending" in f.output())
    require(f.read_state()["stable_release_partition"] == "v1" and not (f.root / "env-writes").exists(), f.output())
    (f.root / "fail-json").unlink()
    f.wait(lambda: f.read_state()["stable_release_partition"] == "v2")


@options(different=True)
def directory_sync_failure_retries(f):
    f.start()
    f.wait(lambda: "state=RUNNING" in f.output())
    (f.root / "fail-dir-sync").touch()
    f.wait(lambda: "confirmation pending" in f.output())
    require(f.read_state()["stable_release_partition"] == "v2" and len(f.starts()) == 1, f.output())
    require(f.environment()["stable_boot_partition"] == "boot_b", f.output())
    (f.root / "fail-dir-sync").unlink()
    f.wait(lambda: "release=CONFIRMED" in f.output())


@options(different=True)
def rollback_json_before_env(f):
    f.document["release_partition"] = "v1"
    f.state.write_text(json.dumps(f.document))
    f.start()
    f.wait(lambda: (f.root / "reboot").exists())
    require(not f.starts(), f.output())
    require(f.environment()["boot_partition"] == "boot_a", f.output())


@options(different=True)
def partial_env_is_not_confirmation(f):
    e = f.environment()
    e.update(stable_boot_partition="boot_b", bootcount="0", upgrade_available="1")
    (f.root / "env.json").write_text(json.dumps(e))
    f.start()
    f.wait(lambda: len(f.starts()) == 1)
    require(f.read_state()["stable_release_partition"] == "v1" and "ENV confirmation recovering" not in f.output(), f.output())
    require(f.environment()["stable_boot_partition"] == "boot_a", f.output())
    f.wait(lambda: f.read_state()["stable_release_partition"] == "v2")


@options(different=True)
def env_commit_without_ack(f):
    (f.root / "write-noack").touch()
    f.start()
    f.wait(lambda: "confirmation pending" in f.output())
    require(f.read_state()["stable_release_partition"] == "v1", f.output())
    require(f.environment()["stable_boot_partition"] == "boot_b", f.output())
    f.stop()
    (f.root / "write-noack").unlink()
    f.start()
    f.wait(lambda: f.read_state()["stable_release_partition"] == "v2")
    require("ENV confirmation recovering" in f.output(), f.output())


@options(mode="heartbeat")
def rollback_json_failure_blocks_spawn(f):
    (f.root / "fail-json").touch()
    f.start()
    f.wait(lambda: "rollback pending" in f.output())
    require(len(f.starts()) == 1 and f.read_state()["release_partition"] == "v2", f.output())
    (f.root / "fail-json").unlink()
    f.wait(lambda: any(row[0] == "v1" for row in f.starts()))


@options(different=True, mode="heartbeat")
def rollback_env_failure_blocks_spawn(f):
    (f.root / "write-fail").touch()
    f.start()
    f.wait(lambda: "rollback pending" in f.output())
    require(len(f.starts()) == 1 and f.read_state()["release_partition"] == "v1", f.output())
    require(not (f.root / "reboot").exists(), f.output())
    (f.root / "write-fail").unlink()
    f.wait(lambda: (f.root / "reboot").exists())
    require(all(row[0] == "v2" for row in f.starts()), f.output())


@options(mode="ignore-term")
def unreaped_candidate_blocks_rollback(f):
    marker = f.root / "identity-fault"
    f.start({"LD_PRELOAD": str(FAULTS) + ":" + str(PROCESS_FAULTS),
             "LINKG_SUPERVISOR_FAULT_MODE": "unknown", "LINKG_SUPERVISOR_FAULT_MARKER": str(marker)})
    f.wait(lambda: "state=RUNNING" in f.output())
    marker.touch()
    f.wait(lambda: "cleanup unresolved" in f.output())
    require(len(f.starts()) == 1 and f.read_state()["release_partition"] == "v2", f.output())
    require("unsafe_signal" not in f.output(), f.output())
    marker.unlink()
    f.wait(lambda: any(row[0] == "v1" for row in f.starts()))


def duplicate_and_null_strings(f):
    for text in [
        json.dumps(f.document).replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1'),
        json.dumps(f.document).replace('"v2"', '"v2\\u0000junk"', 1),
        json.dumps(f.document) + "garbage",
    ]:
        f.state.write_text(text)
        f.start()
        time.sleep(0.15)
        require(not f.starts(), f.output())
        f.stop()


@options(different=True)
def slow_env_keeps_monitoring(f):
    (f.root / "write-delay").write_text("0.55")
    kernel_confirmation(f)


@options(different=True, mode="exit-during-confirm")
def exit_during_env_cannot_confirm(f):
    (f.root / "write-delay").write_text("0.55")
    f.start()
    f.wait(lambda: (f.root / "reboot").exists())
    require(f.read_state()["stable_release_partition"] == "v1" and "release=CONFIRMED" not in f.output(), f.output())
    require(all(row[0] == "v2" for row in f.starts()), f.output())


@options(different=True, mode="restart-during-confirm")
def restart_during_env_resets_window(f):
    (f.root / "write-delay").write_text("0.55")
    f.start()
    f.wait(lambda: "confirmation pending" in f.output())
    f.wait(lambda: f.output().count("state=RUNNING") == 2)
    time.sleep(0.5)
    require(f.read_state()["stable_release_partition"] == "v1", f.output())
    f.wait(lambda: "release=CONFIRMED" in f.output())
    require(len(f.starts()) == 1, f.output())


def production_confirmation(f):
    f.start()
    f.wait(lambda: "state=RUNNING" in f.output())
    ready_at = time.monotonic()
    time.sleep(55)
    require(f.read_state()["stable_release_partition"] == "v1", f.output())
    f.wait(lambda: f.read_state()["stable_release_partition"] == "v2", timeout=12)
    require(time.monotonic() - ready_at >= 59.8, f.output())
    require(len(f.starts()) == 1, f.output())


def main():
    cases = [
        stable_start, candidate_confirmation, kernel_confirmation, ready_timeout, heartbeat_timeout, late_exit,
        restart_timeout, forced_kill, different_kernel_rollback, uboot_already_rolled_back, interrupted_trial,
        restart_resets_window, stable_after_confirmation, stable_recovers, corrupt_json, uninstalled_candidate,
        missing_release, invalid_link, env_write_failure, pending_decision_recovery, pending_decision_but_uboot_rollback,
        mixed_links_after_power_loss, unknown_actual_kernel, json_schema_validation, coordinated_link_failure,
        json_commit_failure_and_recovery, app_only_json_failure_retries, directory_sync_failure_retries, rollback_json_before_env,
        partial_env_is_not_confirmation, env_commit_without_ack, rollback_json_failure_blocks_spawn,
        rollback_env_failure_blocks_spawn, unreaped_candidate_blocks_rollback, duplicate_and_null_strings,
        slow_env_keeps_monitoring, exit_during_env_cannot_confirm, restart_during_env_resets_window,
    ]
    failures = []
    if PRODUCTION:
        cases = [production_confirmation, ready_timeout, heartbeat_timeout, late_exit]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4 if PRODUCTION else 2) as pool:
        jobs = {pool.submit(scenario, case): case.__name__ for case in cases}
        for job in concurrent.futures.as_completed(jobs):
            try:
                job.result()
                print("PASS", jobs[job], flush=True)
            except Exception as error:
                failures.append(jobs[job])
                print("FAIL", jobs[job], repr(error), flush=True)
    require(not failures, str(failures))
    print(f"PASS: {len(cases)} managed-release scenarios")


if __name__ == "__main__":
    main()

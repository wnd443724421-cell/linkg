#!/usr/bin/env python3
"""Exercise persistent phases and real OTA worker with Flash/ENV/reboot doubles."""
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import unittest

r = Path(__file__).resolve().parents[2]
b = r / "build-native/ota"
supervisor = r / "output/bin/x86/linkg-supervisor-test"
saved_argv = sys.argv[:]
sys.argv = ["test_release.py", str(supervisor), str(r / "output/lib/x86/libsupervisor-release-faults.so"), str(r / "output/lib/x86/libsupervisor-faults.so")]
spec = importlib.util.spec_from_file_location("release_tests", r / "supervisor/tests/test_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
sys.argv = saved_argv


class OtaTests(unittest.TestCase):
    def fixture(self, **kwargs):
        f = release.Fixture(**kwargs)
        self.addCleanup(f.close)
        return f

    def phase(self, f, phase):
        f.document["upgrade_state"] = phase
        f.state.write_text(json.dumps(f.document))

    def ready(self, f):
        f.wait(lambda: "state=RUNNING" in f.output())

    def test_legacy_json_reports_idle_via_web(self):
        f = self.driver()
        response = self.request(f, "g")
        self.assertEqual(response["data"]["upgrade_state"], "idle")

    def test_historical_results_survive_restart(self):
        for phase in ("completed", "rolled_back", "failed"):
            with self.subTest(phase=phase):
                f = self.fixture(candidate=False)
                self.phase(f, phase)
                f.start(); self.ready(f)
                self.assertEqual(f.read_state()["upgrade_state"], phase)
                f.stop(); f.start(); self.ready(f)
                self.assertEqual(f.read_state()["upgrade_state"], phase)

    def test_interrupted_installation_records_failed_after_stable_ready(self):
        for phase in ("verifying", "installing", "flashing", "rebooting"):
            with self.subTest(phase=phase):
                f = self.fixture(candidate=False)
                self.phase(f, phase)
                f.start()
                f.wait(lambda: f.read_state().get("upgrade_state") == "failed")
                self.assertTrue(f.selected("v1"))

    def test_candidate_testing_then_completed(self):
        f = self.fixture()
        self.phase(f, "rebooting")
        f.start(); self.ready(f)
        self.assertEqual(f.read_state()["upgrade_state"], "testing")
        self.assertEqual(f.read_state()["stable_release_partition"], "v1")
        f.wait(lambda: f.read_state().get("upgrade_state") == "completed")
        self.assertEqual(f.read_state()["stable_release_partition"], "v2")

    def test_failed_candidate_rolled_back_only_after_stable_ready(self):
        f = self.fixture(mode="exit")
        self.phase(f, "rebooting")
        f.start()
        f.wait(lambda: f.read_state().get("upgrade_state") == "rolled_back")
        self.assertTrue(f.selected("v1"))
        self.assertIn("state=RUNNING", f.output())

    def test_cross_kernel_rollback_waits_for_actual_stable_boot(self):
        f = self.fixture(different=True, mode="heartbeat")
        self.phase(f, "rebooting")
        f.start()
        f.wait(lambda: (f.root / "reboot").exists())
        self.assertEqual(f.read_state()["upgrade_state"], "rolling_back")
        self.assertFalse(any(row[0] == "v1" for row in f.starts()))
        f.stop()
        (f.root / "cmdline").write_text("kernel_slot=boot_a\n")
        f.start()
        f.wait(lambda: f.read_state().get("upgrade_state") == "rolled_back")
        self.assertTrue(f.selected("v1"))

    def test_prepare_json_before_env_interruption_rolls_back(self):
        f = self.fixture(different=True, actual="boot_a")
        self.phase(f, "rebooting")
        f.start()
        f.wait(lambda: f.read_state().get("upgrade_state") == "rolled_back")
        self.assertEqual(f.environment()["boot_partition"], "boot_a")

    def test_confirm_env_before_json_interruption_completes(self):
        f = self.fixture(different=True)
        self.phase(f, "testing")
        e = f.environment()
        e.update(boot_partition="boot_b", stable_boot_partition="boot_b", bootcount="0", upgrade_available="0")
        (f.root / "env.json").write_text(json.dumps(e))
        f.start()
        f.wait(lambda: f.read_state().get("upgrade_state") == "completed")
        self.assertEqual(f.read_state()["stable_release_partition"], "v2")

    def test_invalid_phase_and_unknown_keys_rejected(self):
        for update in ({"upgrade_state": "success"}, {"upgrade_state": 1}, {"unknown": 1}):
            with self.subTest(update=update):
                f = self.fixture(candidate=False)
                f.document.update(update)
                f.state.write_text(json.dumps(f.document))
                f.start()
                f.wait(lambda: "release preparation pending" in f.output())
                self.assertFalse(f.starts())

    def test_install_lease_blocks_startup_state_changes(self):
        f = self.fixture(candidate=False)
        self.phase(f, "installing")
        with (f.app / "version_state.json.ota.lock").open("a") as lease:
            fcntl.flock(lease, fcntl.LOCK_EX)
            f.start()
            f.wait(lambda: "release preparation pending" in f.output())
            self.assertEqual(f.read_state()["upgrade_state"], "installing")
            self.assertFalse(f.starts())
        f.wait(lambda: f.read_state().get("upgrade_state") == "failed")

    def driver(self):
        f = self.fixture(candidate=False)
        shutil.copy2(b / "ota-driver", f.app / "releases/v1/bin/linkg")
        tmp = f.root / "tmp"; tmp.mkdir()
        dev = f.root / "dev"; dev.mkdir()
        system = f.root / "system"; system.mkdir()
        (dev / "mtd0").write_bytes(b"A" * 4096 + b"\xff" * (32768 - 4096))
        (dev / "mtd1").write_bytes(b"C" * 32768)
        (f.root / "mtd").write_text('dev: size erasesize name\nmtd0: 00008000 00001000 "boot_a"\nmtd1: 00008000 00001000 "boot_b"\n')
        values = {"STATE": f.state, "APP_ROOT": f.app, "WEB_ROOT": f.web, "UPLOAD": tmp / "upgrade.tar.gz",
                  "UPLOAD_LOCK": tmp / ".linkg-upload.lock", "TASK_TEMPLATE": tmp / ".linkg-upgrade-task.XXXXXX",
                  "EXTRACT_ROOT": tmp, "EXTRACT_PATH": tmp / "upgrade", "CMDLINE": f.root / "cmdline",
                  "DEV_ROOT": dev, "MTD_TABLE": f.root / "mtd", "SYSTEM_ROOT": system, "REBOOT": f.root / "reboot"}
        env = dict(os.environ, LINKG_RELEASE_TEST_ROOT=str(f.root))
        env.update({"LINKG_OTA_TEST_" + name: str(path) for name, path in values.items()})
        env["LINKG_OTA_TEST_UPLOAD_TEMP"] = str(tmp / ".linkg-upload.XXXXXX")
        env["LINKG_OTA_TEST_CONFIG"] = str(tmp / "config_import.json")
        f.driver_env = env
        for name, file in [("CMDLINE", "cmdline"), ("PRINTENV", "printenv"), ("SETENV", "setenv"), ("REBOOT", "reboot"), ("ENV_LOCK", "env.lock")]:
            env["LINKG_SUPERVISOR_TEST_" + name] = str(f.root / file)
        f.log = (f.root / "log").open("a")
        f.process = subprocess.Popen([str(supervisor), "--lock-file", str(f.root / "lock"), "--state-file", str(f.state),
                                      "--app-root", str(f.app), "--web-root", str(f.web)], stdin=subprocess.PIPE,
                                     stdout=f.log, stderr=f.log, env=env)
        original_stop = f.stop
        def stop_driver():
            original_stop()
            if f.process is not None and f.process.stdin is not None:
                f.process.stdin.close()
        f.stop = stop_driver
        f.wait(lambda: "DRIVER_READY" in f.output())
        f.tmp = tmp
        return f

    def request(self, f, command):
        count = f.output().count("RESPONSE ")
        f.process.stdin.write(command.encode()); f.process.stdin.flush()
        f.wait(lambda: f.output().count("RESPONSE ") > count)
        lines = [line for line in f.output().splitlines() if line.startswith("RESPONSE ")]
        return json.loads(lines[-1].split(" ", 2)[2])

    def package(self, f, kernel=None, system=False):
        builder = f.root / "builder"; builder.mkdir()
        (builder / "VERSION").write_text("V1.2.3\n")
        package = builder / "upgrade"; package.mkdir()
        shutil.copy2(r / "upgrade/build.sh", package / "build.sh")
        for name in ["app/bin/linkg", "www/index.html", "www/cgi-bin/cmain", "www/cgi-bin/upgrade", "www/version.json"]:
            p = package / name; p.parent.mkdir(parents=True, exist_ok=True)
            if name == "app/bin/linkg": shutil.copy2(release.DOUBLE, p)
            else: p.write_text("{}\n" if name.endswith("json") else "fixture\n")
            p.chmod(0o755)
        if kernel is not None:
            (package / "kernel").mkdir(); (package / "kernel/boot.img").write_bytes(kernel)
        if system:
            (package / "app/usr/sbin").mkdir(parents=True)
            shutil.copy2(r / "output/bin/x86/linkg-supervisor", package / "app/usr/sbin/linkg-supervisor")
            (package / "system_install.json").write_text(json.dumps({"schema_version": 1, "files": [{"source": "usr/sbin/linkg-supervisor", "destination": "/usr/sbin/linkg-supervisor"}]}))
        p = subprocess.run(["bash", str(package / "build.sh")], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        body = next((package / "output").glob("*.tar.gz")).read_bytes()
        env = dict(f.driver_env, REQUEST_METHOD="POST", CONTENT_LENGTH=str(len(body)), HTTP_X_UPLOAD_TYPE="upgrade")
        result = subprocess.run([str(b / "upgrade-cgi")], input=body, capture_output=True, env=env)
        self.assertEqual(result.returncode, 0)
        response = json.loads(result.stdout.split(b"\r\n\r\n", 1)[1])
        self.assertEqual(response["code"], "ok", response)
        self.assertEqual(response["size"], len(body))

    def install(self, f):
        response = self.request(f, "s")
        self.assertEqual(response["code"], "ok", response)
        self.assertTrue(response["data"]["accepted"])
        f.wait(lambda: (f.root / "reboot").exists() or f.read_state().get("upgrade_state") == "failed", timeout=8)

    def test_app_only_end_to_end_commit_and_confirmation(self):
        f = self.driver()
        self.package(f)
        stable = (f.app / "releases/v1/bin/linkg").read_bytes()
        self.install(f)
        self.assertEqual(f.read_state()["upgrade_state"], "rebooting")
        self.assertEqual(f.read_state()["releases"]["v2"]["kernel_slot"], "boot_a")
        self.assertFalse((f.root / "env-writes").exists())
        self.assertTrue(f.selected("v1"))
        self.assertEqual((f.app / "releases/v1/bin/linkg").read_bytes(), stable)
        self.assertFalse((f.app / "releases/v2/kernel/boot.img").exists())
        self.assertFalse((f.tmp / "upgrade.tar.gz").exists())
        self.assertEqual(self.request(f, "g")["data"]["upgrade_state"], "rebooting")
        self.assertEqual(self.request(f, "s")["code"], "error")
        f.stop(); f.start()
        f.wait(lambda: f.read_state().get("upgrade_state") == "completed")
        self.assertEqual(f.read_state()["stable_release_partition"], "v2")

    def test_same_kernel_does_not_flash_or_write_env(self):
        f = self.driver(); self.package(f, kernel=b"A" * 4096)
        old = (f.root / "dev/mtd1").read_bytes()
        self.install(f)
        self.assertEqual(f.read_state()["upgrade_state"], "rebooting")
        self.assertEqual((f.root / "dev/mtd1").read_bytes(), old)
        self.assertFalse((f.root / "env-writes").exists())

    def test_changed_kernel_flashes_inactive_and_readback_then_env(self):
        f = self.driver(); self.package(f, kernel=b"B" * 4096)
        stable = (f.root / "dev/mtd0").read_bytes()
        self.install(f)
        self.assertEqual(f.read_state()["upgrade_state"], "rebooting")
        self.assertEqual((f.root / "dev/mtd1").read_bytes(), b"B" * 4096 + b"\xff" * (32768 - 4096))
        self.assertEqual((f.root / "dev/mtd0").read_bytes(), stable)
        self.assertEqual(f.environment()["boot_partition"], "boot_b")
        self.assertEqual(f.environment()["stable_boot_partition"], "boot_a")
        self.assertEqual(f.environment()["upgrade_available"], "1")
        self.assertEqual(f.environment()["bootlimit"], "5")

    def test_system_supervisor_uses_fixed_path(self):
        f = self.driver(); self.package(f, system=True); self.install(f)
        self.assertEqual(f.read_state()["upgrade_state"], "rebooting")
        self.assertEqual((f.root / "system/usr/sbin/linkg-supervisor").read_bytes(), (r / "output/bin/x86/linkg-supervisor").read_bytes())

    def test_invalid_archive_records_failed_no_reboot(self):
        f = self.driver()
        (f.tmp / "upgrade.tar.gz").write_bytes(b"invalid archive")
        self.install(f)
        self.assertEqual(f.read_state()["upgrade_state"], "failed")
        self.assertEqual(f.read_state()["release_partition"], "v1")
        self.assertFalse((f.root / "reboot").exists())

    def test_env_write_failure_cannot_confirm_or_reboot(self):
        f = self.driver(); self.package(f, kernel=b"B" * 4096)
        (f.root / "write-fail").touch()
        self.install(f)
        self.assertEqual(f.read_state()["upgrade_state"], "failed")
        self.assertEqual(f.read_state()["release_partition"], "v1")
        self.assertFalse((f.root / "reboot").exists())

    def test_corrupt_state_web_errors_instead_of_idle(self):
        f = self.driver()
        f.state.write_text("corrupt")
        self.assertEqual(self.request(f, "g")["code"], "error")

    def test_next_upgrade_replaces_previous_failed_state(self):
        f = self.driver()
        (f.tmp / "upgrade.tar.gz").write_bytes(b"broken")
        self.install(f)
        self.assertEqual(f.read_state()["upgrade_state"], "failed")
        self.package(f)
        self.install(f)
        self.assertEqual(f.read_state()["upgrade_state"], "rebooting")

    def test_cgi_incomplete_and_oversized_body_rejected(self):
        f = self.driver()
        for length, body in (("10", b"short"), (str(64 * 1024 * 1024 + 1), b""), ("-1", b"")):
            env = dict(f.driver_env, REQUEST_METHOD="POST", CONTENT_LENGTH=length)
            result = subprocess.run([str(b / "upgrade-cgi")], input=body, capture_output=True, env=env)
            response = json.loads(result.stdout.split(b"\r\n\r\n", 1)[1])
            self.assertEqual(response["code"], "error")
            self.assertFalse((f.tmp / "upgrade.tar.gz").exists())

    def test_old_running_manager_cannot_publish_extended_json(self):
        f = self.driver()
        self.package(f)
        # An unmanaged copy receives no live parent capability, even if the new binary is on disk.
        result = subprocess.run([str(b / "ota-driver")], input=b"s", capture_output=True, env=f.driver_env, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [line for line in result.stdout.decode().splitlines() if line.startswith("RESPONSE ")]
        self.assertEqual(json.loads(lines[-1].split(" ", 2)[2])["code"], "error")
        self.assertNotIn("upgrade_state", f.read_state())
        self.assertTrue((f.tmp / "upgrade.tar.gz").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)

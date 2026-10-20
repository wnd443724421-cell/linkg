#!/usr/bin/env python3
"""Native tests use temporary roots; no real /usr, MTD, ENV or reboot access."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]


class SystemInstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="linkg-system-build-")
        cls.runner = Path(cls.build.name) / "install"
        subprocess.run([
            "gcc", "-std=gnu17", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-I" + str(REPO / "core/upgrade"), "-I" + str(REPO / "third_party/cjson"),
            str(REPO / "test/upgrade/system_install_test.c"),
            str(REPO / "core/upgrade/upgrade_system.c"),
            str(REPO / "third_party/cjson/cJSON.c"), "-Wl,--wrap=open", "-o", str(cls.runner),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="linkg-system-test-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.package = self.base / "upgrade"
        self.system = self.base / "system"
        self.package.mkdir()
        (self.package / "app/usr/sbin").mkdir(parents=True)
        (self.system / "usr/sbin").mkdir(parents=True)
        self.source = self.package / "app/usr/sbin/linkg-supervisor"
        self.destination = self.system / "usr/sbin/linkg-supervisor"
        self.source.write_bytes(b"new-supervisor\n")
        self.source.chmod(0o755)
        self.destination.write_bytes(b"old-supervisor\n")
        self.destination.chmod(0o755)

    def declaration(self, source="usr/sbin/linkg-supervisor", destination="/usr/sbin/linkg-supervisor", extra=None):
        entries = [{"source": source, "destination": destination}]
        if extra is not None:
            entries.append(extra)
        (self.package / "system_install.json").write_text(json.dumps({"schema_version": 1, "files": entries}))

    def run_install(self):
        result = subprocess.run([str(self.runner), str(self.package), str(self.system)], capture_output=True, text=True, check=True)
        return int(result.stdout.strip())

    def assert_rejected(self):
        before = self.destination.read_bytes()
        self.assertLess(self.run_install(), 0)
        self.assertEqual(self.destination.read_bytes(), before)

    def test_optional_declaration_missing(self):
        self.assertEqual(self.run_install(), 0)
        self.assertEqual(self.destination.read_bytes(), b"old-supervisor\n")

    def test_screenshot_app_relative_supervisor(self):
        old_inode = self.destination.stat().st_ino
        self.declaration()
        self.assertEqual(self.run_install(), 0)
        self.assertEqual(self.destination.read_bytes(), self.source.read_bytes())
        self.assertEqual(self.destination.stat().st_mode & 0o777, 0o755)
        self.assertNotEqual(self.destination.stat().st_ino, old_inode)
        self.assertEqual(list(self.destination.parent.glob(".linkg-system-*")), [])

    def test_legacy_app_prefix(self):
        self.declaration(source="app/usr/sbin/linkg-supervisor")
        self.assertEqual(self.run_install(), 0)

    def test_firmware_destinations_preserved(self):
        for destination in ("/vendor/firmware/radio.bin", "/lib/firmware/radio.bin"):
            with self.subTest(destination=destination):
                self.declaration(destination=destination)
                self.assertEqual(self.run_install(), 0)
                self.assertEqual((self.system / destination.lstrip("/")).read_bytes(), self.source.read_bytes())

    def test_arbitrary_system_targets_rejected(self):
        for destination in ("/usr/sbin/other", "/etc/init.d/linkg", "/app/version_state.json", "/proc/sysrq-trigger", "/usr/sbin/linkg-supervisor/extra"):
            with self.subTest(destination=destination):
                self.declaration(destination=destination)
                self.assert_rejected()

    def test_unsafe_source_paths_rejected(self):
        for source in ("../usr/sbin/linkg-supervisor", "/usr/sbin/linkg-supervisor", "usr/../sbin/linkg-supervisor", "usr//sbin/linkg-supervisor", "usr/sbin/", "usr/sbin/linkg-supervisor\x00suffix"):
            with self.subTest(source=source):
                self.declaration(source=source)
                self.assert_rejected()

    def test_nonexecutable_supervisor_rejected(self):
        self.source.chmod(0o644)
        self.declaration()
        self.assert_rejected()

    def test_source_symlink_rejected(self):
        other = self.source.with_name("other")
        self.source.rename(other)
        self.source.symlink_to(other.name)
        self.declaration()
        self.assert_rejected()

    def test_source_parent_symlink_rejected(self):
        directory = self.package / "app/usr/sbin"
        directory.rename(directory.with_name("real"))
        directory.symlink_to("real", target_is_directory=True)
        self.declaration()
        self.assert_rejected()

    def test_destination_symlink_rejected(self):
        old = self.destination.with_name("old")
        self.destination.rename(old)
        self.destination.symlink_to(old.name)
        self.declaration()
        self.assert_rejected()

    def test_destination_parent_symlink_rejected(self):
        directory = self.system / "usr/sbin"
        directory.rename(directory.with_name("real"))
        directory.symlink_to("real", target_is_directory=True)
        self.declaration()
        self.assert_rejected()

    def test_all_entries_validated_before_replacing_any(self):
        self.declaration(extra={"source": "missing.bin", "destination": "/lib/firmware/missing.bin"})
        self.assert_rejected()

    def test_duplicate_target_rejected(self):
        self.declaration(extra={"source": "usr/sbin/linkg-supervisor", "destination": "/usr/sbin/linkg-supervisor"})
        self.assert_rejected()

    def test_duplicate_json_key_rejected(self):
        (self.package / "system_install.json").write_text('{"schema_version":1,"schema_version":1,"files":[]}')
        self.assert_rejected()

    def test_unknown_json_key_rejected(self):
        self.declaration()
        path = self.package / "system_install.json"
        data = json.loads(path.read_text())
        data["unexpected"] = True
        path.write_text(json.dumps(data))
        self.assert_rejected()

    def test_atomic_replacement_keeps_running_old_program(self):
        fixture = self.base / "probe.c"
        fixture.write_text('#include <stdio.h>\n#include <unistd.h>\nint main(void) { puts(VERSION); fflush(stdout); usleep(300000); puts(VERSION); return 0; }\n')
        for version, output in (("old", self.destination), ("new", self.source)):
            subprocess.run(["gcc", '-DVERSION="' + version + '"', str(fixture), "-o", str(output)], check=True)
        self.declaration()
        with subprocess.Popen([str(self.destination)], stdout=subprocess.PIPE, text=True) as running:
            self.assertEqual(running.stdout.readline().strip(), "old")
            self.assertEqual(self.run_install(), 0)
            self.assertEqual(running.communicate(timeout=3)[0].strip(), "old")
            self.assertEqual(running.returncode, 0)
        result = subprocess.run([str(self.destination)], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.splitlines(), ["new", "new"])

    def build_package(self, legacy=False, destination="/usr/sbin/linkg-supervisor", executable=True):
        root = self.base / "builder"
        root.mkdir()
        (root / "VERSION").write_text("V1.2.3\n")
        upgrade = root / "upgrade"
        upgrade.mkdir()
        shutil.copy2(REPO / "upgrade/build.sh", upgrade / "build.sh")
        for name in ("app/bin/linkg", "app/usr/sbin/linkg-supervisor", "www/index.html", "www/cgi-bin/cmain", "www/cgi-bin/upgrade", "www/version.json"):
            path = upgrade / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("{}\n" if name.endswith("json") else "fixture\n")
            path.chmod(0o755)
        if not executable:
            (upgrade / "app/usr/sbin/linkg-supervisor").chmod(0o644)
        source = ("app/" if legacy else "") + "usr/sbin/linkg-supervisor"
        (upgrade / "system_install.json").write_text(json.dumps({"schema_version": 1, "files": [{"source": source, "destination": destination}]}))
        return subprocess.run(["bash", str(upgrade / "build.sh")], capture_output=True, text=True)

    def test_builder_accepts_screenshot_json(self):
        result = self.build_package()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_builder_accepts_legacy_prefix(self):
        result = self.build_package(legacy=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_builder_rejects_arbitrary_destination(self):
        self.assertNotEqual(self.build_package(destination="/etc/passwd").returncode, 0)

    def test_builder_rejects_nonexecutable_supervisor(self):
        self.assertNotEqual(self.build_package(executable=False).returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)

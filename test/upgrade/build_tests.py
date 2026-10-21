#!/usr/bin/env python3
"""Build strict native OTA integration targets without touching deployed binaries."""
from pathlib import Path
import subprocess

r = Path(__file__).resolve().parents[2]
b = r / "build-native/ota"
b.mkdir(parents=True, exist_ok=True)
includes = {p.parent for root in ["include", "core", "service", "third_party/cjson"] for p in (r / root).rglob("*.h")}
command = ["gcc", "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-DLINKG_OTA_TEST_IO", "-DLINKG_SUPERVISOR_TEST_BUILD"]
command += ["-I" + str(p) for p in sorted(includes)]
sources = list((r / "core/upgrade").glob("*.c")) + [r / n for n in [
    "test/upgrade/ota_driver.c", "infra/base/linkg_release_state.c", "infra/base/linkg_atomic_file.c",
    "infra/base/linkg_boot_env.c", "infra/base/linkg_thread.c", "infra/base/linkg_supervisor_client.c",
    "infra/base/linkg_json.c", "third_party/cjson/cJSON.c", "service/web/web_response.c", "service/web/handlers/web_handler_upgrade.c",
]]
subprocess.run(command + [str(s) for s in sources] + ["-pthread", "-lm", "-Wl,--wrap=open,--wrap=fstat,--wrap=fsync,--wrap=ioctl", "-o", str(b / "ota-driver")], check=True)
print(b / "ota-driver")
subprocess.run(["gcc", "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-DLINKG_OTA_TEST_IO", str(r / "Tools/upgrade/upgrade.c"), "-o", str(b / "upgrade-cgi")], check=True)

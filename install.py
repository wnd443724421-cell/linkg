#!/usr/bin/env python3
"""Install LinkG 5G speedtest files into a matching LinkG checkout.

Usage: python3 install.py /path/to/linkg [--dry-run]
The installer preserves unrelated source changes and makes timestamped backups.
"""

from __future__ import annotations

import argparse
import datetime
import os
from pathlib import Path
import shutil
import sys
import tempfile

PACKAGE = Path(__file__).resolve().parent
NEW_FILES = [
    "service/web/speedtest/web_speedtest.h",
    "service/web/speedtest/web_speedtest.c",
    "service/web/handlers/web_handler_speedtest.c",
    "www/view/diagnostics_speedtest.html",
    "www/css/pages/diagnostics_speedtest.css",
    "www/js/api/speedtest.js",
    "www/js/pages/diagnostics_speedtest.js",
]
PATCHED_FILES = [
    "service/web/web_internal.h",
    "service/web/web_dispatch.c",
    "service/web/linkg_web.c",
    "www/index.html",
]


def replace_once(content: str, old: str, new: str, filename: str) -> str:
    if new in content:
        return content
    count = content.count(old)
    if count != 1:
        raise ValueError(f"{filename}: integration anchor mismatch (count={count}): {old[:90]!r}")
    return content.replace(old, new, 1)


def patched(filename: str, content: str) -> str:
    if filename == "service/web/web_internal.h":
        content = replace_once(
            content,
            "    LINKG_WEB_CMD_UART_DIAG_STOP        = 1027  // 请求取消UART硬件诊断",
            "    LINKG_WEB_CMD_UART_DIAG_STOP        = 1027, // 请求取消UART硬件诊断\n"
            "    LINKG_WEB_CMD_SPEEDTEST_START      = 1028, // 启动5G测速任务\n"
            "    LINKG_WEB_CMD_SPEEDTEST_GET        = 1029, // 查询5G测速任务\n"
            "    LINKG_WEB_CMD_SPEEDTEST_STOP       = 1030  // 取消5G测速任务",
            filename,
        )
        content = replace_once(
            content,
            "int  _linkg_web_handler_uart_diagnostic_stop(const cJSON *param, char **response);",
            "int  _linkg_web_handler_uart_diagnostic_stop(const cJSON *param, char **response);\n"
            "int  _linkg_web_handler_speedtest_start(const cJSON *param, char **response);\n"
            "int  _linkg_web_handler_speedtest_get(const cJSON *param, char **response);\n"
            "int  _linkg_web_handler_speedtest_stop(const cJSON *param, char **response);",
            filename,
        )
    elif filename == "service/web/web_dispatch.c":
        content = replace_once(
            content,
            "    {LINKG_WEB_CMD_UART_DIAG_STOP,        _linkg_web_handler_uart_diagnostic_stop},",
            "    {LINKG_WEB_CMD_UART_DIAG_STOP,        _linkg_web_handler_uart_diagnostic_stop},\n"
            "    {LINKG_WEB_CMD_SPEEDTEST_START,       _linkg_web_handler_speedtest_start},\n"
            "    {LINKG_WEB_CMD_SPEEDTEST_GET,         _linkg_web_handler_speedtest_get},\n"
            "    {LINKG_WEB_CMD_SPEEDTEST_STOP,        _linkg_web_handler_speedtest_stop},",
            filename,
        )
        content = replace_once(
            content,
            "         cmd == LINKG_WEB_CMD_UART_DIAG_START))",
            "         cmd == LINKG_WEB_CMD_UART_DIAG_START || cmd == LINKG_WEB_CMD_SPEEDTEST_START))",
            filename,
        )
    elif filename == "service/web/linkg_web.c":
        content = replace_once(
            content,
            '#include "web_internal.h"',
            '#include "web_internal.h"\n#include "web_speedtest.h"',
            filename,
        )
        content = replace_once(
            content,
            "    _linkg_web_system_init();\n\n    ret = _linkg_web_server_init();",
            "    _linkg_web_system_init();\n\n"
            "    ret = _linkg_web_speedtest_init();\n"
            "    if (ret != 0)\n"
            "    {\n"
            "        return ret;\n"
            "    }\n\n"
            "    ret = _linkg_web_server_init();",
            filename,
        )
        content = replace_once(
            content,
            "    ret = _linkg_web_server_init();\n    if (ret != 0)\n    {\n        return ret;\n    }",
            "    ret = _linkg_web_server_init();\n    if (ret != 0)\n    {\n"
            "        _linkg_web_speedtest_deinit();\n"
            "        return ret;\n    }",
            filename,
        )
        content = replace_once(
            content,
            "    ret = _linkg_web_server_start();\n    if (ret != 0)\n    {\n        return ret;\n    }",
            "    ret = _linkg_web_speedtest_start();\n"
            "    if (ret != 0)\n"
            "    {\n"
            "        return ret;\n"
            "    }\n\n"
            "    ret = _linkg_web_server_start();\n"
            "    if (ret != 0)\n"
            "    {\n"
            "        _linkg_web_speedtest_stop();\n"
            "        return ret;\n"
            "    }",
            filename,
        )
        content = replace_once(
            content,
            "    ret = _linkg_web_server_stop();\n    if (ret != 0)\n    {\n        return ret;\n    }",
            "    ret = _linkg_web_server_stop();\n"
            "    if (ret != 0)\n"
            "    {\n"
            "        _linkg_web_speedtest_stop();\n"
            "        return ret;\n"
            "    }\n\n"
            "    ret = _linkg_web_speedtest_stop();\n"
            "    if (ret != 0)\n"
            "    {\n"
            "        return ret;\n"
            "    }",
            filename,
        )
        content = replace_once(
            content,
            "    ret = _linkg_web_server_deinit();\n    if (ret != 0)\n    {\n        return ret;\n    }",
            "    ret = _linkg_web_server_deinit();\n"
            "    _linkg_web_speedtest_deinit();\n"
            "    if (ret != 0)\n"
            "    {\n"
            "        return ret;\n"
            "    }",
            filename,
        )
    elif filename == "www/index.html":
        content = replace_once(
            content,
            "                                串口诊断\n                            </a>",
            "                                串口诊断\n                            </a>\n"
            "                            <a href=\"#/diagnostics/speedtest\"\n"
            "                                data-route=\"diagnostics/speedtest\"\n"
            "                                data-page=\"/view/diagnostics_speedtest.html\">\n"
            "                                5G 网络测速\n"
            "                            </a>",
            filename,
        )
        content = replace_once(
            content,
            '                        loadStylesheet("/css/pages/diagnostics_uart.css", latestVersion),',
            '                        loadStylesheet("/css/pages/diagnostics_uart.css", latestVersion),\n'
            '                        loadStylesheet("/css/pages/diagnostics_speedtest.css", latestVersion),',
            filename,
        )
        content = replace_once(
            content,
            '                    await loadScript("/js/api/uart_diagnostic.js", latestVersion);',
            '                    await loadScript("/js/api/uart_diagnostic.js", latestVersion);\n'
            '                    await loadScript("/js/api/speedtest.js", latestVersion);',
            filename,
        )
        content = replace_once(
            content,
            '                    await loadScript("/js/pages/diagnostics_uart.js", latestVersion);',
            '                    await loadScript("/js/pages/diagnostics_uart.js", latestVersion);\n'
            '                    await loadScript("/js/pages/diagnostics_speedtest.js", latestVersion);',
            filename,
        )
    else:
        raise ValueError(f"Unexpected patched file: {filename}")
    return content


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_path = tempfile.mkstemp(prefix=".linkg-speedtest-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
        if path.exists():
            shutil.copymode(path, temp_path)
        os.replace(temp_path, path)
    finally:
        if os.path.exists(temp_path):
            os.unlink(temp_path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="LinkG source tree root")
    parser.add_argument("--dry-run", action="store_true", help="Check without modifying files")
    args = parser.parse_args()
    root = args.root.resolve()

    if not (root / "service/web/web_internal.h").is_file() or not (root / "www/index.html").is_file():
        print("ERROR: target does not appear to be a LinkG checkout", file=sys.stderr)
        return 2

    changes: dict[str, bytes] = {}
    for name in PATCHED_FILES:
        raw = (root / name).read_bytes()
        text = raw.decode("utf-8")
        updated = patched(name, text).encode("utf-8")
        if updated != raw:
            changes[name] = updated

    for name in NEW_FILES:
        target = root / name
        new_data = (PACKAGE / name).read_bytes()
        if target.exists():
            if target.read_bytes() != new_data:
                raise ValueError(f"Refusing to overwrite existing nonmatching file: {name}")
        else:
            changes[name] = new_data

    print("Checked LinkG integration anchors successfully")
    for name in changes:
        print("  +", name)
    if args.dry_run or not changes:
        print("Dry run completed" if args.dry_run else "Already installed")
        return 0

    backup_base = root / ".git" if (root / ".git").is_dir() else root
    backup_root = backup_base / "linkg-speedtest-backup" / datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    for name in changes:
        target = root / name
        if target.exists():
            backup = backup_root / name
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(target, backup)

    for name, data in changes.items():
        atomic_write(root / name, data)
    print("Installed successfully. Backup:", backup_root)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, UnicodeError) as exc:
        print("ERROR:", exc, file=sys.stderr)
        sys.exit(1)

#!/usr/bin/env bash
# LinkG upgrade package builder - run from any directory.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 - "$SCRIPT_DIR" <<'PY'
import gzip
import hashlib
import json
import os
import re
import shutil
import stat
import sys
import tarfile
import tempfile
from pathlib import Path

root = Path(sys.argv[1]).resolve()
version_path = root.parent / "VERSION"
if not version_path.is_file():
    raise SystemExit("缺少仓库根目录 VERSION: " + str(version_path))
raw_version = version_path.read_text(encoding="utf-8").strip()
match = re.fullmatch(r"[Vv]([0-9]+\.[0-9]+\.[0-9]+)", raw_version)
if match is None:
    raise SystemExit("VERSION 格式错误，预期 V1.0.0，实际: " + repr(raw_version))
package_version = "v" + match.group(1)
outdir = root / "output"
archive_path = outdir / ("linkg-upgrade-" + package_version + ".tar.gz")
required = (
    "app/bin/linkg",
    "www/index.html",
    "www/cgi-bin/cmain",
    "www/cgi-bin/upgrade",
    "www/version.json",
)
executables = ("app/bin/linkg", "www/cgi-bin/cmain", "www/cgi-bin/upgrade")


def hash_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def walk(root_path):
    return sorted(root_path.rglob("*"), key=lambda p: p.relative_to(root_path).as_posix())


def check_name(relative):
    if any(c in relative for c in "\n\r\t"):
        raise ValueError("控制字符不能出现在升级包路径: " + repr(relative))
    name = relative.rsplit("/", 1)[-1]
    if name.endswith((".swp", ".tmp", "~")) or ".bak" in name or name in (".DS_Store", "Thumbs.db"):
        raise ValueError("请移除临时/备份文件: " + relative)
    if relative == "app/bin/linkg_watchdog.sh":
        raise ValueError("旧版 linkg_watchdog.sh 与 Supervisor 冲突，请从升级内容移除")


def check_tree(staging):
    entries = walk(staging)
    for path in entries:
        relative = path.relative_to(staging).as_posix()
        check_name(relative)
        if path.is_symlink():
            target = os.readlink(path)
            if os.path.isabs(target) or any(c in target for c in "\n\r\t"):
                raise ValueError("不允许绝对路径或非法符号链接: " + relative)
            resolved = path.resolve(strict=True)
            area_root = staging / relative.split("/", 1)[0]
            try:
                resolved.relative_to(area_root)
            except ValueError as exc:
                raise ValueError("符号链接不能跨安装根目录: " + relative) from exc
            if not resolved.is_file():
                raise ValueError("符号链接必须指向包内普通文件: " + relative)
        elif path.is_file() or path.is_dir():
            pass
        else:
            raise ValueError("不允许特殊文件: " + relative)
    return entries


with tempfile.TemporaryDirectory(prefix="linkg-upgrade-build-") as temp_dir:
    staging = Path(temp_dir) / "package"
    staging.mkdir()
    for section in ("app", "www"):
        source = root / section
        if not source.is_dir():
            raise SystemExit("缺少必需目录: " + str(source))
        shutil.copytree(source, staging / section, symlinks=True)

    kernel_image = root / "kernel" / "boot.img"
    if kernel_image.exists():
        if kernel_image.is_symlink() or not kernel_image.is_file():
            raise SystemExit("kernel/boot.img 必须是普通文件")
        (staging / "kernel").mkdir()
        shutil.copy2(kernel_image, staging / "kernel" / "boot.img")

    # Optional repair declaration: sources are relative to the package's app/ tree.
    # The installer, not the archive, selects the inactive /app/releases/vN root.
    system_install_path = root / "system_install.json"
    system_install_included = system_install_path.exists() or system_install_path.is_symlink()
    if system_install_included:
        if system_install_path.is_symlink() or not system_install_path.is_file():
            raise SystemExit("system_install.json 必须是普通文件")
        if system_install_path.stat().st_size > 64 * 1024:
            raise SystemExit("system_install.json 超过 64 KiB 限制")

        def unique_keys(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError("system_install.json 包含重复键: " + key)
                result[key] = value
            return result

        try:
            declaration = json.loads(
                system_install_path.read_text(encoding="utf-8"),
                object_pairs_hook=unique_keys,
            )
        except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
            raise SystemExit("system_install.json 格式错误: " + str(exc)) from exc
        if (not isinstance(declaration, dict)
                or set(declaration) != {"schema_version", "files"}
                or type(declaration["schema_version"]) is not int
                or declaration["schema_version"] != 1
                or not isinstance(declaration["files"], list)
                or not 1 <= len(declaration["files"]) <= 64):
            raise SystemExit("system_install.json 必须包含 schema_version=1 和非空 files 数组")

        def valid_components(value, absolute):
            if not isinstance(value, str) or not value or "\x00" in value:
                return False
            if any(ord(char) < 32 or ord(char) == 127 for char in value):
                return False
            if value.startswith("/") != absolute:
                return False
            components = value.split("/")[1:] if absolute else value.split("/")
            return bool(components) and all(component not in ("", ".", "..") for component in components)

        targets = set()
        for index, item in enumerate(declaration["files"]):
            if not isinstance(item, dict) or set(item) != {"source", "destination"}:
                raise SystemExit(f"system_install.json files[{index}] 必须只包含 source 和 destination")
            source = item["source"]
            destination = item["destination"]
            if not valid_components(source, False):
                raise SystemExit(f"system_install.json files[{index}].source 必须是相对 app/ 的安全路径")
            if not valid_components(destination, True):
                raise SystemExit(f"system_install.json files[{index}].destination 必须是安全绝对路径")
            if (destination != "/usr/sbin/linkg-supervisor"
                    and not destination.startswith(("/vendor/firmware/", "/lib/firmware/"))):
                raise SystemExit("system_install.json 不允许覆盖该系统路径: " + destination)
            if destination in targets:
                raise SystemExit("system_install.json 重复目标路径: " + destination)
            targets.add(destination)
            source_file = staging / source if source.startswith("app/") else staging / "app" / source
            if source_file.is_symlink() or not source_file.is_file():
                raise SystemExit("system_install.json 指定的源文件不存在或不是普通文件: " + source)
            if destination == "/usr/sbin/linkg-supervisor" and not os.access(source_file, os.X_OK):
                raise SystemExit("system_install.json 中 Supervisor 源文件没有执行权限: " + source)
        shutil.copy2(system_install_path, staging / "system_install.json")

    for relative in required:
        path = staging / relative
        if path.is_symlink() or not path.is_file():
            raise SystemExit("缺少必需普通文件: " + relative)
    for relative in executables:
        if not os.access(staging / relative, os.X_OK):
            raise SystemExit("必需程序没有执行权限: " + relative)

    # Keep the manually maintained www/version.json unchanged in the upgrade package.

    entries = check_tree(staging)
    manifest_lines = []
    total_bytes = 0
    for path in entries:
        relative = path.relative_to(staging).as_posix()
        if path.is_symlink():
            target = os.readlink(path)
            data = target.encode("utf-8")
            manifest_lines.append("L\t{}\t{}\t{}\t{}".format(
                len(data), hashlib.sha256(data).hexdigest(), relative, target))
        elif path.is_file():
            size = path.stat().st_size
            total_bytes += size
            manifest_lines.append("F\t{}\t{}\t{}".format(size, hash_file(path), relative))
    if total_bytes > 128 * 1024 * 1024:
        raise SystemExit("安装文件总大小超过 128 MiB 限制")

    manifest = staging / "FILES.txt"
    manifest.write_text("\n".join(manifest_lines) + "\n", encoding="utf-8")

    outdir.mkdir(parents=True, exist_ok=True)
    temporary_archive = outdir / (".linkg-upgrade-{}.tmp".format(os.getpid()))
    try:
        with temporary_archive.open("wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=0, filename="") as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as tar:
                # Every member belongs to the single archive root: upgrade/.
                top = tarfile.TarInfo("upgrade/")
                top.type = tarfile.DIRTYPE
                top.mode = 0o755
                top.uid = top.gid = 0
                top.uname = top.gname = ""
                top.mtime = 0
                tar.addfile(top)
                for path in walk(staging):
                    info = tar.gettarinfo(str(path), arcname="upgrade/" + path.relative_to(staging).as_posix())
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mtime = 0
                    if info.isreg():
                        with path.open("rb") as contents:
                            tar.addfile(info, contents)
                    else:
                        tar.addfile(info)
                # FILES.txt is already part of walk(staging); do not add it twice.
        if temporary_archive.stat().st_size > 64 * 1024 * 1024:
            raise ValueError("压缩包超过现有 CGI 64 MiB 上限")
        os.replace(temporary_archive, archive_path)
    finally:
        temporary_archive.unlink(missing_ok=True)

    def human_size(size):
        if size >= 1024 * 1024:
            return f"{size / (1024 * 1024):.2f} MiB"
        if size >= 1024:
            return f"{size / 1024:.2f} KiB"
        return f"{size} B"

    print("软件版本:", package_version)
    print("升级包生成成功:", archive_path)
    print("普通文件/符号链接记录:", len(manifest_lines))
    print("未压缩文件总大小:", human_size(total_bytes))
    print("压缩包大小:", human_size(archive_path.stat().st_size))
    print("压缩包顶层目录: upgrade/")
    print("Kernel:", "included" if kernel_image.exists() else "not included (app-only)")
    print("系统文件替换声明:", "included" if system_install_included else "not included")
    print("提示: 安装后端应严格验证 FILES.txt 每一行及归档文件全集")
PY

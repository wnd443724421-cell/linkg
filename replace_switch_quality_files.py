#!/usr/bin/env python3
"""Safely replace two LinkG source files using semantic context rather than git apply.

Run this from the LinkG repository root:
    python3 replace_switch_quality_files.py

Keep the accompanying switch_observation_internal.patch in the same directory.
Creates .before_quality_update.bak backups before writing either source file.
"""
from pathlib import Path
from dataclasses import dataclass
import re
import sys

@dataclass
class Hunk:
    old: str
    new: str


def read_hunks(patch_text: str) -> dict[str, list[Hunk]]:
    files: dict[str, list[Hunk]] = {}
    current = None
    old_lines = []
    new_lines = []
    inside = False

    def finish():
        nonlocal old_lines, new_lines
        if inside and current:
            files.setdefault(current, []).append(Hunk(''.join(old_lines), ''.join(new_lines)))
        old_lines, new_lines = [], []

    for line in patch_text.splitlines(keepends=True):
        if line.startswith('diff --git '):
            finish()
            inside = False
            current = line.split(' b/', 1)[1].strip()
        elif line.startswith('@@ '):
            finish()
            inside = True
        elif inside and line.startswith(' '):
            old_lines.append(line[1:])
            new_lines.append(line[1:])
        elif inside and line.startswith('-'):
            old_lines.append(line[1:])
        elif inside and line.startswith('+'):
            new_lines.append(line[1:])
        elif inside and line.startswith('\\ No newline'):
            pass
        elif inside:
            raise ValueError(f'Unexpected patch data: {line!r}')
    finish()
    return files


def handle_custom_internal_header(text: str) -> tuple[str, int]:
    """Add only the two missing fields even if other local header lines differ."""
    fields = (
        ('remote_wifi_uplink_loss;', 'remote_wifi_uplink_classes[LINKG_TRANSPORT_CLASS_COUNT];',
         'AP上报的STA到AP Wi-Fi三业务Wire丢包'),
        ('remote_cellular_uplink_loss;', 'remote_cellular_uplink_classes[LINKG_TRANSPORT_CLASS_COUNT];',
         'AP上报的STA到AP Cellular三业务Wire丢包'),
    )
    lines = text.splitlines(keepends=True)
    changed = 0
    for anchor, name, comment in fields:
        if name in ''.join(lines):
            continue
        matches = [i for i, line in enumerate(lines) if anchor in line and not line.lstrip().startswith('//')]
        if len(matches) != 1:
            raise ValueError(f'找不到唯一的内部运行态字段：{anchor}')
        i = matches[0]
        original = lines[i]
        indent = original[:len(original)-len(original.lstrip())]
        insert = f'{indent}linkg_switch_loss_observation_t  {name:<50} // {comment}\n'
        lines.insert(i + 1, insert)
        changed += 1
    return ''.join(lines), changed


def main() -> None:
    root = Path.cwd()
    package_dir = Path(__file__).resolve().parent
    patch = package_dir / 'switch_observation_internal.patch'
    if not patch.exists():
        raise FileNotFoundError('缺少 switch_observation_internal.patch，请与脚本放在同一目录')
    hunks = read_hunks(patch.read_text(encoding='utf-8'))
    staged = {}
    for name in ('core/switch/switch_observation.c', 'core/switch/switch_internal.h'):
        target = root / name
        if not target.is_file():
            raise FileNotFoundError(f'找不到源文件：{target}')
        content = target.read_text(encoding='utf-8')
        if name.endswith('switch_internal.h'):
            output, changed = handle_custom_internal_header(content)
        else:
            output, changed = content, 0
            for idx, hunk in enumerate(hunks.get(name, ()), 1):
                if hunk.old in output:
                    output = output.replace(hunk.old, hunk.new, 1)
                    changed += 1
                elif hunk.new in output:
                    continue
                else:
                    raise ValueError(f'{name} 第{idx}处与预期源码差异较大；没有写入任何文件')
        staged[target] = (content, output, changed)

    for target, (old, new, changed) in staged.items():
        if old != new:
            backup = target.with_name(target.name + '.before_quality_update.bak')
            if backup.exists():
                raise FileExistsError(f'备份已存在，避免覆盖：{backup}')
            backup.write_text(old, encoding='utf-8')
            target.write_text(new, encoding='utf-8')
            print(f'完成：{target.relative_to(root)}  修改段落={changed}  备份={backup.name}')
        else:
            print(f'已包含修改：{target.relative_to(root)}')
    print('已生成完整替换后的两个C源文件，保留其他原有内容。')

if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print(f'操作中止：{exc}', file=sys.stderr)
        sys.exit(1)

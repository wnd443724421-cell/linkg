#!/usr/bin/env python3
"""Audit application style and independent Keil/GCC input paths; no vendor edits."""
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
failures = []
def check(condition, message):
    if not condition:
        failures.append(message)

sources = sorted(p for folder in ('Core', 'bsp', 'service', 'tests')
                 for p in (root / folder).rglob('*') if p.suffix in ('.c', '.h'))
functions = 0
for path in sources:
    text = path.read_text(encoding='utf-8')
    name = path.relative_to(root).as_posix()
    check('\t' not in text, f'{name}: tab')
    check(text.endswith('\n'), f'{name}: missing final newline')
    check(not re.search(r'[^\n]\n\n\n', text), f'{name}: repeated blank line')
    check(f'@file {path.name}' in text, f'{name}: file header')
    if path.suffix == '.c':
        for field in ('@author Dawn', '@version 2.0.0', '@date 2026-10-14'):
            check(field in text, f'{name}: {field}')
    check(not re.search(r'/\*(?!\*)', text), f'{name}: ordinary block comment')
    for line_no, line in enumerate(text.splitlines(), 1):
        check(line == line.rstrip(), f'{name}:{line_no}: trailing spaces')
        if line.startswith(' '):
            indent = len(line) - len(line.lstrip(' '))
            check(line.lstrip().startswith('*') or indent % 4 == 0,
                  f'{name}:{line_no}: indentation')
        if line.lstrip().startswith('/***'):
            check(re.fullmatch(r'/\*{30} .+ \*{30}/', line.strip()) is not None,
                  f'{name}:{line_no}: section divider')
        if re.search(r'\)\s*{', line):
            failures.append(f'{name}:{line_no}: brace on signature/control line')
    definitions = list(re.finditer(r'^#define (\S+)(.*?)// (.+)$', text, re.M))
    check(len(definitions) == len(re.findall(r'^#define ', text, re.M)), f'{name}: macro comment')
    value_columns = {m.start(2) - m.start() + len(m[2]) - len(m[2].lstrip())
                     for m in definitions if m[2].strip()}
    comment_columns = {m[0].index('//') for m in definitions}
    check(len(value_columns) <= 1, f'{name}: file-wide macro value alignment')
    check(len(comment_columns) <= 1, f'{name}: file-wide macro comment alignment')
    for block in re.finditer(r'typedef struct\s*\n\{(.*?)\n\}', text, re.S):
        lines = [l for l in block[1].splitlines() if ';' in l]
        check(all('//' in l for l in lines), f'{name}: struct member comments')
        check(len({l.index('//') for l in lines if '//' in l}) <= 1, f'{name}: struct comment alignment')
        cols = set()
        for line in lines:
            match = re.search(r'(\*?[A-Za-z_]\w*)(?:\[[^;]+\])?;', line)
            if match:
                cols.add(match.start())
        check(len(cols) <= 1, f'{name}: struct member name alignment')
    for block in re.finditer(r'typedef enum\s*\n\{(.*?)\n\}', text, re.S):
        check(all('//' in l for l in block[1].splitlines() if l.strip()), f'{name}: enum comments')
    if path.suffix == '.h':
        check(not re.search(r'\([^;{}]*\n[^;{}]*\);', text), f'{name}: multiline prototype')
        check('@brief' not in text[text.index('*/') + 2:], f'{name}: per-function header comments')
        for block in re.split(r'/\*{30}.*?\*{30}/', text):
            protos = re.findall(r'^.*?\b([A-Za-z_]\w*)\([^;{}]*\);$', block, re.M)
            columns = set()
            for line in block.splitlines():
                for proto in protos:
                    if re.search(r'\b' + proto + r'\(', line):
                        columns.add(line.index(proto))
            check(len(columns) <= 1, f'{name}: prototype alignment')
    else:
        for match in re.finditer(r'^(?:static\s+)?[A-Za-z_]\w*[\w\s*]*\([^;{}]*\)\n\{', text, re.M):
            functions += 1
            prefix = text[:match.start()].rstrip()
            start = prefix.rfind('/**')
            check(prefix.endswith('*/') and start >= 0 and '@brief' in prefix[start:],
                  f'{name}: function brief near {match[0][:70]}')

tree = ET.parse(root / 'MDK-ARM/soft.uvprojx')
base = root / 'MDK-ARM'
paths = tree.findall('.//Groups/Group/Files/File/FilePath')
for el in paths:
    resolved = (base / el.text.replace('\\', '/')).resolve()
    check(resolved.is_file(), f'Keil missing file: {el.text}')
    check(root in resolved.parents, f'Keil external file: {el.text}')
for el in tree.findall('.//Cads/VariousControls/IncludePath'):
    for part in el.text.split(';'):
        resolved = (base / part).resolve()
        check(resolved.is_dir(), f'Keil missing include: {part}')
        check(root in resolved.parents, f'Keil external include: {part}')
check(tree.findtext('.//Device') == 'STM32L031G6Ux', 'Keil device')
check(tree.findtext('.//IRAM/Size') == '0x2000', 'Keil RAM size')
check(tree.findtext('.//IROM/Size') == '0x8000', 'Keil Flash size')
check(tree.findtext('.//UpdateFlashBeforeDebugging') == '0', 'Keil implicit flash')
makefile = (root / 'Makefile').read_text()
keil_c = {Path(el.text).name for el in paths if el.text.endswith('.c')}
gcc_c = set(re.findall(r'(\w+\.c)', makefile.split('OBJECTS :=')[0]))
check(keil_c == gcc_c, f'Keil/GCC source mismatch: {keil_c ^ gcc_c}')
for path in [root / 'Makefile', root / 'MDK-ARM/soft.uvprojx', root / 'MDK-ARM/soft.uvoptx'] + sources:
    text = path.read_text(encoding='utf-8')
    check(not re.search(r'mcu_soft|user_main\.c|usart_server\.c|115200', text), f'{path.name}: stale dependency')
    if path.parent.name in ('bsp', 'service'):
        check(not re.search(r'->(?:gState|RxState)\s*=', text), f'{path.name}: HAL state mutation')
        check(not re.search(r'HAL_UART_(?:Transmit|Receive)\s*\(', text), f'{path.name}: blocking UART')
        check(not re.search(r'\b(?:malloc|calloc|realloc|free)\s*\(', text), f'{path.name}: dynamic allocation')

if failures:
    print('\n'.join(failures))
    sys.exit(1)
print(f'PASS: {len(sources)} C/H files; {functions} function briefs; formatting; {len(paths)} Keil inputs; independent paths and source parity')

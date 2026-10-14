#!/usr/bin/env python3
"""Validate the actual linked ARM image, vectors, bounds and forbidden symbols."""
from pathlib import Path
import hashlib
import re
import struct
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
prefix = sys.argv[1] if len(sys.argv) > 1 else 'arm-none-eabi-'
elf = root / 'build/gcc/linkg_mcu_v2.elf'
binary = elf.with_suffix('.bin')

def run(tool, *args):
    return subprocess.check_output([prefix + tool, *args, str(elf)], text=True)

nm = run('nm', '-n')
symbols = {}
for line in nm.splitlines():
    parts = line.split()
    if len(parts) == 3:
        symbols[parts[2]] = int(parts[0], 16)
assert run('nm', '-u').strip() == '', 'Undefined symbol in firmware'
for name in ('malloc', 'calloc', 'realloc', 'free', '_sbrk', 'HAL_UART_Transmit', 'HAL_UART_Receive',
             'HAL_UART_IRQHandler', 'user_main', 'usart_server'):
    assert name not in symbols, f'Unexpected linked symbol: {name}'
header = run('readelf', '-h', '-A', '-l')
assert 'Machine:                           ARM' in header
assert 'Microcontroller' in header
assert 'Thumb-1' in header
assert 'INTERP' not in header and 'DYNAMIC' not in header
blob = binary.read_bytes()
assert len(blob) <= 32768
vector = struct.unpack_from('<48I', blob)
assert vector[0] == 0x20002000
for slot, name in [(1, 'Reset_Handler'), (2, 'NMI_Handler'), (3, 'HardFault_Handler'),
                   (11, 'SVC_Handler'), (14, 'PendSV_Handler'), (15, 'SysTick_Handler'),
                   (44, 'USART2_IRQHandler'), (45, 'LPUART1_IRQHandler')]:
    assert vector[slot] == symbols[name] | 1, (slot, name, hex(vector[slot]))
for entry in vector[1:]:
    assert entry == 0 or entry & 1 and 0x08000000 <= entry < 0x08008000
assert symbols['_sdata'] == 0x20000000
assert symbols['_edata'] <= symbols['_sbss'] <= symbols['_ebss'] <= symbols['_sstack']
assert symbols['_sstack'] == 0x20001C00
assert symbols['_estack'] - symbols['_sstack'] == 1024
assert symbols['_sidata'] - 0x08000000 + symbols['_edata'] - symbols['_sdata'] == len(blob)
data_bytes = symbols['_edata'] - symbols['_sdata']
bss_bytes = symbols['_ebss'] - symbols['_sbss']
print('PASS: ARMv6-M Thumb image, 48 vectors, initial SP, IRQ entries, data/BSS/stack bounds, no undefined/dynamic/protocol symbols')
print(f'Flash image: {len(blob)} / 32768 bytes')
print(f'Static RAM: data {data_bytes} + BSS {bss_bytes} = {data_bytes + bss_bytes} / 8192 bytes')
print(f'Reserved stack: 1024 bytes; gap before stack: {symbols["_sstack"] - symbols["_ebss"]} bytes')
print('The linker memory-region report shows RAM 100% because .stack is placed at the top of RAM; it is not static data usage.')
for path in (elf, binary, elf.with_suffix('.hex')):
    print(f'SHA256 {path.name}: {hashlib.sha256(path.read_bytes()).hexdigest()}')

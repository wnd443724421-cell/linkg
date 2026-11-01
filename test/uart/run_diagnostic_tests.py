"""Build the production diagnostic task against controlled lifecycle adapters."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
output = root / 'build/uart-test'
output.mkdir(parents=True, exist_ok=True)
includes = sorted({str(p.parent) for folder in ('include', 'service', 'third_party')
                   for p in (root / folder).rglob('*.h')})
command = ['gcc', '-std=gnu17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
command += ['-I' + directory for directory in includes]
command += [str(root / 'service/uart/uart_diagnostic.c'),
            str(root / 'infra/base/linkg_thread.c'),
            str(root / 'test/uart/uart_diagnostic_lifecycle_test.c'),
            '-Wl,--wrap=pthread_create', '-Wl,--wrap=pthread_join', '-Wl,--wrap=eventfd',
            '-Wl,--wrap=linkg_thread_wakeup', '-Wl,--wrap=poll', '-Wl,--wrap=__poll_chk',
            '-o', str(output / 'uart_diagnostic_lifecycle_test')]
subprocess.run(command, check=True)
subprocess.run([str(output / 'uart_diagnostic_lifecycle_test')], check=True)

console_command = command[:command.index(str(root / 'service/uart/uart_diagnostic.c'))]
console_command += [str(root / 'service/uart/uart_diagnostic_test.c'),
                    str(root / 'test/uart/uart_diagnostic_console_test.c'),
                    '-Wl,--wrap=fopen', '-o', str(output / 'uart_diagnostic_console_test')]
subprocess.run(console_command, check=True)
subprocess.run([str(output / 'uart_diagnostic_console_test')], check=True)

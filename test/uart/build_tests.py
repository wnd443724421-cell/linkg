"""Build real UART/config/Web code with isolated GPIO and PTY test adapters."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[2]
os.chdir(root)
output = root / 'build/uart-test'
output.mkdir(parents=True, exist_ok=True)
includes = sorted({str(p.parent) for folder in ('include', 'infra', 'service', 'platform', 'modules', 'third_party') for p in Path(folder).rglob('*.h')})
sources = [str(p) for p in Path('service/uart').glob('*.c')]
sources += [str(p) for p in Path('infra/config').glob('*.c')]
sources += ['infra/base/linkg_thread.c', 'infra/base/linkg_time.c', 'infra/base/linkg_file.c',
            'infra/base/linkg_network_ops.c', 'modules/wifi/linkg_wifi_ops.c',
            'infra/base/linkg_json.c', 'platform/uart/linkg_uart.c', 'third_party/cjson/cJSON.c',
            'service/web/web_response.c', 'service/web/web_dispatch.c', 'service/web/web_server.c',
            'service/web/handlers/web_handler_uart.c', 'test/uart/uart_test_server.c']
wraps = ['linux_uart_open', 'linkg_thread_init', 'linkg_thread_start', 'linkg_thread_stop',
         'linkg_file_read_all', 'linkg_file_write_all_atomic']
command = ['gcc', '-std=gnu17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', '-ffunction-sections', '-Wl,--gc-sections',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
command += ['-I' + directory for directory in includes]
command += sources + ['-Wl,--wrap=' + name for name in wraps] + ['-lm', '-lutil', '-o', str(output / 'uart_test_server')]
print(' '.join(command), flush=True)
subprocess.run(command, check=True)

platform_command = [
    'gcc', '-std=gnu17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
    '-Iinclude/linkg/platform/uart', 'platform/uart/linkg_uart.c', 'test/uart/uart_platform_test.c',
]
platform_command += ['-Wl,--wrap=' + name for name in ('open', 'close', 'ioctl', 'tcgetattr', 'tcsetattr', 'tcflush', 'write')]
platform_command += ['-o', str(output / 'uart_platform_test')]
print(' '.join(platform_command), flush=True)
subprocess.run(platform_command, check=True)
subprocess.run([str(output / 'uart_platform_test')], check=True)


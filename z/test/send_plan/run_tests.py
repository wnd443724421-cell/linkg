"""Build real LinkG plan/scheduler/RX code with isolated network adapters."""
from pathlib import Path
import subprocess
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--thread-sanitizer', action='store_true')
args = parser.parse_args()

root = Path(__file__).resolve().parents[2]
output = root / 'build' / 'class-plan-tests'
output.mkdir(parents=True, exist_ok=True)
includes = sorted({str(p.parent) for folder in ('include', 'core', 'infra', 'modules', 'platform', 'service', 'third_party') for p in (root / folder).rglob('*.h')})
command = ['gcc', '-std=gnu17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
           '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
           '-fsanitize=thread' if args.thread_sanitizer else '-fsanitize=address,undefined',
           '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
command += ['-I' + name for name in includes]
for name in ('plan_test', 'audit_test', 'hostapd_test'):
    sources = [root / 'test/send_plan' / (name + '.c')]
    sources += [root / 'infra/base/linkg_text.c'] if name == 'hostapd_test' else [root / 'infra/base/linkg_json.c', root / 'third_party/cjson/cJSON.c']
    executable = output / (name + ('_tsan' if args.thread_sanitizer else ''))
    subprocess.run(command + [str(source) for source in sources] + ['-lm', '-o', str(executable)], check=True, cwd=root)
    subprocess.run([str(executable)], check=True, cwd=root)

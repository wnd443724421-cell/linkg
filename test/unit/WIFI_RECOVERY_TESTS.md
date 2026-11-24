# Wi-Fi恢复回归测试

AP检测和Runtime用例移植自LQ10。STA接管用例编译实际Owner、Monitor、Runtime，注入WPA服务成功启动但事件监听建立失败，验证连续失败前后动态配置互斥；不运行真实无线服务。使用CMake生成的compile_commands.json中Wi-Fi Owner的include及宏定义参数；测试独立编译，不链接生产Owner目标文件。

```sh
# 在仓库根目录，设置为实际x86构建目录下的compile_commands.json
python3 - build/x86/compile_commands.json <<'PY'
import json, shlex, subprocess, sys
from pathlib import Path
entries = json.loads(Path(sys.argv[1]).read_text())
entry = next(e for e in entries if e["file"].endswith("/modules/wifi/linkg_wifi.c"))
args = entry.get("arguments") or shlex.split(entry["command"])
flags = [a for a in args[1:] if a.startswith(("-I", "-D"))]
for name, sources in [("test_wifi_runtime", ["modules/wifi/wifi_runtime.c"]), ("test_wifi_ap_monitor", ["modules/wifi/wifi_monitor.c"]), ("test_wifi_recovery_takeover", ["modules/wifi/wifi_monitor.c", "modules/wifi/wifi_runtime.c"])]:
    subprocess.run(["gcc", "-std=gnu17", "-Wall", "-Wextra", "-ffunction-sections", "-fdata-sections", *flags, "test/unit/" + name + ".c", *sources, "-Wl,--gc-sections", "-pthread", "-o", "/tmp/" + name], check=True)
    subprocess.run(["/tmp/" + name], check=True)
PY
```

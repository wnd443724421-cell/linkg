# GPIO 资源生命周期回归

`platform/gpio/linkg_gpio.c` 使用 Linux GPIO 字符设备 v1 接口，通过 `/dev/gpiochip0` 申请单引脚句柄。T113 当前主 PIO 的 base 为 0、引脚数为 224，继续使用 PA0～PG31 映射。输出方向与初始电平在一次申请中设置，不改变公开 API 或 GPIO 管理器的业务电平。

正常关闭、崩溃和 SIGKILL 最终关闭最后一个引脚句柄时，内核 `linehandle_release()` 调用 `gpiod_free()`。句柄设置 `FD_CLOEXEC`，外部命令执行后不会继续持有引脚。其他程序或驱动占用的引脚仍返回 EBUSY；缺少设备或权限时返回原始错误，不回退到 sysfs。

## 运行测试

在仓库根目录执行：

```bash
cmake -S test/gpio -B /tmp/linkg-gpio-tests -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/linkg-gpio-tests -j2
ctest --test-dir /tmp/linkg-gpio-tests --output-on-failure
/tmp/linkg-gpio-tests/linkg_gpio_tests
```

测试链接真实 GPIO 模块，通过链接器包装 `open/close/ioctl/fcntl` 模拟 GPIO 驱动，使用真实文件描述符、跨进程文件锁、fork、exec、SIGKILL 和 waitpid 验证资源生命周期，不读写实际引脚。覆盖方向、初始值、读写、无效参数、占用冲突、正常关闭、设备缺失、权限失败、ioctl 错误与 EINTR、句柄标志设置失败、25 次强杀重开以及 exec 后释放。错误路径检查句柄计数归零。

可使用已有 ARM 工具链编译相同测试，在板上独立执行；这些仍是模拟驱动测试，不能代替实际硬件 GPIO 申请与电平验证。

## 部署与验证边界

首次从 sysfs 旧版切换时，先正常停止旧 LinkG，等待其释放 export 的引脚，再启动新版。旧进程已被强杀而留下 sysfs export 时，需要在维护期间确认归属后清理，或重启设备。新版不会自动抢占或 unexport 其他持有者。

关闭句柄只保证释放引脚的申请关系，不承诺板级电平回到安全值或外设自动复位。正常 GPIO 清理流程和 Supervisor 现有的 SIGTERM 宽限仍然需要保留。fork 后未 exec 的长驻子进程可能继承句柄；当前 LinkG 外部命令通过 exec 启动，FD_CLOEXEC 用于该路径。

实板仍需验证新版 LinkG 的正常启动与退出、强杀后重新拉起、各控制引脚的电平及受控外设恢复。Wi-Fi、蜂窝、串口等其他资源的强杀恢复不在本补丁的保证范围内。

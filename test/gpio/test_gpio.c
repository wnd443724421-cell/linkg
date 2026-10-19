/**
 * @file test_gpio.c
 * @brief 模拟GPIO字符设备并验证资源生命周期与故障路径
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-19
 */

#include "linkg_gpio.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

/****************************** 测试常量 ******************************/

#define TEST_DESCRIPTOR_COUNT 1024U                               // 模拟句柄表容量
#define TEST_LINE_COUNT       224U                                // 板上主PIO引脚数量
#define TEST_KILL_CYCLES      25U                                 // 强杀后重新申请循环次数
#define TEST_CHECK(condition) _test_check((condition), __LINE__)  // 保留失败断言的位置

/****************************** 测试状态 ******************************/

static char          g_directory[] = "/tmp/linkg-gpio-test-XXXXXX";
static unsigned int  g_kinds[TEST_DESCRIPTOR_COUNT];
static unsigned int  g_active;
static unsigned int  g_checks;
static unsigned int  g_lines = TEST_LINE_COUNT;
static unsigned int  g_last_offset;
static unsigned int  g_last_flags;
static int           g_last_line = -1;
static int           g_open_error;
static int           g_fcntl_error;
static int           g_fcntl_command;
static int           g_ioctl_error;
static unsigned long g_ioctl_command;

/****************************** 系统调用声明 ******************************/

int __real_open(const char *path, int flags, ...);
int __real_close(int descriptor);
int __real_fcntl(int descriptor, int command, ...);
int __wrap_open(const char *path, int flags, ...);
int __wrap_close(int descriptor);
int __wrap_fcntl(int descriptor, int command, ...);
int __wrap_ioctl(int descriptor, unsigned long command, ...);

/****************************** 测试辅助 ******************************/

/**
 * @brief 检查断言并在失败时报告位置。
 */
static void _test_check(bool condition, int line)
{
    g_checks++;
    if (!condition)
    {
        fprintf(stderr, "GPIO check failed at line %d, errno=%d\n", line, errno);
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief 记录使用真实文件描述符承载的模拟设备句柄。
 */
static int _test_track(int descriptor, unsigned int kind)
{
    TEST_CHECK(descriptor >= 0 && (unsigned int)descriptor < TEST_DESCRIPTOR_COUNT);
    TEST_CHECK(g_kinds[descriptor] == 0U);
    g_kinds[descriptor] = kind;
    g_active++;
    return descriptor;
}

/**
 * @brief 模拟设备节点打开及缺失、权限错误。
 */
int __wrap_open(const char *path, int flags, ...)
{
    TEST_CHECK(strcmp(path, "/dev/gpiochip0") == 0);
    TEST_CHECK((flags & O_CLOEXEC) != 0);
    if (g_open_error != 0)
    {
        errno = g_open_error;
        g_open_error = 0;
        return -1;
    }

    return _test_track(__real_open("/dev/null", flags), 1U);
}

/**
 * @brief 关闭真实句柄，同时检查错误路径是否完整释放资源。
 */
int __wrap_close(int descriptor)
{
    if (descriptor >= 0 && (unsigned int)descriptor < TEST_DESCRIPTOR_COUNT && g_kinds[descriptor] != 0U)
    {
        g_kinds[descriptor] = 0U;
        g_active--;
    }

    return __real_close(descriptor);
}

/**
 * @brief 模拟句柄标志设置失败，其他操作由真实内核执行。
 */
int __wrap_fcntl(int descriptor, int command, ...)
{
    va_list arguments;
    int     flags;

    if (g_fcntl_error != 0 && command == g_fcntl_command)
    {
        errno = g_fcntl_error;
        g_fcntl_error = 0;
        return -1;
    }

    if (command == F_GETFD)
    {
        return __real_fcntl(descriptor, command);
    }

    TEST_CHECK(command == F_SETFD);
    va_start(arguments, command);
    flags = va_arg(arguments, int);
    va_end(arguments);
    return __real_fcntl(descriptor, command, flags);
}

/**
 * @brief 使用文件锁模拟内核独占申请，锁随真实句柄关闭而释放。
 */
static int _test_request(struct gpiohandle_request *request)
{
    char          path[256];
    unsigned char value;
    int           descriptor;
    int           saved_error;

    TEST_CHECK(request->lines == 1U);
    TEST_CHECK(strcmp(request->consumer_label, "linkg") == 0);
    TEST_CHECK(request->lineoffsets[0] < g_lines);
    g_last_offset = request->lineoffsets[0];
    g_last_flags  = request->flags;
    TEST_CHECK(snprintf(path, sizeof(path), "%s/%u", g_directory, g_last_offset) > 0);
    descriptor = __real_open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    TEST_CHECK(descriptor >= 0);
    if (flock(descriptor, LOCK_EX | LOCK_NB) < 0)
    {
        saved_error = errno;
        (void)__real_close(descriptor);
        errno = saved_error == EWOULDBLOCK ? EBUSY : saved_error;
        return -1;
    }

    value = request->default_values[0];
    TEST_CHECK(pwrite(descriptor, &value, sizeof(value), 0) == (ssize_t)sizeof(value));
    request->fd = _test_track(descriptor, 2U);
    g_last_line = descriptor;
    return 0;
}

/**
 * @brief 模拟GPIO v1控制命令，支持注入中断及硬件错误。
 */
int __wrap_ioctl(int descriptor, unsigned long command, ...)
{
    struct gpiohandle_data *data;
    va_list                 arguments;
    void                   *argument;

    TEST_CHECK(descriptor >= 0 && (unsigned int)descriptor < TEST_DESCRIPTOR_COUNT);
    TEST_CHECK(g_kinds[descriptor] != 0U);
    va_start(arguments, command);
    argument = va_arg(arguments, void *);
    va_end(arguments);
    if (g_ioctl_error != 0 && command == g_ioctl_command)
    {
        errno = g_ioctl_error;
        g_ioctl_error = 0;
        return -1;
    }

    if (command == GPIO_GET_CHIPINFO_IOCTL)
    {
        TEST_CHECK(g_kinds[descriptor] == 1U);
        ((struct gpiochip_info *)argument)->lines = g_lines;
        return 0;
    }

    if (command == GPIO_GET_LINEHANDLE_IOCTL)
    {
        TEST_CHECK(g_kinds[descriptor] == 1U);
        return _test_request(argument);
    }

    TEST_CHECK(g_kinds[descriptor] == 2U);
    data = argument;
    if (command == GPIOHANDLE_GET_LINE_VALUES_IOCTL)
    {
        return pread(descriptor, data->values, 1U, 0) == 1 ? 0 : -1;
    }

    TEST_CHECK(command == GPIOHANDLE_SET_LINE_VALUES_IOCTL);
    return pwrite(descriptor, data->values, 1U, 0) == 1 ? 0 : -1;
}

/****************************** 回归场景 ******************************/

/**
 * @brief 验证映射、电平、独占和正常关闭。
 */
static void _test_operations(void)
{
    linux_gpio_t *gpio;
    linux_gpio_t *other;
    bool          value;

    TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_OUTPUT, true, &gpio) == 0);
    TEST_CHECK(g_last_offset == 112U && g_last_flags == GPIOHANDLE_REQUEST_OUTPUT);
    TEST_CHECK((__real_fcntl(g_last_line, F_GETFD) & FD_CLOEXEC) != 0);
    TEST_CHECK(linux_gpio_read(gpio, &value) == 0 && value);
    TEST_CHECK(linux_gpio_write(gpio, false) == 0);
    TEST_CHECK(linux_gpio_read(gpio, &value) == 0 && !value);
    TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, &other) == -EBUSY && other == NULL);
    TEST_CHECK(linux_gpio_read(gpio, &value) == 0 && !value);
    linux_gpio_close(gpio);
    TEST_CHECK(g_active == 0U);
    TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, &gpio) == 0);
    TEST_CHECK(g_last_flags == GPIOHANDLE_REQUEST_INPUT);
    TEST_CHECK(linux_gpio_write(gpio, true) == -EPERM);
    TEST_CHECK(linux_gpio_read(gpio, NULL) == -EINVAL);
    linux_gpio_close(gpio);
    linux_gpio_close(NULL);
    TEST_CHECK(linux_gpio_read(NULL, &value) == -EINVAL);
    TEST_CHECK(linux_gpio_write(NULL, false) == -EINVAL);
}

/**
 * @brief 验证输入检查及申请失败后无句柄泄漏。
 */
static void _test_failures(void)
{
    const char    *invalid[]  = {NULL, "", "P", "PA", "PH0", "PA32", "PD-1", "PD16x", "PD9999999999"};
    unsigned long  commands[] = {GPIO_GET_CHIPINFO_IOCTL, GPIO_GET_LINEHANDLE_IOCTL};
    linux_gpio_t  *gpio;
    size_t         index;
    bool           value;

    for (index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); index++)
    {
        TEST_CHECK(linux_gpio_open(invalid[index], LINUX_GPIO_DIRECTION_OUTPUT, false, &gpio) == -EINVAL && gpio == NULL);
    }

    TEST_CHECK(linux_gpio_open("PD16", (linux_gpio_direction_t)2, false, &gpio) == -EINVAL);
    TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, NULL) == -EINVAL);
    g_lines = 32U;
    TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, &gpio) == -EINVAL);
    g_lines      = TEST_LINE_COUNT;
    g_open_error = ENOENT;
    TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, &gpio) == -ENOENT);
    g_open_error = EACCES;
    TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, &gpio) == -EACCES);
    for (index = 0U; index < sizeof(commands) / sizeof(commands[0]); index++)
    {
        g_ioctl_command = commands[index];
        g_ioctl_error   = EIO;
        TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, &gpio) == -EIO && gpio == NULL);
        TEST_CHECK(g_active == 0U);
        g_ioctl_error = EINTR;
        TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, &gpio) == 0);
        linux_gpio_close(gpio);
    }

    g_fcntl_command = F_GETFD;
    g_fcntl_error   = EIO;
    TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, &gpio) == -EIO && gpio == NULL);
    TEST_CHECK(g_active == 0U);
    g_fcntl_command = F_SETFD;
    g_fcntl_error   = EIO;
    TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, &gpio) == -EIO && gpio == NULL);
    TEST_CHECK(g_active == 0U);
    TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_OUTPUT, false, &gpio) == 0);
    g_ioctl_command = GPIOHANDLE_SET_LINE_VALUES_IOCTL;
    g_ioctl_error   = EIO;
    TEST_CHECK(linux_gpio_write(gpio, true) == -EIO);
    g_ioctl_error = EINTR;
    TEST_CHECK(linux_gpio_write(gpio, true) == 0);
    g_ioctl_command = GPIOHANDLE_GET_LINE_VALUES_IOCTL;
    g_ioctl_error   = EIO;
    TEST_CHECK(linux_gpio_read(gpio, &value) == -EIO);
    g_ioctl_error = EINTR;
    TEST_CHECK(linux_gpio_read(gpio, &value) == 0 && value);
    linux_gpio_close(gpio);
    TEST_CHECK(g_active == 0U);
}

/**
 * @brief 验证强杀释放及exec不泄漏句柄，使用真实进程和文件锁。
 */
static void _test_process_lifetime(bool execute)
{
    linux_gpio_t *gpio;
    int           notification[2];
    int           status;
    pid_t         child;
    char          data;

    TEST_CHECK(pipe2(notification, O_CLOEXEC) == 0);
    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0)
    {
        (void)close(notification[0]);
        TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_OUTPUT, false, &gpio) == 0);
        if (execute)
        {
            execl("/bin/sleep", "sleep", "5", (char *)NULL);
            _exit(100);
        }

        TEST_CHECK(write(notification[1], "R", 1U) == 1);
        for (;;)
        {
            pause();
        }
    }

    (void)close(notification[1]);
    TEST_CHECK(read(notification[0], &data, 1U) == (execute ? 0 : 1));
    (void)close(notification[0]);
    if (!execute)
    {
        TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, &gpio) == -EBUSY);
    }

    if (execute)
    {
        TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_INPUT, false, &gpio) == 0);
        linux_gpio_close(gpio);
    }

    TEST_CHECK(kill(child, SIGKILL) == 0);
    TEST_CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    TEST_CHECK(linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_OUTPUT, true, &gpio) == 0);
    linux_gpio_close(gpio);
    TEST_CHECK(g_active == 0U);
}

/**
 * @brief 运行回归并清理独立的模拟引脚文件。
 */
int main(void)
{
    char         path[256];
    unsigned int index;

    TEST_CHECK(mkdtemp(g_directory) != NULL);
    _test_operations();
    _test_failures();
    for (index = 0U; index < TEST_KILL_CYCLES; index++)
    {
        _test_process_lifetime(false);
    }

    _test_process_lifetime(true);
    TEST_CHECK(snprintf(path, sizeof(path), "%s/112", g_directory) > 0);
    TEST_CHECK(unlink(path) == 0);
    TEST_CHECK(rmdir(g_directory) == 0);
    printf("PASS: %u assertions, %u SIGKILL cycles, exec release, no descriptor leaks\n", g_checks, TEST_KILL_CYCLES);
    return EXIT_SUCCESS;
}

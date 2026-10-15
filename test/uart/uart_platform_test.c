/**
 * @file uart_platform_test.c
 * @brief Linux串口termios、自定义分频及部分写入单元测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-15
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <linux/serial.h>
#include <sys/ioctl.h>

#include "linkg_uart.h"

/****************************** 测试上下文 ******************************/

static struct serial_struct g_test_serial;           // 模拟驱动串口参数
static struct termios       g_test_termios;          // 模拟termios回读值
static int                  g_test_serial_get_error; // TIOCGSERIAL错误注入
static int                  g_test_serial_sets;      // TIOCSSERIAL调用次数
static int                  g_test_write_step;       // 部分写入步骤
static bool                 g_test_preserve_hw;      // 模拟USB WWAN保留硬件termios字段

ssize_t __real_write(int fd, const void *buffer, size_t length);

/****************************** 系统调用替身 ******************************/

/**
 * @brief 返回固定测试fd，不访问主机串口。
 */
int __wrap_open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    return 42;
}

/**
 * @brief 模拟关闭测试fd。
 */
int __wrap_close(int fd)
{
    assert(fd == 42);
    return 0;
}

/**
 * @brief 模拟Linux serial_struct读取、写入和独占设置。
 */
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    void   *argument;

    assert(fd == 42);
    if (request == TIOCEXCL)
    {
        return 0;
    }

    va_start(arguments, request);
    argument = va_arg(arguments, void *);
    va_end(arguments);

    if (request == TIOCGSERIAL)
    {
        if (g_test_serial_get_error != 0)
        {
            errno = g_test_serial_get_error;
            return -1;
        }
        *(struct serial_struct *)argument = g_test_serial;
        return 0;
    }
    if (request == TIOCSSERIAL)
    {
        g_test_serial = *(struct serial_struct *)argument;
        g_test_serial_sets++;
        return 0;
    }

    errno = EINVAL;
    return -1;
}

/**
 * @brief 返回最近一次保存的termios状态。
 */
int __wrap_tcgetattr(int fd, struct termios *termios_p)
{
    assert(fd == 42);
    *termios_p = g_test_termios;
    return 0;
}

/**
 * @brief 保存待设置termios状态，供后续回读验证。
 */
int __wrap_tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
    struct termios previous;
    tcflag_t       hardware_mask;

    assert(fd == 42);
    assert(optional_actions == TCSANOW);

    previous = g_test_termios;
    g_test_termios = *termios_p;

    if (g_test_preserve_hw)
    {
        hardware_mask = CBAUD | CBAUDEX | CSIZE | CSTOPB | PARENB | PARODD | CMSPAR | CRTSCTS;
        g_test_termios.c_cflag = (g_test_termios.c_cflag & ~hardware_mask) | (previous.c_cflag & hardware_mask);
        g_test_termios.c_ispeed = previous.c_ispeed;
        g_test_termios.c_ospeed = previous.c_ospeed;
    }

    return 0;
}

/**
 * @brief 模拟刷新测试串口缓存。
 */
int __wrap_tcflush(int fd, int queue_selector)
{
    assert(fd == 42);
    assert(queue_selector == TCIOFLUSH);
    return 0;
}

/**
 * @brief 注入一次部分写入后EAGAIN，其他调用转发到真实write。
 */
ssize_t __wrap_write(int fd, const void *buffer, size_t length)
{
    (void)buffer;
    if (fd != 42)
    {
        return __real_write(fd, buffer, length);
    }
    if (g_test_write_step++ == 0)
    {
        assert(length >= 2U);
        return 2;
    }

    errno = EAGAIN;
    return -1;
}

/****************************** 测试用例 ******************************/

/**
 * @brief 重置串口驱动和termios替身。
 */
static void _uart_platform_reset(int baud_base)
{
    memset(&g_test_serial, 0, sizeof(g_test_serial));
    memset(&g_test_termios, 0, sizeof(g_test_termios));
    g_test_serial.baud_base = baud_base;
    g_test_serial_get_error = 0;
    g_test_serial_sets      = 0;
    g_test_write_step       = 0;
    g_test_preserve_hw      = false;
}

/**
 * @brief 验证标准波特率会清除上次自定义分频。
 */
static void _uart_platform_standard(void)
{
    uart_config_t config;
    int           fd;

    _uart_platform_reset(1500000);
    g_test_serial.type           = PORT_16550A;
    g_test_serial.flags          = ASYNC_SPD_CUST;
    g_test_serial.custom_divisor = 15;
    config.baudrate              = 115200;
    config.data_bits             = 8;
    config.stop_bits             = 1;
    config.parity                = 'n';
    config.hw_flow_control       = false;
    config.exclusive             = true;

    fd = linux_uart_open("/dev/test", &config);
    assert(fd == 42);
    assert((g_test_serial.flags & ASYNC_SPD_MASK) == 0);
    assert(g_test_serial_sets == 1);
    assert(cfgetispeed(&g_test_termios) == B115200);
    assert(cfgetospeed(&g_test_termios) == B115200);
    assert((g_test_termios.c_cflag & (PARENB | CSTOPB)) == 0);
    assert(linux_uart_close(fd) == 0);
}

/**
 * @brief 验证USB WWAN保留硬件termios字段时仍可启用标准115200通道。
 */
static void _uart_platform_standard_usb(void)
{
    uart_config_t config;
    int           fd;

    _uart_platform_reset(9600);
    g_test_serial.type     = PORT_UNKNOWN;
    g_test_preserve_hw     = true;
    g_test_termios.c_iflag = ICRNL | IXON;
    g_test_termios.c_oflag = OPOST;
    g_test_termios.c_lflag = ICANON | ECHO;
    g_test_termios.c_cflag = B9600 | CS8 | CREAD | CLOCAL;
    cfsetispeed(&g_test_termios, B9600);
    cfsetospeed(&g_test_termios, B9600);

    config.baudrate        = 115200;
    config.data_bits       = 8;
    config.stop_bits       = 1;
    config.parity          = 'n';
    config.hw_flow_control = false;
    config.exclusive       = true;

    fd = linux_uart_open("/dev/test", &config);
    assert(fd == 42);
    assert(g_test_serial_sets == 0);
    assert(cfgetispeed(&g_test_termios) == B9600);
    assert(cfgetospeed(&g_test_termios) == B9600);
    assert((g_test_termios.c_iflag & (ICRNL | IXON)) == 0);
    assert((g_test_termios.c_oflag & OPOST) == 0);
    assert((g_test_termios.c_lflag & (ICANON | ECHO)) == 0);
    assert(g_test_termios.c_cc[VMIN] == 0);
    assert(g_test_termios.c_cc[VTIME] == 0);
    assert(linux_uart_close(fd) == 0);
}

/**
 * @brief 验证100000 baud自定义分频及8E2设置。
 */
static void _uart_platform_custom_sbus(void)
{
    linux_uart_baud_info_t info;
    uart_config_t          config;
    int                    fd;

    _uart_platform_reset(1500000);
    g_test_serial.type     = PORT_16550A;
    config.baudrate        = 100000;
    config.data_bits       = 8;
    config.stop_bits       = 2;
    config.parity          = 'e';
    config.hw_flow_control = false;
    config.exclusive       = false;

    assert(linux_uart_baud_info(42, 100000U, &info) == 0);
    assert(info.divisor == 15U && info.actual == 100000.0 && info.error_pct == 0.0);
    fd = linux_uart_open("/dev/test", &config);
    assert(fd == 42);
    assert((g_test_serial.flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST);
    assert(g_test_serial.custom_divisor == 15);
    assert((g_test_termios.c_cflag & PARENB) != 0);
    assert((g_test_termios.c_cflag & PARODD) == 0);
    assert((g_test_termios.c_cflag & CSTOPB) != 0);
    assert(cfgetispeed(&g_test_termios) == B38400);
    assert(linux_uart_close(fd) == 0);
}

/**
 * @brief 验证不可靠分频、无serial_struct设备和部分写入处理。
 */
static void _uart_platform_edges(void)
{
    uint8_t        data[4] = {1U, 2U, 3U, 4U};
    uart_config_t  config;

    _uart_platform_reset(1000000);
    assert(linux_uart_baud_info(42, 300000U, &(linux_uart_baud_info_t){0}) == -ERANGE);

    g_test_serial_get_error   = ENOTTY;
    config.baudrate           = 115200;
    config.data_bits          = 8;
    config.stop_bits          = 1;
    config.parity             = 'n';
    config.hw_flow_control    = false;
    config.exclusive          = false;
    assert(linux_uart_open("/dev/test", &config) == 42);
    assert(linux_uart_write(42, data, sizeof(data)) == 2);

    config.baudrate = 0;
    assert(linux_uart_open("/dev/test", &config) == -EINVAL);
}

/**
 * @brief 执行Linux串口平台边界测试。
 */
int main(void)
{
    _uart_platform_standard();
    _uart_platform_standard_usb();
    _uart_platform_custom_sbus();
    _uart_platform_edges();
    puts("PASS UART platform termios, divisor readback, 8E2 and partial-write tests");
    return 0;
}

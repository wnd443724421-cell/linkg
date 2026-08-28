/**
 * @file linkg_uart.c
 * @brief Linux平台通用串口实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-28
 */

#define _GNU_SOURCE

#include "linkg_uart.h"

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <linux/serial.h>
#include <sys/ioctl.h>

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取标准波特率对应的termios参数。
 */
static speed_t _linux_uart_get_baud_flag(int baudrate)
{
    switch (baudrate)
    {
        case 9600:
            return B9600;

        case 19200:
            return B19200;

        case 38400:
            return B38400;

        case 57600:
            return B57600;

        case 115200:
            return B115200;

        case 230400:
            return B230400;

        case 460800:
            return B460800;

        case 921600:
            return B921600;

        default:
            return B0;
    }
}

/**
 * @brief 设置串口非标准波特率。
 */
static int _linux_uart_set_custom_baud(int fd, int baudrate)
{
    struct serial_struct serial;

    if (ioctl(fd, TIOCGSERIAL, &serial) < 0)
    {
        return -1;
    }

    serial.flags &= ~ASYNC_SPD_MASK;
    serial.flags |= ASYNC_SPD_CUST;
    serial.custom_divisor = (serial.baud_base + baudrate / 2) / baudrate;

    if (ioctl(fd, TIOCSSERIAL, &serial) < 0)
    {
        return -1;
    }

    return 0;
}

/**
 * @brief 清除串口自定义波特率配置。
 */
static void _linux_uart_clear_custom_baud(int fd)
{
    struct serial_struct serial;

    if (ioctl(fd, TIOCGSERIAL, &serial) < 0)
    {
        return;
    }

    if ((serial.flags & ASYNC_SPD_MASK) != ASYNC_SPD_CUST)
    {
        return;
    }

    serial.flags &= ~ASYNC_SPD_MASK;
    (void)ioctl(fd, TIOCSSERIAL, &serial);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 打开并配置Linux串口设备。
 */
int linux_uart_open(const char *dev, const uart_config_t *config)
{
    struct termios tty;
    speed_t baud_flag;
    int fd;

    if (dev == NULL)
    {
        return -EINVAL;
    }

    if (config == NULL)
    {
        return -EINVAL;
    }

    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        return -errno;
    }

    if (config->exclusive)
    {
        if (ioctl(fd, TIOCEXCL) < 0)
        {
            close(fd);
            return -errno;
        }
    }

    if (tcgetattr(fd, &tty) < 0)
    {
        close(fd);
        return -errno;
    }

    cfmakeraw(&tty);

    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CSIZE;

    switch (config->data_bits)
    {
        case 5:
            tty.c_cflag |= CS5;
            break;

        case 6:
            tty.c_cflag |= CS6;
            break;

        case 7:
            tty.c_cflag |= CS7;
            break;

        default:
            tty.c_cflag |= CS8;
            break;
    }

    if (config->parity == 'e')
    {
        tty.c_cflag |= PARENB;
        tty.c_cflag &= ~PARODD;
    }
    else if (config->parity == 'o')
    {
        tty.c_cflag |= PARENB;
        tty.c_cflag |= PARODD;
    }
    else
    {
        tty.c_cflag &= ~PARENB;
    }

    if (config->stop_bits == 2)
    {
        tty.c_cflag |= CSTOPB;
    }
    else
    {
        tty.c_cflag &= ~CSTOPB;
    }

#ifdef CRTSCTS
    if (config->hw_flow_control)
    {
        tty.c_cflag |= CRTSCTS;
    }
    else
    {
        tty.c_cflag &= ~CRTSCTS;
    }
#endif

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    baud_flag = _linux_uart_get_baud_flag(config->baudrate);

    if (baud_flag != B0)
    {
        _linux_uart_clear_custom_baud(fd);

        cfsetispeed(&tty, baud_flag);
        cfsetospeed(&tty, baud_flag);
    }
    else
    {
        if (_linux_uart_set_custom_baud(fd, config->baudrate) < 0)
        {
            close(fd);
            return -errno;
        }

        cfsetispeed(&tty, B38400);
        cfsetospeed(&tty, B38400);
    }

    if (tcsetattr(fd, TCSANOW, &tty) < 0)
    {
        close(fd);
        return -errno;
    }

    tcflush(fd, TCIOFLUSH);

    return fd;
}

/**
 * @brief 关闭Linux串口设备。
 */
int linux_uart_close(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }

    return 0;
}

/****************************** 数据收发 ******************************/

/**
 * @brief 向串口完整写入指定长度的数据。
 */
int linux_uart_write(int fd, const void *buf, int len)
{
    int offset = 0;
    int ret;

    if (fd < 0)
    {
        return -EINVAL;
    }

    if (buf == NULL)
    {
        return -EINVAL;
    }

    if (len <= 0)
    {
        return -EINVAL;
    }

    while (offset < len)
    {
        ret = write(fd, (const char *)buf + offset, len - offset);
        if (ret > 0)
        {
            offset += ret;
            continue;
        }

        if (errno == EINTR)
        {
            continue;
        }

        return -errno;
    }

    return offset;
}

/**
 * @brief 从串口读取当前可用数据。
 */
int linux_uart_read(int fd, void *buf, int len)
{
    int ret;

    if (fd < 0)
    {
        return -EINVAL;
    }

    if (buf == NULL)
    {
        return -EINVAL;
    }

    if (len <= 0)
    {
        return -EINVAL;
    }

    ret = read(fd, buf, len);
    if (ret < 0)
    {
        if (errno == EAGAIN || errno == EINTR)
        {
            return 0;
        }

        return -errno;
    }

    return ret;
}

/****************************** 控制接口 ******************************/

/**
 * @brief 清空串口输入和输出缓冲区。
 */
void linux_uart_flush(int fd)
{
    if (fd >= 0)
    {
        tcflush(fd, TCIOFLUSH);
    }
}

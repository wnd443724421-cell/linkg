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
#include <stdint.h>
#include <string.h>
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
        case 1200:
            return B1200;

        case 2400:
            return B2400;

        case 4800:
            return B4800;

        case 500000:
            return B500000;

        case 1500000:
            return B1500000;

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
 * @brief 根据驱动真实baud_base计算整数分频和误差，不猜测T113时钟频率。
 */
int linux_uart_baud_info(int fd, uint32_t baudrate, linux_uart_baud_info_t *out)
{
    struct serial_struct serial;
    uint64_t             divisor;

    if (fd < 0 || baudrate == 0U || out == NULL)
    {
        return -EINVAL;
    }

    memset(&serial, 0, sizeof(serial));
    if (ioctl(fd, TIOCGSERIAL, &serial) < 0)
    {
        return -errno;
    }

    if (serial.baud_base <= 0)
    {
        return -ERANGE;
    }

    divisor = ((uint64_t)serial.baud_base + baudrate / 2U) / baudrate;
    if (divisor == 0U || divisor > UINT16_MAX)
    {
        return -ERANGE;
    }

    out->baud_base = (uint32_t)serial.baud_base;
    out->divisor   = (uint32_t)divisor;
    out->actual    = (double)out->baud_base / out->divisor;
    out->error_pct = (out->actual / baudrate - 1.0) * 100.0;

    if (out->error_pct > (double)LINKG_UART_BAUD_ERROR_MAX_PPM / 10000.0 ||
        out->error_pct < -(double)LINKG_UART_BAUD_ERROR_MAX_PPM / 10000.0)
    {
        return -ERANGE;
    }

    return 0;
}

/**
 * @brief 设置或清除自定义分频并回读确认，避免沿用上次非标准波特率。
 */
static int _linux_uart_configure_divisor(int fd, uint32_t baudrate, bool custom)
{
    struct serial_struct   serial;
    struct serial_struct   verify;
    linux_uart_baud_info_t  info;
    int                    ret;

    ret = linux_uart_baud_info(fd, baudrate, &info);
    if (ret != 0)
    {
        // PTY和部分USB UART只支持标准termios，不提供serial_struct。
        return !custom && (ret == -ENOTTY || ret == -EINVAL) ? 0 : ret;
    }

    if (ioctl(fd, TIOCGSERIAL, &serial) < 0)
    {
        return -errno;
    }

    if (!custom && (serial.flags & ASYNC_SPD_MASK) == 0)
    {
        return 0;
    }

    serial.flags &= ~ASYNC_SPD_MASK;
    if (custom)
    {
        serial.flags |= ASYNC_SPD_CUST;
        serial.custom_divisor = (int)info.divisor;
    }

    if (ioctl(fd, TIOCSSERIAL, &serial) < 0 || ioctl(fd, TIOCGSERIAL, &verify) < 0)
    {
        return -errno;
    }

    if ((verify.flags & ASYNC_SPD_MASK) != (serial.flags & ASYNC_SPD_MASK) ||
        (custom && verify.custom_divisor != serial.custom_divisor))
    {
        return -EIO;
    }

    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 打开并配置Linux串口设备。
 */
int linux_uart_open(const char *dev, const uart_config_t *config)
{
    struct termios tty;
    struct termios verify;
    speed_t        baud_flag;
    int            fd;
    int            ret;

    if (dev == NULL)
    {
        return -EINVAL;
    }

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (config->baudrate <= 0 || config->data_bits < 5 || config->data_bits > 8 ||
        (config->stop_bits != 1 && config->stop_bits != 2) ||
        (config->parity != 'n' && config->parity != 'e' && config->parity != 'o'))
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
            ret = -errno;
            close(fd);
            return ret;
        }
    }

    if (tcgetattr(fd, &tty) < 0)
    {
        ret = -errno;
        close(fd);
        return ret;
    }

    cfmakeraw(&tty);

    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~(CSIZE | PARENB | PARODD | CMSPAR);

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

    ret = _linux_uart_configure_divisor(fd, (uint32_t)config->baudrate, baud_flag == B0);
    if (ret != 0)
    {
        close(fd);
        return ret;
    }

    if (baud_flag == B0)
    {
        baud_flag = B38400;
    }

    if (cfsetispeed(&tty, baud_flag) < 0 || cfsetospeed(&tty, baud_flag) < 0 ||
        tcsetattr(fd, TCSANOW, &tty) < 0 || tcgetattr(fd, &verify) < 0)
    {
        ret = -errno;
        close(fd);
        return ret;
    }

    if ((verify.c_cflag & (CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS)) !=
        (tty.c_cflag & (CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS)) ||
        cfgetospeed(&verify) != baud_flag || cfgetispeed(&verify) != baud_flag)
    {
        close(fd);
        return -EIO;
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
 * @brief 尽力写入串口，遇到背压或错误时返回已提交进度。
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

        if (ret < 0 && errno == EINTR)
        {
            continue;
        }

        if (offset > 0)
        {
            return offset;
        }

        return ret == 0 ? -EAGAIN : -errno;
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

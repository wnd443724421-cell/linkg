/**
 * @file linkg_gpio.c
 * @brief Linux平台GPIO字符设备操作与进程资源自动释放
 * @author Dawn
 * @version 1.1.0
 * @date 2026-10-19
 */

#include "linkg_gpio.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/****************************** 模块常量 ******************************/

#define LINUX_GPIO_CHIP_DEVICE   "/dev/gpiochip0" // Allwinner主PIO，起始编号为0
#define LINUX_GPIO_CONSUMER      "linkg"          // 内核引脚申请者名称
#define LINUX_GPIO_PINS_PER_BANK 32U              // Sunxi每组GPIO编号跨度

/****************************** 内部类型 ******************************/

struct linux_gpio
{
    linux_gpio_direction_t direction; // GPIO输入输出方向
    int                    line_fd;   // 独占引脚句柄，关闭或进程退出时由内核释放
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 将Sunxi引脚名称解析为Linux GPIO编号。
 */
static int _linux_gpio_parse_pin(const char *pin_name, unsigned int *number)
{
    unsigned int index;
    const char  *cursor;

    if (pin_name == NULL || number == NULL || pin_name[0] != 'P' || pin_name[1] < 'A' || pin_name[1] > 'G')
    {
        return -EINVAL;
    }

    index  = 0U;
    cursor = pin_name + 2;

    if (*cursor == '\0')
    {
        return -EINVAL;
    }

    while (*cursor != '\0')
    {
        if (*cursor < '0' || *cursor > '9')
        {
            return -EINVAL;
        }

        index = index * 10U + (unsigned int)(*cursor - '0');
        if (index >= LINUX_GPIO_PINS_PER_BANK)
        {
            return -EINVAL;
        }

        cursor++;
    }

    *number = (unsigned int)(pin_name[1] - 'A') * LINUX_GPIO_PINS_PER_BANK + index;
    return 0;
}

/**
 * @brief 执行GPIO控制命令，仅在系统调用被信号中断时重试。
 */
static int _linux_gpio_ioctl(int descriptor, unsigned long command, void *argument)
{
    int ret;

    do
    {
        ret = ioctl(descriptor, command, argument);
    } while (ret < 0 && errno == EINTR);

    return ret < 0 ? -errno : 0;
}

/**
 * @brief 独占申请引脚并原子配置方向及初始输出电平。
 */
static int _linux_gpio_request(unsigned int number, linux_gpio_direction_t direction, bool initial_value, int *line_fd)
{
    struct gpiochip_info      information;
    struct gpiohandle_request request;
    int                       chip_fd;
    int                       flags;
    int                       ret;

    *line_fd = -1;

    chip_fd = open(LINUX_GPIO_CHIP_DEVICE, O_RDONLY | O_CLOEXEC);
    if (chip_fd < 0)
    {
        return -errno;
    }

    memset(&information, 0, sizeof(information));
    ret = _linux_gpio_ioctl(chip_fd, GPIO_GET_CHIPINFO_IOCTL, &information);
    if (ret != 0)
    {
        (void)close(chip_fd);
        return ret;
    }

    if (number >= information.lines)
    {
        (void)close(chip_fd);
        return -EINVAL;
    }

    memset(&request, 0, sizeof(request));
    request.lineoffsets[0]     = number;
    request.flags             = direction == LINUX_GPIO_DIRECTION_INPUT ? GPIOHANDLE_REQUEST_INPUT : GPIOHANDLE_REQUEST_OUTPUT;
    request.default_values[0] = direction == LINUX_GPIO_DIRECTION_OUTPUT && initial_value ? 1U : 0U;
    request.lines             = 1U;
    request.fd                = -1;
    memcpy(request.consumer_label, LINUX_GPIO_CONSUMER, sizeof(LINUX_GPIO_CONSUMER));

    ret = _linux_gpio_ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &request);
    (void)close(chip_fd);
    if (ret != 0)
    {
        return ret;
    }

    if (request.fd < 0)
    {
        return -EIO;
    }

    // 内核按句柄生命周期释放引脚，禁止将句柄泄漏到exec启动的子程序。
    flags = fcntl(request.fd, F_GETFD);
    if (flags < 0 || fcntl(request.fd, F_SETFD, flags | FD_CLOEXEC) < 0)
    {
        ret = -errno;
        (void)close(request.fd);
        return ret;
    }

    *line_fd = request.fd;
    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 申请GPIO并配置方向及输出初始电平。
 */
int linux_gpio_open(const char *pin_name, linux_gpio_direction_t direction, bool initial_value, linux_gpio_t **gpio)
{
    linux_gpio_t *instance;
    unsigned int  number;
    int           ret;

    if (gpio == NULL)
    {
        return -EINVAL;
    }

    *gpio = NULL;

    if (direction != LINUX_GPIO_DIRECTION_INPUT && direction != LINUX_GPIO_DIRECTION_OUTPUT)
    {
        return -EINVAL;
    }

    ret = _linux_gpio_parse_pin(pin_name, &number);
    if (ret != 0)
    {
        return ret;
    }

    instance = calloc(1, sizeof(*instance));
    if (instance == NULL)
    {
        return -ENOMEM;
    }

    instance->direction = direction;
    instance->line_fd   = -1;

    // 已被驱动或其他进程占用时返回原始错误，不强制释放或接管。
    ret = _linux_gpio_request(number, direction, initial_value, &instance->line_fd);
    if (ret != 0)
    {
        free(instance);
        return ret;
    }

    *gpio = instance;
    return 0;
}

/**
 * @brief 关闭GPIO句柄并由内核释放本次申请的引脚。
 *
 * 调用方必须确保没有其他线程继续使用该实例。
 */
void linux_gpio_close(linux_gpio_t *gpio)
{
    if (gpio == NULL)
    {
        return;
    }

    if (gpio->line_fd >= 0)
    {
        (void)close(gpio->line_fd);
    }

    free(gpio);
}

/****************************** 电平操作 ******************************/

/**
 * @brief 读取GPIO当前逻辑电平。
 */
int linux_gpio_read(linux_gpio_t *gpio, bool *value)
{
    struct gpiohandle_data data;
    int                    ret;

    if (gpio == NULL || value == NULL)
    {
        return -EINVAL;
    }

    memset(&data, 0, sizeof(data));
    ret = _linux_gpio_ioctl(gpio->line_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);
    if (ret != 0)
    {
        return ret;
    }

    if (data.values[0] > 1U)
    {
        return -EIO;
    }

    *value = data.values[0] != 0U;
    return 0;
}

/**
 * @brief 设置输出GPIO逻辑电平。
 */
int linux_gpio_write(linux_gpio_t *gpio, bool value)
{
    struct gpiohandle_data data;

    if (gpio == NULL)
    {
        return -EINVAL;
    }

    if (gpio->direction != LINUX_GPIO_DIRECTION_OUTPUT)
    {
        return -EPERM;
    }

    memset(&data, 0, sizeof(data));
    data.values[0] = value ? 1U : 0U;
    return _linux_gpio_ioctl(gpio->line_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
}

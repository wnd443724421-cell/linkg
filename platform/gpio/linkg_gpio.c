/**
 * @file linkg_gpio.c
 * @brief Linux平台Sysfs GPIO操作实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-12
 */

#define _GNU_SOURCE

#include "linkg_gpio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "linkg_file.h"
#include "linkg_log.h"

/****************************** 模块常量 ******************************/

#define LINUX_GPIO_SYSFS_DIRECTORY  "/sys/class/gpio"          // Sysfs GPIO根目录
#define LINUX_GPIO_SYSFS_EXPORT     "/sys/class/gpio/export"   // GPIO申请节点
#define LINUX_GPIO_SYSFS_UNEXPORT   "/sys/class/gpio/unexport" // GPIO释放节点
#define LINUX_GPIO_PATH_SIZE        64U                        // Sysfs路径缓冲区大小
#define LINUX_GPIO_PINS_PER_BANK    32U                        // Sunxi每组GPIO编号跨度

/****************************** 内部类型 ******************************/

struct linux_gpio
{
    unsigned int           number;    // Sysfs GPIO全局编号
    linux_gpio_direction_t direction; // GPIO输入输出方向
    int                    value_fd;  // GPIO value节点描述符，由实例持有
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
 * @brief 构造GPIO Sysfs属性文件路径。
 */
static int _linux_gpio_format_path(char *path, size_t size, unsigned int number, const char *attribute)
{
    int length;

    length = snprintf(path, size, "%s/gpio%u/%s", LINUX_GPIO_SYSFS_DIRECTORY, number, attribute);
    if (length < 0 || (size_t)length >= size)
    {
        return -ENAMETOOLONG;
    }

    return 0;
}

/**
 * @brief 将GPIO编号写入Sysfs申请或释放节点。
 */
static int _linux_gpio_write_number(const char *path, unsigned int number)
{
    char buffer[16];
    int  length;

    length = snprintf(buffer, sizeof(buffer), "%u", number);
    if (length < 0 || (size_t)length >= sizeof(buffer))
    {
        return -EINVAL;
    }

    return linkg_file_write_all(path, buffer, (size_t)length);
}

/**
 * @brief 释放本次申请的GPIO资源。
 *
 * 关机过程中Sysfs节点可能已经消失，释放操作只做尽力清理。
 */
static void _linux_gpio_unexport(unsigned int number)
{
    int ret;

    ret = _linux_gpio_write_number(LINUX_GPIO_SYSFS_UNEXPORT, number);
    if (ret != 0 && ret != -ENOENT && ret != -ENODEV)
    {
        LINKG_LOG_WARN("GPIO: unexport gpio%u failed: %d", number, ret);
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 申请GPIO并配置方向及输出初始电平。
 */
int linux_gpio_open(const char *pin_name, linux_gpio_direction_t direction, bool initial_value, linux_gpio_t **gpio)
{
    char          path[LINUX_GPIO_PATH_SIZE];
    linux_gpio_t *instance;
    unsigned int  number;
    const char   *direction_value;
    int           flags;
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

    instance->number    = number;
    instance->direction = direction;
    instance->value_fd  = -1;

    // 只有成功export后才拥有GPIO，不接管其他模块已经export的引脚。
    ret = _linux_gpio_write_number(LINUX_GPIO_SYSFS_EXPORT, number);
    if (ret != 0)
    {
        free(instance);
        return ret;
    }

    ret = _linux_gpio_format_path(path, sizeof(path), number, "direction");
    if (ret != 0)
    {
        goto fail_export;
    }

    // Sysfs支持high/low，配置输出时可同时写入初始电平。
    if (direction == LINUX_GPIO_DIRECTION_INPUT)
    {
        direction_value = "in";
    }
    else
    {
        direction_value = initial_value ? "high" : "low";
    }

    ret = linkg_file_write_all(path, direction_value, strlen(direction_value));
    if (ret != 0)
    {
        goto fail_export;
    }

    ret = _linux_gpio_format_path(path, sizeof(path), number, "value");
    if (ret != 0)
    {
        goto fail_export;
    }

    flags = (direction == LINUX_GPIO_DIRECTION_INPUT) ? O_RDONLY : O_RDWR;

    instance->value_fd = open(path, flags | O_CLOEXEC);
    if (instance->value_fd < 0)
    {
        ret = -errno;
        goto fail_export;
    }

    *gpio = instance;
    return 0;

fail_export:
    _linux_gpio_unexport(number);
    free(instance);
    return ret;
}

/**
 * @brief 关闭GPIO并尽力释放本次申请的Sysfs资源。
 *
 * 调用方必须确保没有其他线程继续使用该实例。
 */
void linux_gpio_close(linux_gpio_t *gpio)
{
    if (gpio == NULL)
    {
        return;
    }

    if (gpio->value_fd >= 0)
    {
        (void)close(gpio->value_fd);
    }

    _linux_gpio_unexport(gpio->number);
    free(gpio);
}

/****************************** 电平操作 ******************************/

/**
 * @brief 读取GPIO当前逻辑电平。
 */
int linux_gpio_read(linux_gpio_t *gpio, bool *value)
{
    char    data;
    ssize_t count;

    if (gpio == NULL || value == NULL)
    {
        return -EINVAL;
    }

    do
    {
        count = pread(gpio->value_fd, &data, 1U, 0);
    } while (count < 0 && errno == EINTR);

    if (count < 0)
    {
        return -errno;
    }

    if (count != 1 || (data != '0' && data != '1'))
    {
        return -EIO;
    }

    *value = (data == '1');
    return 0;
}

/**
 * @brief 设置输出GPIO逻辑电平。
 */
int linux_gpio_write(linux_gpio_t *gpio, bool value)
{
    char    data;
    ssize_t count;

    if (gpio == NULL)
    {
        return -EINVAL;
    }

    if (gpio->direction != LINUX_GPIO_DIRECTION_OUTPUT)
    {
        return -EPERM;
    }

    data = value ? '1' : '0';

    do
    {
        count = pwrite(gpio->value_fd, &data, 1U, 0);
    } while (count < 0 && errno == EINTR);

    if (count < 0)
    {
        return -errno;
    }

    return count == 1 ? 0 : -EIO;
}

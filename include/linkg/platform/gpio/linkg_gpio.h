/**
 * @file linkg_gpio.h
 * @brief Linux平台GPIO字符设备接口，独占句柄随进程退出释放
 */

#ifndef LINKG_GPIO_H
#define LINKG_GPIO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型定义 ******************************/

typedef enum
{
    LINUX_GPIO_DIRECTION_INPUT  = 0, // 输入模式
    LINUX_GPIO_DIRECTION_OUTPUT = 1  // 输出模式
} linux_gpio_direction_t;

typedef struct linux_gpio linux_gpio_t; // GPIO实例，正常退出时由调用方关闭

/****************************** 生命周期 ******************************/

int  linux_gpio_open(const char *pin_name, linux_gpio_direction_t direction, bool initial_value, linux_gpio_t **gpio);
void linux_gpio_close(linux_gpio_t *gpio);

/****************************** 电平操作 ******************************/

int linux_gpio_read(linux_gpio_t *gpio, bool *value);
int linux_gpio_write(linux_gpio_t *gpio, bool value);

#ifdef __cplusplus
}
#endif

#endif

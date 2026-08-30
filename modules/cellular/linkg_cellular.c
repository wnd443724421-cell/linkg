/**
 * @file linkg_cellular.c
 * @brief LinkG蜂窝网络运行控制实现
 */

#include "linkg_cellular.h"

#include <errno.h>


/****************************** 生命周期 ******************************/

int linkg_cellular_init(const linkg_cellular_config_t *config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    return 0;
}

int linkg_cellular_start(void)
{
    return 0;
}

int linkg_cellular_run(linkg_thread_t *owner_thread)
{
    if (owner_thread == NULL)
    {
        return -EINVAL;
    }

    return 0;
}

int linkg_cellular_stop(void)
{
    return 0;
}

int linkg_cellular_deinit(void)
{
    return 0;
}

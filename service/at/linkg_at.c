/**
 * @file linkg_at.c
 * @brief LinkG AT服务实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-26
 */

#include "linkg_at.h"

#include "linkg_log.h"

#include "at_internal.h"

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化AT服务。
 */
int linkg_at_init(void)
{
    int ret;

    ret = _linkg_at_server_init();
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("AT: module initialized");

    return 0;
}

/**
 * @brief 启动AT服务。
 */
int linkg_at_start(void)
{
    int ret;

    ret = _linkg_at_server_start();
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("AT: module started");

    return 0;
}

/**
 * @brief 停止AT服务。
 */
int linkg_at_stop(void)
{
    int ret;

    ret = _linkg_at_server_stop();
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("AT: module stopped");

    return 0;
}

/**
 * @brief 反初始化AT服务。
 */
int linkg_at_deinit(void)
{
    int ret;

    ret = _linkg_at_server_deinit();
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("AT: module deinitialized");

    return 0;
}

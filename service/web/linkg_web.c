/**
 * @file linkg_web.c
 * @brief LinkG Web服务实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-14
 */

#include "linkg_web.h"

#include "linkg_log.h"
#include "web_internal.h"

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Web服务。
 */
int linkg_web_init(void)
{
    int ret;

    ret = _linkg_web_server_init();
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("WEB: module initialized");

    return 0;
}

/**
 * @brief 启动Web服务。
 */
int linkg_web_start(void)
{
    int ret;

    ret = _linkg_web_server_start();
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("WEB: module started");

    return 0;
}

/**
 * @brief 停止Web服务。
 */
int linkg_web_stop(void)
{
    int ret;

    ret = _linkg_web_server_stop();
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("WEB: module stopped");

    return 0;
}

/**
 * @brief 反初始化Web服务。
 */
int linkg_web_deinit(void)
{
    int ret;

    ret = _linkg_web_server_deinit();
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("WEB: module deinitialized");

    return 0;
}

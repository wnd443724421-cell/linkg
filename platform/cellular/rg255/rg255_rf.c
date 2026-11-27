/**
 * @file rg255_rf.c
 * @brief RG255工厂模式强发命令封装
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-27
 */

#include "rg255_rf.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

/****************************** 模块常量 ******************************/

#define RG255_RF_TIMEOUT_MS 3000 // AT命令超时，单位毫秒
#define RG255_RF_TX_GAIN    23   // 文档推荐发射增益设置

/****************************** 射频测试 ******************************/

/**
 * @brief 查询并严格校验工厂模式响应。
 */
int rg255_rf_get_mode(at_channel_t *channel, unsigned int *mode)
{
    at_command_config_t config = {.timeout_ms = RG255_RF_TIMEOUT_MS, .expect_prefix = "+QRFTEST:"};
    char                response[128];
    char                extra;
    unsigned int        value;
    int                 ret;

    if (channel == NULL || mode == NULL)
    {
        return -EINVAL;
    }

    ret = at_channel_exec(channel, "AT+QRFTEST=\"mode\"", &config, response, sizeof(response));
    if (ret != 0)
    {
        return ret;
    }

    if (sscanf(response, "+QRFTEST: %u %c", &value, &extra) != 1 || value > 1U)
    {
        return -EPROTO;
    }

    *mode = value;
    return 0;
}

/**
 * @brief 设置工厂模式。
 */
int rg255_rf_set_mode(at_channel_t *channel, bool enabled)
{
    at_command_config_t config = {.timeout_ms = RG255_RF_TIMEOUT_MS};

    return at_channel_exec(channel, enabled ? "AT+QRFTEST=\"mode\",1" : "AT+QRFTEST=\"mode\",0", &config, NULL, 0);
}

/**
 * @brief 使用已经校验的参数开启或关闭RG255AA强发。
 */
int rg255_rf_set_tx(at_channel_t *channel, uint32_t rat, uint32_t band, uint32_t bandwidth, uint32_t arfcn, bool enabled)
{
    at_command_config_t config = {.timeout_ms = RG255_RF_TIMEOUT_MS};
    char                command[128];
    int                 length;

    length = snprintf(command, sizeof(command), "AT+QRFTEST=\"tx\",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%d",
                      rat, band, bandwidth, arfcn, enabled ? 1U : 0U, RG255_RF_TX_GAIN);
    if (length < 0 || (size_t)length >= sizeof(command))
    {
        return -EINVAL;
    }

    return at_channel_exec(channel, command, &config, NULL, 0);
}

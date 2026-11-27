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

#include "linkg_log.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define RG255_RF_TIMEOUT_MS 3000  // AT命令超时，单位毫秒
#define RG255_RF_DRAIN_MS   1500U // 等待上一个超时事务退场的上限
#define RG255_RF_POLL_MS    50U   // 查询AT通道是否已恢复空闲的间隔
#define RG255_RF_TX_GAIN    23    // 文档推荐发射增益设置

/****************************** 内部辅助 ******************************/

/**
 * @brief 执行诊断白名单内的AT命令并记录实际响应。
 *
 * EAGAIN可能来自通道的迟到响应保护或UART写入。
 * 仅对查询和关闭操作重试，启用FTM和强发不重发。
 */
static int _rg255_rf_exec(at_channel_t *channel, const char *command, const at_command_config_t *config, bool retryable, char *response, int size)
{
    uint64_t started = linkg_time_elapsed_ms();
    int      ret;

    response[0] = '\0';
    do
    {
        ret = at_channel_exec(channel, command, config, response, size);
        if (!retryable || ret != -EAGAIN || linkg_time_elapsed_ms() - started >= RG255_RF_DRAIN_MS)
        {
            break;
        }
        (void)linkg_time_sleep_ms(RG255_RF_POLL_MS);
    }
    while (true);

    if (ret == 0)
    {
        LINKG_LOG_INFO("RF-TEST: AT cmd=%s terminal=OK elapsed_ms=%" PRIu64 " response=%s",
                       command, linkg_time_elapsed_ms() - started, response);
    }
    else
    {
        LINKG_LOG_WARN("RF-TEST: AT cmd=%s error=%d elapsed_ms=%" PRIu64 " response=%s",
                       command, ret, linkg_time_elapsed_ms() - started, response[0] != '\0' ? response : "(none)");
    }
    return ret;
}

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

    ret = _rg255_rf_exec(channel, "AT+QRFTEST=\"mode\"", &config, true, response, sizeof(response));
    if (ret != 0)
    {
        return ret;
    }

    if (sscanf(response, "+QRFTEST: %u %c", &value, &extra) != 1 || value > 1U)
    {
        LINKG_LOG_WARN("RF-TEST: unexpected FTM mode response=%s", response);
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

    char                response[512];

    return _rg255_rf_exec(channel, enabled ? "AT+QRFTEST=\"mode\",1" : "AT+QRFTEST=\"mode\",0", &config, !enabled, response, sizeof(response));
}

/**
 * @brief 使用已经校验的参数开启或关闭RG255AA强发。
 */
int rg255_rf_set_tx(at_channel_t *channel, uint32_t rat, uint32_t band, uint32_t bandwidth, uint32_t arfcn, bool enabled)
{
    at_command_config_t config = {.timeout_ms = RG255_RF_TIMEOUT_MS};
    char                command[128];
    char                response[512];
    int                 length;

    length = snprintf(command, sizeof(command), "AT+QRFTEST=\"tx\",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u,%d",
                      rat, band, bandwidth, arfcn, enabled ? 1U : 0U, RG255_RF_TX_GAIN);
    if (length < 0 || (size_t)length >= sizeof(command))
    {
        return -EINVAL;
    }

    return _rg255_rf_exec(channel, command, &config, !enabled, response, sizeof(response));
}

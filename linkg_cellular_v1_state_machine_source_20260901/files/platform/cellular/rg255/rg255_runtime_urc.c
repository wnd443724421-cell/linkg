/**
 * @file rg255_runtime_urc.c
 * @brief RG255运行期URC配置实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-01
 */

#include "rg255_runtime_urc.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

/****************************** 模块常量 ******************************/

#define RG255_RUNTIME_URC_CONFIG_TIMEOUT_MS 5000 // 运行期URC配置命令超时，单位ms

/****************************** 内部辅助 ******************************/

/**
 * @brief 执行一条不需要返回正文的运行期URC配置命令。
 */
static int _rg255_runtime_urc_exec(at_channel_t *channel, const char *command)
{
    at_command_config_t config;

    if (channel == NULL || command == NULL || command[0] == '\0')
    {
        return -EINVAL;
    }

    memset(&config, 0, sizeof(config));
    config.timeout_ms = RG255_RUNTIME_URC_CONFIG_TIMEOUT_MS;

    return at_channel_exec(channel, command, &config, NULL, 0);
}

/****************************** URC配置 ******************************/

/**
 * @brief 设置EPS网络注册状态URC上报。
 */
int rg255_cmd_set_eps_registration_urc(at_channel_t *channel, bool enable)
{
    return _rg255_runtime_urc_exec(channel, enable ? "AT+CEREG=1" : "AT+CEREG=0");
}

/**
 * @brief 设置5GS网络注册状态URC上报。
 */
int rg255_cmd_set_5g_registration_urc(at_channel_t *channel, bool enable)
{
    return _rg255_runtime_urc_exec(channel, enable ? "AT+C5GREG=1" : "AT+C5GREG=0");
}

/**
 * @brief 设置无线信号变化URC上报。
 */
int rg255_cmd_set_signal_urc(at_channel_t *channel, bool enable)
{
    return _rg255_runtime_urc_exec(channel, enable ? "AT+QCSQ=1" : "AT+QCSQ=0");
}

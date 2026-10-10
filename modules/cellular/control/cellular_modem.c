/**
 * @file cellular_modem.c
 * @brief LinkG RG255模组AT通道及基础配置管理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-10
 */

#include "cellular_modem.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_system_resources.h"
#include "linkg_time.h"
#include "linkg_uart.h"

#include "rg255_cmd.h"
#include "rg255_query.h"

#include "cellular_internal.h"

/****************************** 模块常量 ******************************/

#define CELLULAR_MODEM_AT_DEVICE            "/dev/ttyUSB1"  // RG255 AT控制串口设备
#define CELLULAR_MODEM_AT_BAUDRATE          115200          // RG255 AT串口波特率
#define CELLULAR_MODEM_AT_READY_TIMEOUT_MS  30000U          // AT通道整体就绪等待时间
#define CELLULAR_MODEM_AT_READY_RETRY_MS    500U            // AT通道就绪失败重试间隔
#define CELLULAR_MODEM_RESTART_SETTLE_MS    2000U           // CFUN重启后首次重新探测等待时间

/****************************** 全局上下文 ******************************/

static at_channel_t *g_cellular_modem_channel = NULL; // Modem唯一持有的AT通道，仅Owner串行访问

/****************************** AT通道启动 ******************************/

/**
 * @brief 初始化RG255 AT控制串口参数。
 */
static void _cellular_modem_make_uart_config(uart_config_t *config)
{
    memset(config, 0, sizeof(*config));

    config->baudrate        = CELLULAR_MODEM_AT_BAUDRATE;
    config->data_bits       = 8;
    config->stop_bits       = 1;
    config->parity          = 'N';
    config->hw_flow_control = false;
    config->exclusive       = true;
}

/**
 * @brief 对已经可通信的RG255应用基础AT运行参数。
 */
static int _cellular_modem_apply_at_baseline(at_channel_t *channel)
{
    int ret;

    ret = rg255_cmd_set_echo(channel, false);
    if (ret != 0)
    {
        return ret;
    }

    ret = rg255_cmd_enable_cmee(channel);
    if (ret != 0)
    {
        return ret;
    }

    return rg255_cmd_disable_sleep(channel);
}

/**
 * @brief 创建AT通道并等待RG255进入可执行命令状态。
 */
static int _cellular_modem_create_ready_channel(at_channel_t **out)
{
    uart_config_t config;
    at_channel_t *channel;
    uint64_t      started_ms;
    uint64_t      now_ms;
    unsigned int  attempt;
    int           error;
    int           ret;

    if (out == NULL)
    {
        return -EINVAL;
    }

    *out       = NULL;
    started_ms = linkg_time_elapsed_ms();
    attempt    = 0U;
    error      = -ETIMEDOUT;

    _cellular_modem_make_uart_config(&config);

    for (;;)
    {
        attempt++;

        errno   = 0;
        channel = at_channel_create(CELLULAR_MODEM_AT_DEVICE, &config);

        if (channel == NULL)
        {
            error = errno != 0 ? -errno : -EIO;

            CELLULAR_DEBUG("AT channel create failed, device=%s, attempt=%u, error=%d", CELLULAR_MODEM_AT_DEVICE, attempt, error);
        }
        else
        {
            ret = at_channel_start(channel);
            if (ret != 0)
            {
                CELLULAR_DEBUG("AT channel start failed, attempt=%u, error=%d", attempt, ret);
            }

            if (ret == 0)
            {
                ret = rg255_cmd_test(channel);
                if (ret != 0)
                {
                    CELLULAR_DEBUG("RG255 AT probe failed, attempt=%u, error=%d", attempt, ret);
                }
            }

            if (ret == 0)
            {
                ret = _cellular_modem_apply_at_baseline(channel);
                if (ret != 0)
                {
                    CELLULAR_DEBUG("RG255 AT baseline failed, attempt=%u, error=%d", attempt, ret);
                }
            }

            if (ret == 0)
            {
                now_ms = linkg_time_elapsed_ms();
                *out   = channel;

                CELLULAR_INFO("AT channel ready, device=%s, attempts=%u, elapsed_ms=%llu", CELLULAR_MODEM_AT_DEVICE, attempt, (unsigned long long)(now_ms - started_ms));

                return 0;
            }

            error = ret;
            at_channel_destroy(channel);
        }

        now_ms = linkg_time_elapsed_ms();

        if (now_ms - started_ms >= CELLULAR_MODEM_AT_READY_TIMEOUT_MS)
        {
            CELLULAR_WARN("AT channel ready timeout, device=%s, attempts=%u, elapsed_ms=%llu, error=%d", CELLULAR_MODEM_AT_DEVICE, attempt, (unsigned long long)(now_ms - started_ms), error);

            return error;
        }

        ret = linkg_time_sleep_ms(CELLULAR_MODEM_AT_READY_RETRY_MS);
        if (ret != 0)
        {
            return ret;
        }
    }
}

/****************************** 持久配置 ******************************/

/**
 * @brief 返回项目硬件要求的SIM插入有效电平。
 */
static rg255_sim_insert_level_t _cellular_modem_required_sim_insert_level(void)
{
    return LINKG_RESOURCE_CELLULAR_SIM_INSERT_ACTIVE_HIGH ? RG255_SIM_INSERT_LEVEL_HIGH : RG255_SIM_INSERT_LEVEL_LOW;
}

/**
 * @brief 检查并按需收敛RG255持久运行配置。
 *
 * @note apply为true时允许修正不一致配置；apply为false时只验证，
 *       任一配置不满足LinkG要求立即返回-EPROTO。
 */
static int _cellular_modem_converge_persistent_config(at_channel_t *channel, const linkg_cellular_config_t *config, bool apply, bool *changed)
{
    rg255_sim_detect_config_t     sim_detect;
    rg255_sim_status_urc_t        sim_urc;
    linkg_cellular_network_mode_t network_mode;
    rg255_network_card_mode_t     card_mode;
    rg255_sim_insert_level_t      insert_level;
    rg255_usbnet_mode_t           usbnet_mode;
    bool                          local_changed;
    int                           ret;

    if (channel == NULL || config == NULL || changed == NULL)
    {
        return -EINVAL;
    }

    local_changed = false;
    insert_level  = _cellular_modem_required_sim_insert_level();

    ret = rg255_query_usbnet_mode(channel, &usbnet_mode);
    if (ret != 0)
    {
        CELLULAR_DEBUG("query persistent config failed, item=usbnet, error=%d", ret);
        return ret;
    }

    if (usbnet_mode != RG255_USBNET_MODE_ECM)
    {
        if (!apply)
        {
            CELLULAR_DEBUG("persistent config verification failed, item=usbnet, current=%d, expected=%d", (int)usbnet_mode, (int)RG255_USBNET_MODE_ECM);
            return -EPROTO;
        }

        ret = rg255_cmd_set_usbnet(channel, RG255_USBNET_MODE_ECM);
        if (ret != 0)
        {
            CELLULAR_DEBUG("update persistent config failed, item=usbnet, old=%d, new=%d, error=%d", (int)usbnet_mode, (int)RG255_USBNET_MODE_ECM, ret);
            return ret;
        }

        CELLULAR_INFO("persistent config updated, item=usbnet, old=%d, new=%d", (int)usbnet_mode, (int)RG255_USBNET_MODE_ECM);
        local_changed = true;
    }

    ret = rg255_query_network_card_mode(channel, &card_mode);
    if (ret != 0)
    {
        CELLULAR_DEBUG("query persistent config failed, item=network_card, error=%d", ret);
        return ret;
    }

    if (card_mode != RG255_NETWORK_CARD_MODE_NIC)
    {
        if (!apply)
        {
            CELLULAR_DEBUG("persistent config verification failed, item=network_card, current=%d, expected=%d", (int)card_mode, (int)RG255_NETWORK_CARD_MODE_NIC);
            return -EPROTO;
        }

        ret = rg255_cmd_set_network_card_mode(channel, RG255_NETWORK_CARD_MODE_NIC);
        if (ret != 0)
        {
            CELLULAR_DEBUG("update persistent config failed, item=network_card, old=%d, new=%d, error=%d", (int)card_mode, (int)RG255_NETWORK_CARD_MODE_NIC, ret);
            return ret;
        }

        CELLULAR_INFO("persistent config updated, item=network_card, old=%d, new=%d", (int)card_mode, (int)RG255_NETWORK_CARD_MODE_NIC);
        local_changed = true;
    }

    ret = rg255_query_network_mode(channel, &network_mode);
    if (ret != 0)
    {
        CELLULAR_DEBUG("query persistent config failed, item=network_mode, error=%d", ret);
        return ret;
    }

    if (network_mode != config->network_mode)
    {
        if (!apply)
        {
            CELLULAR_DEBUG("persistent config verification failed, item=network_mode, current=%d, expected=%d", (int)network_mode, (int)config->network_mode);
            return -EPROTO;
        }

        ret = rg255_cmd_set_network_mode(channel, config->network_mode);
        if (ret != 0)
        {
            CELLULAR_DEBUG("update persistent config failed, item=network_mode, old=%d, new=%d, error=%d", (int)network_mode, (int)config->network_mode, ret);
            return ret;
        }

        CELLULAR_INFO("persistent config updated, item=network_mode, old=%d, new=%d", (int)network_mode, (int)config->network_mode);
        local_changed = true;
    }

    memset(&sim_detect, 0, sizeof(sim_detect));

    ret = rg255_query_sim_detect(channel, &sim_detect);
    if (ret != 0)
    {
        CELLULAR_DEBUG("query persistent config failed, item=sim_detect, error=%d", ret);
        return ret;
    }

    if (!sim_detect.enabled || sim_detect.insert_level != insert_level)
    {
        if (!apply)
        {
            CELLULAR_DEBUG("persistent config verification failed, item=sim_detect, enabled=%d, level=%d, expected_enabled=1, expected_level=%d", sim_detect.enabled ? 1 : 0, (int)sim_detect.insert_level, (int)insert_level);
            return -EPROTO;
        }

        ret = rg255_cmd_set_sim_detect(channel, true, insert_level);
        if (ret != 0)
        {
            CELLULAR_DEBUG("update persistent config failed, item=sim_detect, enabled=%d, level=%d, expected_level=%d, error=%d", sim_detect.enabled ? 1 : 0, (int)sim_detect.insert_level, (int)insert_level, ret);
            return ret;
        }

        CELLULAR_INFO("persistent config updated, item=sim_detect, old_enabled=%d, old_level=%d, new_enabled=1, new_level=%d", sim_detect.enabled ? 1 : 0, (int)sim_detect.insert_level, (int)insert_level);
        local_changed = true;
    }

    memset(&sim_urc, 0, sizeof(sim_urc));

    ret = rg255_query_sim_status_urc(channel, &sim_urc);
    if (ret != 0)
    {
        CELLULAR_DEBUG("query persistent config failed, item=sim_status_urc, error=%d", ret);
        return ret;
    }

    if (!sim_urc.enabled)
    {
        if (!apply)
        {
            CELLULAR_DEBUG("persistent config verification failed, item=sim_status_urc, current=0, expected=1");
            return -EPROTO;
        }

        ret = rg255_cmd_set_sim_status_urc(channel, true);
        if (ret != 0)
        {
            CELLULAR_DEBUG("update persistent config failed, item=sim_status_urc, old=0, new=1, error=%d", ret);
            return ret;
        }

        CELLULAR_INFO("persistent config updated, item=sim_status_urc, old=0, new=1");
        local_changed = true;
    }

    *changed = *changed || local_changed;

    return 0;
}

/**
 * @brief 完成持久配置收敛并在必要时只重启RG255一次。
 */
static int _cellular_modem_prepare_persistent_config(at_channel_t **channel, const linkg_cellular_config_t *config)
{
    at_channel_t *replacement;
    bool          changed;
    int           ret;

    if (channel == NULL || *channel == NULL || config == NULL)
    {
        return -EINVAL;
    }

    changed = false;

    ret = _cellular_modem_converge_persistent_config(*channel, config, true, &changed);
    if (ret != 0)
    {
        return ret;
    }

    if (!changed)
    {
        CELLULAR_INFO("persistent config verified, restart_required=0");
        return 0;
    }

    CELLULAR_INFO("persistent config changed, restarting modem");

    ret = rg255_cmd_restart(*channel);
    if (ret != 0)
    {
        return ret;
    }

    at_channel_destroy(*channel);
    *channel = NULL;

    ret = linkg_time_sleep_ms(CELLULAR_MODEM_RESTART_SETTLE_MS);
    if (ret != 0)
    {
        return ret;
    }

    replacement = NULL;
    ret = _cellular_modem_create_ready_channel(&replacement);
    if (ret != 0)
    {
        return ret;
    }

    changed = false;
    ret = _cellular_modem_converge_persistent_config(replacement, config, false, &changed);
    if (ret != 0)
    {
        at_channel_destroy(replacement);
        return ret;
    }

    *channel = replacement;

    CELLULAR_INFO("persistent config verified after modem restart");

    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 启动RG255 AT通道并完成模组持久配置收敛。
 *
 * @note 仅由Owner串行调用。成功后Modem拥有AT通道；失败时释放本次创建的通道。
 */
int cellular_modem_start(const linkg_cellular_config_t *config)
{
    at_channel_t *channel;
    int           ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (g_cellular_modem_channel != NULL)
    {
        return -EALREADY;
    }

    channel = NULL;
    ret = _cellular_modem_create_ready_channel(&channel);
    if (ret != 0)
    {
        return ret;
    }

    ret = _cellular_modem_prepare_persistent_config(&channel, config);
    if (ret != 0)
    {
        if (channel != NULL)
        {
            at_channel_destroy(channel);
        }

        return ret;
    }

    if (channel == NULL)
    {
        return -EIO;
    }

    g_cellular_modem_channel = channel;

    return 0;
}

/**
 * @brief 停止RG255 AT通道并释放Modem持有的资源。
 *
 * @note 调用前必须保证Monitor和Status已经停止使用借用的AT通道。
 */
void cellular_modem_stop(void)
{
    at_channel_t *channel;

    channel = g_cellular_modem_channel;
    g_cellular_modem_channel = NULL;

    if (channel != NULL)
    {
        at_channel_destroy(channel);
    }
}

/****************************** 运行配置 ******************************/

/**
 * @brief 开启最终AT通道上的运行期异步状态上报。
 */
int cellular_modem_enable_runtime_urcs(void)
{
    at_channel_t *channel;
    int           ret;

    channel = g_cellular_modem_channel;
    if (channel == NULL)
    {
        return -ENETDOWN;
    }

    ret = rg255_cmd_set_eps_registration_urc(channel, true);
    if (ret != 0)
    {
        CELLULAR_DEBUG("enable EPS registration URC failed, error=%d", ret);
        return ret;
    }

    ret = rg255_cmd_set_5g_registration_urc(channel, true);
    if (ret != 0)
    {
        CELLULAR_DEBUG("enable 5GS registration URC failed, error=%d", ret);
        return ret;
    }

    ret = rg255_cmd_set_signal_urc(channel, true);
    if (ret != 0)
    {
        CELLULAR_DEBUG("enable signal URC failed, error=%d", ret);
        return ret;
    }

    CELLULAR_DEBUG("runtime URCs enabled");

    return 0;
}

/****************************** 通道查询 ******************************/

/**
 * @brief 获取当前RG255 AT通道的借用引用。
 *
 * @note 返回指针不增加引用计数，不能由调用方销毁；仅在Modem启动到停止期间有效。
 */
at_channel_t *cellular_modem_get_channel(void)
{
    return g_cellular_modem_channel;
}

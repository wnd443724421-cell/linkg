/**
 * @file rg255_cmd.h
 * @brief RG255 AT命令封装接口
 */

#ifndef RG255_CMD_H
#define RG255_CMD_H

#include <stdbool.h>

#include "linkg_cellular_config.h"

#include "at_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 命令常量 ******************************/

#define RG255_PDP_CONTEXT_ID    1U                            // 默认PDP上下文ID
#define RG255_APN_MAX_LENGTH    99U                           // RG255支持的APN最大长度
#define RG255_IMSI_MAX_LENGTH   15U                           // IMSI最大长度
#define RG255_IMSI_BUFFER_SIZE  (RG255_IMSI_MAX_LENGTH + 1U)  // IMSI缓存大小

/****************************** USB网卡协议 ******************************/

typedef enum
{
    RG255_USBNET_MODE_UNKNOWN = 0, // USB网卡接口协议未知
    RG255_USBNET_MODE_ECM     = 1, // ECM接口协议
    RG255_USBNET_MODE_MBIM    = 2, // MBIM接口协议，当前厂家文档标注暂不支持
    RG255_USBNET_MODE_RNDIS   = 3  // RNDIS接口协议
} rg255_usbnet_mode_t;

/****************************** 网卡工作模式 ******************************/

typedef enum
{
    RG255_NETWORK_CARD_MODE_UNKNOWN = -1, // 网卡工作模式未知
    RG255_NETWORK_CARD_MODE_ROUTER  = 0,  // 路由模式，Host获得192.168.X.X地址
    RG255_NETWORK_CARD_MODE_NIC     = 1   // 网卡模式，Host获得实际网络地址
} rg255_network_card_mode_t;

/****************************** 网络设备类型 ******************************/

typedef enum
{
    RG255_NETDEV_TYPE_UNKNOWN    = -1, // USB网卡连接方式未知
    RG255_NETDEV_TYPE_DISCONNECT = 0, // 断开USB网卡连接
    RG255_NETDEV_TYPE_ONCE       = 1, // 单次连接USB网卡
    RG255_NETDEV_TYPE_AUTO       = 3  // 自动连接USB网卡
} rg255_netdev_type_t;

/****************************** 基础命令 ******************************/

int rg255_cmd_test(at_channel_t *channel);
int rg255_cmd_set_echo(at_channel_t *channel, bool enable);
int rg255_cmd_enable_cmee(at_channel_t *channel);
int rg255_cmd_disable_sleep(at_channel_t *channel);

/****************************** SIM接口 ******************************/

int rg255_cmd_query_sim_status(at_channel_t *channel, char *response, int response_size);
int rg255_cmd_enter_pin(at_channel_t *channel, const char *pin);
int rg255_cmd_get_imsi(at_channel_t *channel, char *imsi, int imsi_size);

/****************************** 网络模式 ******************************/

int rg255_cmd_query_network_mode(at_channel_t *channel, char *response, int response_size);
int rg255_cmd_set_network_mode(at_channel_t *channel, linkg_cellular_network_mode_t mode);

/****************************** 网络注册 ******************************/

int rg255_cmd_query_eps_registration(at_channel_t *channel, char *response, int response_size);
int rg255_cmd_query_5g_registration(at_channel_t *channel, char *response, int response_size);

/****************************** 无线状态 ******************************/

int rg255_cmd_query_serving_cell(at_channel_t *channel, char *response, int response_size);

/****************************** USB配置 ******************************/

int rg255_cmd_query_usbnet(at_channel_t *channel, char *response, int response_size);
int rg255_cmd_set_usbnet(at_channel_t *channel, rg255_usbnet_mode_t mode);
int rg255_cmd_query_network_card_mode(at_channel_t *channel, char *response, int response_size);
int rg255_cmd_set_network_card_mode(at_channel_t *channel, rg255_network_card_mode_t mode);
int rg255_cmd_query_network_card_ipv4(at_channel_t *channel, char *response, int response_size);
int rg255_cmd_query_network_card_ipv6(at_channel_t *channel, char *response, int response_size);

/****************************** PDP配置 ******************************/

int rg255_cmd_query_pdp_config(at_channel_t *channel, char *response, int response_size);
int rg255_cmd_set_pdp_context(at_channel_t *channel, const char *apn);
int rg255_cmd_query_pdp_state(at_channel_t *channel, char *response, int response_size);
int rg255_cmd_set_pdp_active(at_channel_t *channel, bool active);
int rg255_cmd_query_pdp_address(at_channel_t *channel, char *response, int response_size);
int rg255_cmd_query_pdp_runtime(at_channel_t *channel, char *response, int response_size);

/****************************** 网络设备 ******************************/

int rg255_cmd_start_netdev(at_channel_t *channel);
int rg255_cmd_stop_netdev(at_channel_t *channel);
int rg255_cmd_query_netdev(at_channel_t *channel, char *response, int response_size);
int rg255_cmd_enable_netdev_auto_keep(at_channel_t *channel);

/****************************** 模块控制 ******************************/

int rg255_cmd_restart(at_channel_t *channel);

#ifdef __cplusplus
}
#endif

#endif

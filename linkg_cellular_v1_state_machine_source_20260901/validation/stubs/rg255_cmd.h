#ifndef RG255_CMD_H
#define RG255_CMD_H
#include <stdbool.h>
#include "linkg_cellular_config.h"
#include "at_channel.h"
#define RG255_PDP_CONTEXT_ID   1U
#define RG255_APN_MAX_LENGTH   99U
#define RG255_IMSI_MAX_LENGTH  15U
#define RG255_IMSI_BUFFER_SIZE (RG255_IMSI_MAX_LENGTH + 1U)
typedef enum
{
    RG255_USBNET_MODE_UNKNOWN = 0,
    RG255_USBNET_MODE_ECM = 1,
    RG255_USBNET_MODE_MBIM = 2,
    RG255_USBNET_MODE_RNDIS = 3
} rg255_usbnet_mode_t;
typedef enum
{
    RG255_NETWORK_CARD_MODE_UNKNOWN = -1,
    RG255_NETWORK_CARD_MODE_ROUTER = 0,
    RG255_NETWORK_CARD_MODE_NIC = 1
} rg255_network_card_mode_t;
typedef enum
{
    RG255_NETDEV_TYPE_UNKNOWN = -1,
    RG255_NETDEV_TYPE_DISCONNECT = 0,
    RG255_NETDEV_TYPE_ONCE = 1,
    RG255_NETDEV_TYPE_AUTO = 3
} rg255_netdev_type_t;
typedef enum
{
    RG255_SIM_INSERT_LEVEL_UNKNOWN = -1,
    RG255_SIM_INSERT_LEVEL_LOW = 0,
    RG255_SIM_INSERT_LEVEL_HIGH = 1
} rg255_sim_insert_level_t;
int rg255_cmd_test(at_channel_t *channel);
int rg255_cmd_set_echo(at_channel_t *channel, bool enable);
int rg255_cmd_enable_cmee(at_channel_t *channel);
int rg255_cmd_disable_sleep(at_channel_t *channel);
int rg255_cmd_enter_pin(at_channel_t *channel, const char *pin);
int rg255_cmd_set_sim_detect(at_channel_t *channel, bool enable, rg255_sim_insert_level_t insert_level);
int rg255_cmd_set_sim_status_urc(at_channel_t *channel, bool enable);
int rg255_cmd_set_network_mode(at_channel_t *channel, linkg_cellular_network_mode_t mode);
int rg255_cmd_set_usbnet(at_channel_t *channel, rg255_usbnet_mode_t mode);
int rg255_cmd_set_network_card_mode(at_channel_t *channel, rg255_network_card_mode_t mode);
int rg255_cmd_set_pdp_context(at_channel_t *channel, const char *apn);
int rg255_cmd_set_pdp_active(at_channel_t *channel, bool active);
int rg255_cmd_start_netdev(at_channel_t *channel);
int rg255_cmd_stop_netdev(at_channel_t *channel);
int rg255_cmd_restart(at_channel_t *channel);
#endif

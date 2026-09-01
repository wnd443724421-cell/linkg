#ifndef RG255_QUERY_H
#define RG255_QUERY_H
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include "linkg_cellular_status.h"
#include "rg255_cmd.h"
typedef enum
{
    RG255_PDP_TYPE_UNKNOWN = 0,
    RG255_PDP_TYPE_IPV4,
    RG255_PDP_TYPE_IPV6,
    RG255_PDP_TYPE_IPV4V6
} rg255_pdp_type_t;
typedef enum
{
    RG255_SIM_INSERT_STATE_UNKNOWN = -1,
    RG255_SIM_INSERT_STATE_REMOVED = 0,
    RG255_SIM_INSERT_STATE_INSERTED = 1
} rg255_sim_insert_state_t;
typedef struct
{
    uint8_t          cid;
    rg255_pdp_type_t pdp_type;
    char             apn[LINKG_CELLULAR_APN_MAX + 1U];
} rg255_pdp_config_t;
typedef struct
{
    bool                     enabled;
    rg255_sim_insert_level_t insert_level;
} rg255_sim_detect_config_t;
typedef struct
{
    bool                     enabled;
    rg255_sim_insert_state_t state;
} rg255_sim_status_urc_t;
int rg255_query_usbnet_mode(at_channel_t *channel, rg255_usbnet_mode_t *mode);
int rg255_query_network_card_mode(at_channel_t *channel, rg255_network_card_mode_t *mode);
int rg255_query_network_mode(at_channel_t *channel, linkg_cellular_network_mode_t *mode);
int rg255_query_sim_detect(at_channel_t *channel, rg255_sim_detect_config_t *config);
int rg255_query_sim_status_urc(at_channel_t *channel, rg255_sim_status_urc_t *status);
int rg255_query_pdp_config(at_channel_t *channel, rg255_pdp_config_t *config);
#endif

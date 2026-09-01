#ifndef LINKG_CELLULAR_STATUS_H
#define LINKG_CELLULAR_STATUS_H
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include "linkg_cellular_config.h"
typedef enum
{
    LINKG_CELLULAR_SIM_STATE_UNKNOWN = 0,
    LINKG_CELLULAR_SIM_STATE_NOT_READY,
    LINKG_CELLULAR_SIM_STATE_ABSENT,
    LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED,
    LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED,
    LINKG_CELLULAR_SIM_STATE_READY
} linkg_cellular_sim_state_t;
typedef enum
{
    LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN = 0,
    LINKG_CELLULAR_REGISTRATION_STATE_NOT_REGISTERED,
    LINKG_CELLULAR_REGISTRATION_STATE_REGISTERING,
    LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED,
    LINKG_CELLULAR_REGISTRATION_STATE_DENIED
} linkg_cellular_registration_state_t;
typedef enum
{
    LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN = 0,
    LINKG_CELLULAR_NETWORK_TYPE_LTE,
    LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA
} linkg_cellular_network_type_t;
typedef struct
{
    bool     valid;
    uint16_t band;
    int32_t  rsrp_dbm;
    bool     rsrp_valid;
    int32_t  rsrq_db;
    bool     rsrq_valid;
    int32_t  sinr_db;
    bool     sinr_valid;
    uint64_t updated_ms;
} linkg_cellular_serving_cell_status_t;
typedef struct
{
    linkg_cellular_network_mode_t network_mode;
    linkg_cellular_sim_state_t    sim_state;
    uint64_t                      network_mode_updated_ms;
    uint64_t                      sim_updated_ms;
} linkg_cellular_local_status_t;
typedef struct
{
    linkg_cellular_registration_state_t  registration;
    uint64_t                             registration_updated_ms;
    linkg_cellular_network_type_t        network_type;
    uint64_t                             network_type_updated_ms;
    linkg_cellular_serving_cell_status_t serving_cell;
} linkg_cellular_network_status_t;
typedef struct
{
    bool            pdp_valid;
    bool            pdp_active;
    uint64_t        pdp_updated_ms;
    bool            ipv4_valid;
    struct in_addr  ipv4;
    bool            global_ipv6_valid;
    struct in6_addr global_ipv6;
    uint64_t        address_updated_ms;
} linkg_cellular_data_status_t;
typedef struct
{
    bool                            partial;
    linkg_cellular_local_status_t   local;
    linkg_cellular_network_status_t network;
    linkg_cellular_data_status_t    data;
} linkg_cellular_status_snapshot_t;
#endif

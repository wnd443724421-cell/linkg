#ifndef CELLULAR_STATUS_H
#define CELLULAR_STATUS_H
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include "linkg_cellular_status.h"
#include "at_channel.h"
typedef uint32_t cellular_status_refresh_mask_t;
#define CELLULAR_STATUS_REFRESH_NONE              0U
#define CELLULAR_STATUS_REFRESH_NETWORK_MODE     (1U << 0)
#define CELLULAR_STATUS_REFRESH_SIM              (1U << 1)
#define CELLULAR_STATUS_REFRESH_REGISTRATION     (1U << 2)
#define CELLULAR_STATUS_REFRESH_RADIO            (1U << 3)
#define CELLULAR_STATUS_REFRESH_PDP              (1U << 4)
#define CELLULAR_STATUS_REFRESH_PDP_ADDRESS      (1U << 5)
#define CELLULAR_STATUS_REFRESH_NETDEV           (1U << 6)
#define CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK (1U << 7)
#define CELLULAR_STATUS_REFRESH_HOST             (1U << 8)
#define CELLULAR_STATUS_REFRESH_ALL             ((1U << 9) - 1U)
typedef struct
{
    bool     confirmed;
    uint64_t attempted_ms;
    uint64_t updated_ms;
    int      last_error;
} cellular_status_meta_t;
typedef struct
{
    cellular_status_meta_t        network_mode_meta;
    linkg_cellular_network_mode_t network_mode;
    cellular_status_meta_t        sim_meta;
    linkg_cellular_sim_state_t    sim_state;
} cellular_status_local_info_t;
typedef struct
{
    cellular_status_meta_t              registration_meta;
    linkg_cellular_registration_state_t registration;
    cellular_status_meta_t              radio_meta;
    bool                                serving_cell_valid;
    linkg_cellular_network_type_t       network_type;
    uint16_t                            band;
    int32_t                             rsrp_dbm;
    bool                                rsrp_valid;
    int32_t                             rsrq_db;
    bool                                rsrq_valid;
    int32_t                             sinr_db;
    bool                                sinr_valid;
} cellular_status_network_info_t;
typedef struct
{
    cellular_status_meta_t active_meta;
    bool                   active;
    cellular_status_meta_t address_meta;
    bool                   ipv4_valid;
    struct in_addr         ipv4;
    bool                   global_ipv6_valid;
    struct in6_addr        global_ipv6;
} cellular_status_pdp_info_t;
typedef enum
{
    CELLULAR_STATUS_NETDEV_MODE_UNKNOWN = 0,
    CELLULAR_STATUS_NETDEV_MODE_DISCONNECT,
    CELLULAR_STATUS_NETDEV_MODE_ONCE,
    CELLULAR_STATUS_NETDEV_MODE_AUTO
} cellular_status_netdev_mode_t;
typedef struct
{
    cellular_status_meta_t        meta;
    cellular_status_netdev_mode_t mode;
    uint8_t                       cid;
    bool                          urc_enabled;
    bool                          connected;
} cellular_status_netdev_state_info_t;
typedef struct
{
    cellular_status_meta_t meta;
    bool                   valid;
    struct in_addr         address;
    struct in_addr         netmask;
    struct in_addr         gateway;
} cellular_status_expected_ipv4_info_t;
typedef struct
{
    cellular_status_meta_t meta;
    bool                   valid;
    struct in6_addr        prefix;
    uint8_t                prefix_length;
    struct in6_addr        gateway;
} cellular_status_expected_ipv6_info_t;
typedef struct
{
    cellular_status_netdev_state_info_t  state;
    cellular_status_expected_ipv4_info_t expected_ipv4;
    cellular_status_expected_ipv6_info_t expected_ipv6;
} cellular_status_netdev_info_t;
typedef struct
{
    cellular_status_meta_t interface_meta;
    bool                   interface_present;
    unsigned int           interface_index;
    cellular_status_meta_t interface_up_meta;
    bool                   interface_up;
    cellular_status_meta_t ipv4_meta;
    bool                   ipv4_valid;
    struct in_addr         ipv4;
    cellular_status_meta_t ipv4_netmask_meta;
    bool                   ipv4_netmask_valid;
    struct in_addr         ipv4_netmask;
    cellular_status_meta_t ipv6_meta;
    bool                   global_ipv6_valid;
    struct in6_addr        global_ipv6;
    cellular_status_meta_t ipv4_route_meta;
    bool                   ipv4_gateway_valid;
    struct in_addr         ipv4_gateway;
    cellular_status_meta_t ipv6_route_meta;
    bool                   ipv6_gateway_valid;
    struct in6_addr        ipv6_gateway;
} cellular_status_host_info_t;
typedef struct
{
    bool                           valid;
    bool                           partial;
    uint64_t                       generation;
    uint64_t                       published_ms;
    uint64_t                       last_attempt_ms;
    uint64_t                       last_complete_ms;
    int                            last_error;
    cellular_status_local_info_t   local;
    cellular_status_network_info_t network;
    cellular_status_pdp_info_t     pdp;
    cellular_status_netdev_info_t  netdev;
    cellular_status_host_info_t    host;
} cellular_status_info_t;
int      cellular_status_init(void);
int      cellular_status_start(at_channel_t *channel);
int      cellular_status_stop(void);
int      cellular_status_deinit(void);
uint64_t cellular_status_get_deadline(void);
int      cellular_status_process(uint64_t now_ms, cellular_status_refresh_mask_t requested);
int      cellular_status_get_info(cellular_status_info_t *info);
#endif

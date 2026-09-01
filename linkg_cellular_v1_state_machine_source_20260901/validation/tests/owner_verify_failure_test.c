
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "linkg_cellular.h"
#include "cellular_monitor.h"
#include "cellular_status.h"
#include "rg255_query.h"
#include "rg255_cmd.h"
#include "linkg_thread.h"
#include "linkg_time.h"

struct at_channel
{
    int dummy;
};

static struct at_channel g_channel_object;
static cellular_status_info_t g_info;
static uint64_t g_now_ms = 1000U;
static bool g_pdp_active;
static bool g_netdev_connected;
static int g_monitor_fd = -1;
static int g_owner_fd = -1;
static unsigned int g_probe_count;
static unsigned int g_stop_netdev_calls;
static unsigned int g_deactivate_calls;

static void meta_ok(cellular_status_meta_t *meta)
{
    meta->confirmed = true;
    meta->attempted_ms = g_now_ms;
    meta->updated_ms = g_now_ms;
    meta->last_error = 0;
}

int linkg_cellular_config_validate(const linkg_cellular_config_t *config)
{
    return config == NULL ? -EINVAL : 0;
}

uint64_t linkg_time_elapsed_ms(void)
{
    return ++g_now_ms;
}

uint64_t linkg_time_monotonic_us(void)
{
    return linkg_time_elapsed_ms() * 1000U;
}

uint64_t linkg_time_elapsed_us(void)
{
    return linkg_time_elapsed_ms() * 1000U;
}

int linkg_time_sleep_ms(uint32_t milliseconds)
{
    g_now_ms += milliseconds;
    return 0;
}

int linkg_time_sleep_us(uint32_t microseconds)
{
    g_now_ms += microseconds / 1000U;
    return 0;
}

int linkg_time_sleep_until_us(uint64_t deadline_us)
{
    g_now_ms = deadline_us / 1000U;
    return 0;
}

at_channel_t *at_channel_create(const char *device, const uart_config_t *config)
{
    assert(device != NULL);
    assert(config != NULL);
    return &g_channel_object;
}

int at_channel_start(at_channel_t *channel)
{
    return channel == NULL ? -EINVAL : 0;
}

int at_channel_stop(at_channel_t *channel)
{
    return channel == NULL ? -EINVAL : 0;
}

void at_channel_destroy(at_channel_t *channel)
{
    (void)channel;
}

int at_channel_register_urc(at_channel_t *channel, at_urc_callback_t callback, void *context)
{
    (void)callback;
    (void)context;
    return channel == NULL ? -EINVAL : 0;
}

int at_channel_exec(at_channel_t *channel, const char *command, const at_command_config_t *config, char *response, int response_size)
{
    (void)channel;
    (void)command;
    (void)config;
    (void)response;
    (void)response_size;
    return 0;
}

int cellular_monitor_init(void)
{
    if (g_monitor_fd < 0)
    {
        g_monitor_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    }
    return g_monitor_fd < 0 ? -errno : 0;
}

int cellular_monitor_start(at_channel_t *channel)
{
    return channel == NULL ? -EINVAL : 0;
}

int cellular_monitor_stop(void)
{
    return 0;
}

void cellular_monitor_deinit(void)
{
    if (g_monitor_fd >= 0)
    {
        close(g_monitor_fd);
        g_monitor_fd = -1;
    }
}

int cellular_monitor_get_event_fd(void)
{
    return g_monitor_fd;
}

int cellular_monitor_take_events(cellular_monitor_events_t *events)
{
    if (events == NULL)
    {
        return -EINVAL;
    }

    memset(events, 0, sizeof(*events));
    return 0;
}

int cellular_status_init(void)
{
    memset(&g_info, 0, sizeof(g_info));
    return 0;
}

int cellular_status_start(at_channel_t *channel)
{
    return channel == NULL ? -EINVAL : 0;
}

int cellular_status_stop(void)
{
    return 0;
}

int cellular_status_deinit(void)
{
    return 0;
}

uint64_t cellular_status_get_deadline(void)
{
    return g_now_ms;
}

int cellular_status_process(uint64_t now_ms, cellular_status_refresh_mask_t requested)
{
    (void)requested;
    g_now_ms = now_ms;

    memset(&g_info, 0, sizeof(g_info));
    g_info.valid = true;

    meta_ok(&g_info.local.network_mode_meta);
    g_info.local.network_mode = LINKG_CELLULAR_NETWORK_MODE_AUTO;

    meta_ok(&g_info.local.sim_meta);
    g_info.local.sim_state = LINKG_CELLULAR_SIM_STATE_READY;

    meta_ok(&g_info.network.registration_meta);
    g_info.network.registration = LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED;

    meta_ok(&g_info.network.radio_meta);
    g_info.network.serving_cell_valid = true;
    g_info.network.network_type = LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA;

    meta_ok(&g_info.pdp.active_meta);
    g_info.pdp.active = g_pdp_active;

    meta_ok(&g_info.pdp.address_meta);
    g_info.pdp.global_ipv6_valid = g_pdp_active;

    meta_ok(&g_info.netdev.state.meta);
    g_info.netdev.state.cid = 1U;
    g_info.netdev.state.connected = g_netdev_connected;

    meta_ok(&g_info.netdev.expected_ipv4.meta);
    g_info.netdev.expected_ipv4.valid = g_netdev_connected;
    g_info.netdev.expected_ipv4.address.s_addr = 0x0100000aU;
    g_info.netdev.expected_ipv4.netmask.s_addr = 0x00ffffffU;
    g_info.netdev.expected_ipv4.gateway.s_addr = 0x0200000aU;

    meta_ok(&g_info.netdev.expected_ipv6.meta);
    g_info.netdev.expected_ipv6.valid = g_netdev_connected;
    g_info.netdev.expected_ipv6.prefix_length = 64U;
    g_info.netdev.expected_ipv6.prefix.s6_addr[0] = 0x20U;
    g_info.netdev.expected_ipv6.prefix.s6_addr[1] = 0x01U;
    g_info.netdev.expected_ipv6.gateway.s6_addr[0] = 0xfeU;
    g_info.netdev.expected_ipv6.gateway.s6_addr[1] = 0x80U;
    g_info.netdev.expected_ipv6.gateway.s6_addr[15] = 1U;

    meta_ok(&g_info.host.interface_meta);
    meta_ok(&g_info.host.interface_up_meta);
    meta_ok(&g_info.host.ipv4_meta);
    meta_ok(&g_info.host.ipv4_netmask_meta);
    meta_ok(&g_info.host.ipv6_meta);
    meta_ok(&g_info.host.ipv4_route_meta);
    meta_ok(&g_info.host.ipv6_route_meta);

    g_info.host.interface_present = g_netdev_connected;
    g_info.host.interface_up = g_netdev_connected;
    g_info.host.ipv4_valid = g_netdev_connected;
    g_info.host.ipv4 = g_info.netdev.expected_ipv4.address;
    g_info.host.ipv4_netmask_valid = g_netdev_connected;
    g_info.host.ipv4_netmask = g_info.netdev.expected_ipv4.netmask;
    g_info.host.ipv4_gateway_valid = g_netdev_connected;
    g_info.host.ipv4_gateway = g_info.netdev.expected_ipv4.gateway;
    g_info.host.global_ipv6_valid = g_netdev_connected;
    g_info.host.global_ipv6.s6_addr[0] = 0x20U;
    g_info.host.global_ipv6.s6_addr[1] = 0x01U;
    g_info.host.global_ipv6.s6_addr[15] = 2U;
    g_info.host.ipv6_gateway_valid = g_netdev_connected;
    g_info.host.ipv6_gateway = g_info.netdev.expected_ipv6.gateway;

    return 0;
}

int cellular_status_get_info(cellular_status_info_t *info)
{
    if (info == NULL)
    {
        return -EINVAL;
    }
    *info = g_info;
    return 0;
}

int cellular_status_get_snapshot(linkg_cellular_status_snapshot_t *snapshot)
{
    (void)snapshot;
    return 0;
}

int rg255_cmd_test(at_channel_t *channel)
{
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_set_echo(at_channel_t *channel, bool enable)
{
    (void)enable;
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_enable_cmee(at_channel_t *channel)
{
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_disable_sleep(at_channel_t *channel)
{
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_set_usbnet(at_channel_t *channel, rg255_usbnet_mode_t mode)
{
    (void)mode;
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_set_network_card_mode(at_channel_t *channel, rg255_network_card_mode_t mode)
{
    (void)mode;
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_set_network_mode(at_channel_t *channel, linkg_cellular_network_mode_t mode)
{
    (void)mode;
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_set_sim_detect(at_channel_t *channel, bool enable, rg255_sim_insert_level_t insert_level)
{
    (void)enable;
    (void)insert_level;
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_set_sim_status_urc(at_channel_t *channel, bool enable)
{
    (void)enable;
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_set_eps_registration_urc(at_channel_t *channel, bool enable)
{
    (void)enable;
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_set_5g_registration_urc(at_channel_t *channel, bool enable)
{
    (void)enable;
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_set_signal_urc(at_channel_t *channel, bool enable)
{
    (void)enable;
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_restart(at_channel_t *channel)
{
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_enter_pin(at_channel_t *channel, const char *pin)
{
    (void)pin;
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_set_pdp_context(at_channel_t *channel, const char *apn)
{
    (void)apn;
    return channel == NULL ? -EINVAL : 0;
}

int rg255_cmd_set_pdp_active(at_channel_t *channel, bool active)
{
    if (channel == NULL)
    {
        return -EINVAL;
    }

    if (!active)
    {
        g_deactivate_calls++;
    }

    g_pdp_active = active;
    return 0;
}

int rg255_cmd_start_netdev(at_channel_t *channel)
{
    if (channel == NULL)
    {
        return -EINVAL;
    }

    g_netdev_connected = true;
    return 0;
}

int rg255_cmd_stop_netdev(at_channel_t *channel)
{
    if (channel == NULL)
    {
        return -EINVAL;
    }

    g_stop_netdev_calls++;
    g_netdev_connected = false;
    return 0;
}

int rg255_query_usbnet_mode(at_channel_t *channel, rg255_usbnet_mode_t *mode)
{
    (void)channel;
    *mode = RG255_USBNET_MODE_ECM;
    return 0;
}

int rg255_query_network_card_mode(at_channel_t *channel, rg255_network_card_mode_t *mode)
{
    (void)channel;
    *mode = RG255_NETWORK_CARD_MODE_NIC;
    return 0;
}

int rg255_query_network_mode(at_channel_t *channel, linkg_cellular_network_mode_t *mode)
{
    (void)channel;
    *mode = LINKG_CELLULAR_NETWORK_MODE_AUTO;
    return 0;
}

int rg255_query_sim_detect(at_channel_t *channel, rg255_sim_detect_config_t *config)
{
    (void)channel;
    config->enabled = true;
    config->insert_level = RG255_SIM_INSERT_LEVEL_HIGH;
    return 0;
}

int rg255_query_sim_status_urc(at_channel_t *channel, rg255_sim_status_urc_t *status)
{
    (void)channel;
    status->enabled = true;
    status->state = RG255_SIM_INSERT_STATE_INSERTED;
    return 0;
}

int rg255_query_pdp_config(at_channel_t *channel, rg255_pdp_config_t *config)
{
    (void)channel;
    memset(config, 0, sizeof(*config));
    config->cid = 1U;
    config->pdp_type = RG255_PDP_TYPE_IPV4V6;
    return 0;
}

int linkg_os_run(const char *file, ...)
{
    (void)file;
    g_probe_count++;
    return -EIO;
}

bool linkg_thread_is_running(const linkg_thread_t *thread)
{
    (void)thread;
    return !(g_probe_count >= 3U &&
             g_stop_netdev_calls > 0U &&
             g_deactivate_calls > 0U);
}

int linkg_thread_get_wakeup_fd(const linkg_thread_t *thread)
{
    (void)thread;
    if (g_owner_fd < 0)
    {
        g_owner_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    }
    return g_owner_fd;
}

int linkg_thread_clear_wakeup(linkg_thread_t *thread)
{
    (void)thread;
    return 0;
}

int main(void)
{
    linkg_cellular_config_t config;
    linkg_thread_t owner;
    int ret;

    memset(&config, 0, sizeof(config));
    memset(&owner, 0, sizeof(owner));

    config.enabled = true;
    config.network_mode = LINKG_CELLULAR_NETWORK_MODE_AUTO;

    ret = linkg_cellular_init(&config);
    assert(ret == 0);

    ret = linkg_cellular_start();
    assert(ret == 0);

    ret = linkg_cellular_run(&owner);
    assert(ret == 0);
    assert(g_probe_count == 3U);
    assert(g_stop_netdev_calls == 1U);
    assert(g_deactivate_calls == 1U);
    assert(!g_pdp_active);
    assert(!g_netdev_connected);

    ret = linkg_cellular_stop();
    assert(ret == 0);
    assert(g_stop_netdev_calls == 2U);
    assert(g_deactivate_calls == 2U);

    ret = linkg_cellular_deinit();
    assert(ret == 0);

    if (g_owner_fd >= 0)
    {
        close(g_owner_fd);
    }

    return 0;
}

/**
 * @file rg255_query_test.c
 * @brief RG255 query end-to-end test through UART, AT channel, command and parser.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "at_channel.h"
#include "linkg_log.h"
#include "rg255_cmd.h"
#include "rg255_query.h"

#define TEST_AT_DEVICE "/dev/ttyUSB1"
#define TEST_POLL_COUNT 40
#define TEST_POLL_SECONDS 2

typedef int (*raw_query_fn_t)(at_channel_t *, char *, int);

static int g_pass;
static int g_fail;
static atomic_uint g_urc_count;
static atomic_uint g_qnetdev_urc_count;

static const char *mode_name(linkg_cellular_network_mode_t mode)
{
    switch (mode)
    {
        case LINKG_CELLULAR_NETWORK_MODE_AUTO: return "AUTO";
        case LINKG_CELLULAR_NETWORK_MODE_4G: return "LTE";
        case LINKG_CELLULAR_NETWORK_MODE_5G: return "NR5G-SA";
        default: return "UNKNOWN";
    }
}

static const char *network_name(linkg_cellular_network_type_t type)
{
    switch (type)
    {
        case LINKG_CELLULAR_NETWORK_TYPE_LTE: return "LTE";
        case LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA: return "NR5G-SA";
        default: return "UNKNOWN";
    }
}

static const char *registration_name(linkg_cellular_registration_state_t state)
{
    switch (state)
    {
        case LINKG_CELLULAR_REGISTRATION_STATE_NOT_REGISTERED: return "NOT_REGISTERED";
        case LINKG_CELLULAR_REGISTRATION_STATE_REGISTERING: return "REGISTERING";
        case LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED: return "REGISTERED";
        case LINKG_CELLULAR_REGISTRATION_STATE_FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

static const char *sim_name(linkg_cellular_sim_state_t state)
{
    switch (state)
    {
        case LINKG_CELLULAR_SIM_STATE_NOT_READY: return "NOT_READY";
        case LINKG_CELLULAR_SIM_STATE_ABSENT: return "ABSENT";
        case LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED: return "PIN_REQUIRED";
        case LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED: return "PUK_REQUIRED";
        case LINKG_CELLULAR_SIM_STATE_READY: return "READY";
        default: return "UNKNOWN";
    }
}

static const char *pdp_name(rg255_pdp_type_t type)
{
    switch (type)
    {
        case RG255_PDP_TYPE_IPV4: return "IP";
        case RG255_PDP_TYPE_IPV6: return "IPV6";
        case RG255_PDP_TYPE_IPV4V6: return "IPV4V6";
        default: return "UNKNOWN";
    }
}

static void result(const char *name, bool passed)
{
    if (passed)
    {
        g_pass++;
        printf("[PASS] %s\n", name);
    }
    else
    {
        g_fail++;
        printf("[FAIL] %s\n", name);
    }
}

static void dump_raw(at_channel_t *channel, const char *name, raw_query_fn_t query)
{
    char response[4096];
    int ret;

    response[0] = '\0';
    ret = query(channel, response, sizeof(response));
    printf("       RAW %s ret=%d response=<%s>\n", name, ret, response);
}

static void urc_callback(const char *line, void *context)
{
    (void)context;

    atomic_fetch_add_explicit(&g_urc_count, 1U, memory_order_relaxed);

    if (strncmp(line, "+QNETDEVSTATUS:", strlen("+QNETDEVSTATUS:")) == 0)
    {
        atomic_fetch_add_explicit(&g_qnetdev_urc_count, 1U, memory_order_relaxed);
    }

    printf("[URC] %s\n", line);
}

static bool snapshot(at_channel_t *channel, const char *label, linkg_cellular_network_mode_t *mode_out)
{
    linkg_cellular_sim_state_t sim;
    linkg_cellular_network_mode_t mode;
    linkg_cellular_registration_state_t registration;
    rg255_serving_cell_info_t cell;
    rg255_pdp_config_t pdp;
    rg255_pdp_address_t address;
    char imsi[RG255_IMSI_BUFFER_SIZE];
    char ipv4[INET_ADDRSTRLEN];
    char ipv6[INET6_ADDRSTRLEN];
    bool nat;
    bool active;
    bool netdev;
    int usbnet;
    int ret;
    bool all_ok;

    printf("\n===== %s =====\n", label);
    all_ok = true;

    ret = rg255_query_sim_state(channel, &sim);
    printf("       state=%s ret=%d\n", sim_name(sim), ret);
    result("SIM", ret == 0 && sim == LINKG_CELLULAR_SIM_STATE_READY);
    if (ret != 0) dump_raw(channel, "CPIN", rg255_cmd_query_sim_status);
    all_ok = all_ok && ret == 0 && sim == LINKG_CELLULAR_SIM_STATE_READY;

    ret = rg255_query_network_mode(channel, &mode);
    printf("       mode=%s ret=%d\n", mode_name(mode), ret);
    result("NETWORK_MODE", ret == 0 && mode != LINKG_CELLULAR_NETWORK_MODE_UNKNOWN);
    if (ret != 0) dump_raw(channel, "MODE", rg255_cmd_query_network_mode);
    all_ok = all_ok && ret == 0 && mode != LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;
    if (ret == 0 && mode_out != NULL) *mode_out = mode;

    ret = rg255_query_serving_cell(channel, &cell);
    printf("       type=%s band=%u rsrp=%d/%d rsrq=%d/%d sinr=%d/%d ret=%d\n",
           network_name(cell.network_type), cell.band,
           cell.rsrp_dbm, cell.rsrp_valid, cell.rsrq_db, cell.rsrq_valid,
           cell.sinr_db, cell.sinr_valid, ret);
    result("SERVING_CELL",
           ret == 0 && cell.network_type != LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN &&
           cell.band > 0U && cell.rsrp_valid && cell.rsrq_valid && cell.sinr_valid);
    if (ret != 0) dump_raw(channel, "QENG", rg255_cmd_query_serving_cell);
    all_ok = all_ok && ret == 0 && cell.network_type != LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;

    ret = rg255_query_registration(channel, mode, cell.network_type, &registration);
    printf("       registration=%s ret=%d\n", registration_name(registration), ret);
    result("REGISTRATION", ret == 0 && registration == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED);
    if (ret != 0)
    {
        dump_raw(channel, "CEREG", rg255_cmd_query_eps_registration);
        dump_raw(channel, "C5GREG", rg255_cmd_query_5g_registration);
    }
    all_ok = all_ok && ret == 0 && registration == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED;

    ret = rg255_query_usbnet_mode(channel, &usbnet);
    printf("       usbnet=%d ret=%d\n", usbnet, ret);
    result("USBNET", ret == 0 && usbnet == (int)RG255_USBNET_ECM);
    if (ret != 0) dump_raw(channel, "USBNET", rg255_cmd_query_usbnet);
    all_ok = all_ok && ret == 0 && usbnet == (int)RG255_USBNET_ECM;

    ret = rg255_query_nat_enabled(channel, &nat);
    printf("       nat=%s ret=%d\n", nat ? "true" : "false", ret);
    result("NAT", ret == 0 && nat);
    if (ret != 0) dump_raw(channel, "NAT", rg255_cmd_query_nat);
    all_ok = all_ok && ret == 0 && nat;

    ret = rg255_query_pdp_config(channel, &pdp);
    printf("       cid=%u type=%s apn=%s ret=%d\n", pdp.cid, pdp_name(pdp.pdp_type), pdp.apn, ret);
    result("PDP_CONFIG",
           ret == 0 && pdp.cid == RG255_PDP_CONTEXT_ID &&
           pdp.pdp_type == RG255_PDP_TYPE_IPV4V6 && pdp.apn[0] != '\0');
    if (ret != 0) dump_raw(channel, "CGDCONT", rg255_cmd_query_pdp_config);
    all_ok = all_ok && ret == 0 && pdp.cid == RG255_PDP_CONTEXT_ID;

    ret = rg255_query_pdp_active(channel, &active);
    printf("       pdp_active=%s ret=%d\n", active ? "true" : "false", ret);
    result("PDP_ACTIVE", ret == 0 && active);
    if (ret != 0) dump_raw(channel, "CGACT", rg255_cmd_query_pdp_state);
    all_ok = all_ok && ret == 0 && active;

    memset(ipv4, 0, sizeof(ipv4));
    memset(ipv6, 0, sizeof(ipv6));
    ret = rg255_query_pdp_address(channel, &address);
    if (ret == 0 && address.ipv4_valid) (void)inet_ntop(AF_INET, &address.ipv4, ipv4, sizeof(ipv4));
    if (ret == 0 && address.ipv6_valid) (void)inet_ntop(AF_INET6, &address.ipv6, ipv6, sizeof(ipv6));
    printf("       ipv4=%s valid=%d ipv6=%s valid=%d ret=%d\n",
           ipv4, address.ipv4_valid, ipv6, address.ipv6_valid, ret);
    result("PDP_ADDRESS", ret == 0 && address.ipv4_valid && address.ipv6_valid);
    if (ret != 0) dump_raw(channel, "CGPADDR", rg255_cmd_query_pdp_address);
    all_ok = all_ok && ret == 0 && address.ipv4_valid && address.ipv6_valid;

    ret = rg255_query_netdev_active(channel, &netdev);
    printf("       qnetdev=%s ret=%d\n", netdev ? "true" : "false", ret);
    result("QNETDEV", ret == 0 && netdev);
    if (ret != 0) dump_raw(channel, "QNETDEV", rg255_cmd_query_netdev);
    all_ok = all_ok && ret == 0 && netdev;

    memset(imsi, 0, sizeof(imsi));
    ret = rg255_cmd_get_imsi(channel, imsi, sizeof(imsi));
    printf("       imsi=%s ret=%d\n", imsi, ret);
    result("IMSI_CMD_LAYER", ret == 0 && strlen(imsi) > 0U);
    all_ok = all_ok && ret == 0 && strlen(imsi) > 0U;

    return all_ok;
}

static bool expected_rat(linkg_cellular_network_mode_t mode, linkg_cellular_network_type_t type)
{
    if (mode == LINKG_CELLULAR_NETWORK_MODE_4G)
    {
        return type == LINKG_CELLULAR_NETWORK_TYPE_LTE;
    }

    if (mode == LINKG_CELLULAR_NETWORK_MODE_5G)
    {
        return type == LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA;
    }

    if (mode == LINKG_CELLULAR_NETWORK_MODE_AUTO)
    {
        return type == LINKG_CELLULAR_NETWORK_TYPE_LTE ||
               type == LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA;
    }

    return false;
}

static bool transition_ret_allowed(int ret)
{
    return ret == 0 || ret == -ENODATA || ret == -EREMOTEIO ||
           ret == -ETIMEDOUT || ret == -EAGAIN;
}

static bool wait_mode(at_channel_t *channel, linkg_cellular_network_mode_t requested, const char *label)
{
    linkg_cellular_network_mode_t mode;
    linkg_cellular_registration_state_t registration;
    rg255_serving_cell_info_t cell;
    rg255_pdp_address_t address;
    bool pdp_active;
    bool netdev;
    bool parse_error;
    int mode_ret;
    int cell_ret;
    int reg_ret;
    int pdp_ret;
    int addr_ret;
    int netdev_ret;
    int set_ret;
    int poll_index;

    printf("\n===== SWITCH %s =====\n", label);
    set_ret = rg255_cmd_set_network_mode(channel, requested);

    if (set_ret != 0)
    {
        printf("[FAIL] %s set_mode ret=%d\n", label, set_ret);
        g_fail++;
        return false;
    }

    parse_error = false;

    for (poll_index = 1; poll_index <= TEST_POLL_COUNT; poll_index++)
    {
        mode = LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;
        memset(&cell, 0, sizeof(cell));
        registration = LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;
        pdp_active = false;
        memset(&address, 0, sizeof(address));
        netdev = false;

        mode_ret = rg255_query_network_mode(channel, &mode);
        cell_ret = rg255_query_serving_cell(channel, &cell);
        reg_ret = rg255_query_registration(channel,
                                           mode_ret == 0 ? mode : requested,
                                           cell_ret == 0 ? cell.network_type : LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN,
                                           &registration);
        pdp_ret = rg255_query_pdp_active(channel, &pdp_active);
        addr_ret = rg255_query_pdp_address(channel, &address);
        netdev_ret = rg255_query_netdev_active(channel, &netdev);

        printf("[OBS] %s poll=%d mode=%s/%d type=%s/%d reg=%s/%d "
               "pdp=%d/%d ipv4=%d ipv6=%d addr_ret=%d netdev=%d/%d\n",
               label, poll_index, mode_name(mode), mode_ret,
               network_name(cell.network_type), cell_ret,
               registration_name(registration), reg_ret,
               pdp_active, pdp_ret, address.ipv4_valid, address.ipv6_valid,
               addr_ret, netdev, netdev_ret);

        if (!transition_ret_allowed(mode_ret) || !transition_ret_allowed(cell_ret) ||
            !transition_ret_allowed(reg_ret) || !transition_ret_allowed(pdp_ret) ||
            !transition_ret_allowed(addr_ret) || !transition_ret_allowed(netdev_ret))
        {
            parse_error = true;
            printf("[FAIL] %s unexpected parser result mode=%d cell=%d reg=%d pdp=%d addr=%d netdev=%d\n",
                   label, mode_ret, cell_ret, reg_ret, pdp_ret, addr_ret, netdev_ret);
            if (cell_ret != 0) dump_raw(channel, "QENG", rg255_cmd_query_serving_cell);
            if (pdp_ret != 0) dump_raw(channel, "CGACT", rg255_cmd_query_pdp_state);
            if (addr_ret != 0) dump_raw(channel, "CGPADDR", rg255_cmd_query_pdp_address);
            break;
        }

        if (mode_ret == 0 && mode == requested &&
            cell_ret == 0 && expected_rat(requested, cell.network_type) &&
            cell.band > 0U && cell.rsrp_valid && cell.rsrq_valid && cell.sinr_valid &&
            reg_ret == 0 && registration == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED &&
            pdp_ret == 0 && pdp_active &&
            addr_ret == 0 && address.ipv4_valid && address.ipv6_valid &&
            netdev_ret == 0 && netdev)
        {
            result(label, !parse_error);
            return !parse_error;
        }

        sleep(TEST_POLL_SECONDS);
    }

    result(label, false);
    return false;
}

int main(int argc, char **argv)
{
    uart_config_t uart_config;
    at_channel_t *channel;
    linkg_cellular_network_mode_t original_mode;
    bool full;
    bool original_known;
    bool restored;
    int ret;

    full = argc <= 1 || strcmp(argv[1], "--full") == 0;
    original_mode = LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;
    original_known = false;
    restored = false;
    channel = NULL;

    (void)linkg_log_init(LINKG_LOG_LEVEL_DEBUG);
    (void)linkg_log_add_console_output(false, false);

    memset(&uart_config, 0, sizeof(uart_config));
    uart_config.baudrate = 115200;
    uart_config.data_bits = 8;
    uart_config.stop_bits = 1;
    uart_config.parity = 'n';
    uart_config.hw_flow_control = false;
    uart_config.exclusive = true;

    channel = at_channel_create(TEST_AT_DEVICE, &uart_config);

    if (channel == NULL)
    {
        printf("[FAIL] CHANNEL_CREATE errno=%d\n", errno);
        g_fail++;
        goto done;
    }

    ret = at_channel_register_urc(channel, urc_callback, NULL);

    if (ret != 0)
    {
        printf("[FAIL] URC_REGISTER ret=%d\n", ret);
        g_fail++;
        goto done;
    }

    ret = at_channel_start(channel);

    if (ret != 0)
    {
        printf("[FAIL] CHANNEL_START ret=%d\n", ret);
        g_fail++;
        goto done;
    }

    ret = rg255_query_network_mode(channel, &original_mode);
    original_known = ret == 0 && original_mode != LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;
    printf("ORIGINAL_MODE=%s ret=%d\n", mode_name(original_mode), ret);
    result("ORIGINAL_MODE_SAVED", original_known);

    (void)snapshot(channel, "CURRENT MODE BASELINE", NULL);

    if (full && original_known)
    {
        (void)wait_mode(channel, LINKG_CELLULAR_NETWORK_MODE_4G, "LTE");
        (void)wait_mode(channel, LINKG_CELLULAR_NETWORK_MODE_5G, "NR5G-SA");
        (void)wait_mode(channel, LINKG_CELLULAR_NETWORK_MODE_AUTO, "AUTO");
    }

    if (original_known)
    {
        restored = wait_mode(channel, original_mode, "RESTORE_ORIGINAL");
        result("ORIGINAL_MODE_RESTORED", restored);
        (void)snapshot(channel, "POST-RESTORE BASELINE", NULL);
    }

    if (full)
    {
        printf("URC_COUNT=%u QNETDEVSTATUS_URC_COUNT=%u\n",
               atomic_load_explicit(&g_urc_count, memory_order_relaxed),
               atomic_load_explicit(&g_qnetdev_urc_count, memory_order_relaxed));
        result("URC_ISOLATION_OBSERVED",
               atomic_load_explicit(&g_qnetdev_urc_count, memory_order_relaxed) > 0U);
    }

done:
    if (channel != NULL)
    {
        (void)at_channel_stop(channel);
        at_channel_destroy(channel);
    }

    linkg_log_deinit();

    printf("\nRG255 QUERY TEST: %d PASS / %d FAIL\n", g_pass, g_fail);

    return g_fail == 0 ? 0 : 1;
}

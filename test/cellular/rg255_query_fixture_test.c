/**
 * @file rg255_query_fixture_test.c
 * @brief RG255 query parser tests derived from the 2026-07-28 RTOS AT manual.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "rg255_cmd.h"
#include "rg255_query.h"

typedef enum
{
    MOCK_SIM = 0,
    MOCK_MODE,
    MOCK_CEREG,
    MOCK_C5GREG,
    MOCK_QENG,
    MOCK_USBNET,
    MOCK_NETCARD,
    MOCK_CGDCONT,
    MOCK_CGACT,
    MOCK_CGPADDR,
    MOCK_QNETDEV,
    MOCK_COUNT
} mock_id_t;

typedef struct
{
    const char *response;
    int result;
} mock_slot_t;

static mock_slot_t g_mock[MOCK_COUNT];
static unsigned int g_calls[MOCK_COUNT];
static unsigned char g_dummy_channel;
static int g_pass;
static int g_fail;

static at_channel_t *fixture_channel(void)
{
    return (at_channel_t *)&g_dummy_channel;
}

static void mock_reset(void)
{
    memset(g_mock, 0, sizeof(g_mock));
    memset(g_calls, 0, sizeof(g_calls));
}

static void mock_set(mock_id_t id, const char *response, int result)
{
    g_mock[id].response = response;
    g_mock[id].result = result;
}

static int mock_reply(mock_id_t id, at_channel_t *channel, char *response, int response_size)
{
    size_t length;

    (void)channel;
    g_calls[id]++;

    if (response == NULL || response_size <= 0)
    {
        return -EINVAL;
    }

    response[0] = '\0';

    if (g_mock[id].response != NULL)
    {
        length = strlen(g_mock[id].response);

        if (length >= (size_t)response_size)
        {
            return -ENOSPC;
        }

        memcpy(response, g_mock[id].response, length + 1U);
    }

    return g_mock[id].result;
}

#define MOCK_QUERY(function_name, mock_id) \
    int function_name(at_channel_t *channel, char *response, int response_size) \
    { \
        return mock_reply((mock_id), channel, response, response_size); \
    }

MOCK_QUERY(rg255_cmd_query_sim_pin_status, MOCK_SIM)
MOCK_QUERY(rg255_cmd_query_network_mode, MOCK_MODE)
MOCK_QUERY(rg255_cmd_query_eps_registration, MOCK_CEREG)
MOCK_QUERY(rg255_cmd_query_5g_registration, MOCK_C5GREG)
MOCK_QUERY(rg255_cmd_query_serving_cell, MOCK_QENG)
MOCK_QUERY(rg255_cmd_query_usbnet, MOCK_USBNET)
MOCK_QUERY(rg255_cmd_query_network_card_mode, MOCK_NETCARD)
MOCK_QUERY(rg255_cmd_query_network_card_ipv4, MOCK_NETCARD)
MOCK_QUERY(rg255_cmd_query_network_card_ipv6, MOCK_NETCARD)
MOCK_QUERY(rg255_cmd_query_pdp_config, MOCK_CGDCONT)
MOCK_QUERY(rg255_cmd_query_pdp_state, MOCK_CGACT)
MOCK_QUERY(rg255_cmd_query_pdp_address, MOCK_CGPADDR)
MOCK_QUERY(rg255_cmd_query_netdev, MOCK_QNETDEV)

static void check_case(const char *name, bool passed)
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

static void test_sim(void)
{
    static const struct
    {
        const char *response;
        int command_result;
        linkg_cellular_sim_state_t expected;
    } cases[] =
    {
        {"+CPIN: READY", 0, LINKG_CELLULAR_SIM_STATE_READY},
        {"+CPIN: SIM PIN", 0, LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED},
        {"+CPIN: SIM PUK", 0, LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED},
        {"+CPIN: NOT INSERTED", 0, LINKG_CELLULAR_SIM_STATE_ABSENT},
        {"+CPIN: NOT READY", 0, LINKG_CELLULAR_SIM_STATE_NOT_READY},
        {"+CME ERROR: 10", -EREMOTEIO, LINKG_CELLULAR_SIM_STATE_ABSENT},
        {"+CME ERROR: (U)SIM not inserted", -EREMOTEIO, LINKG_CELLULAR_SIM_STATE_ABSENT}
    };
    linkg_cellular_sim_state_t state;
    size_t index;
    int ret;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        mock_reset();
        mock_set(MOCK_SIM, cases[index].response, cases[index].command_result);
        ret = rg255_query_sim_state(fixture_channel(), &state);
        check_case(cases[index].response, ret == 0 && state == cases[index].expected);
    }

    mock_set(MOCK_SIM, "", -ETIMEDOUT);
    ret = rg255_query_sim_state(fixture_channel(), &state);
    check_case("SIM command error propagated", ret == -ETIMEDOUT);

    mock_set(MOCK_SIM, "+WRONG: READY", 0);
    ret = rg255_query_sim_state(fixture_channel(), &state);
    check_case("SIM wrong prefix rejected", ret == -ENODATA);
}

static void test_network_mode(void)
{
    linkg_cellular_network_mode_t mode;
    int ret;

    mock_reset();
    mock_set(MOCK_MODE, "+QNWPREFCFG: \"mode_pref\",AUTO", 0);
    ret = rg255_query_network_mode(fixture_channel(), &mode);
    check_case("MODE AUTO", ret == 0 && mode == LINKG_CELLULAR_NETWORK_MODE_AUTO);

    mock_set(MOCK_MODE, "+QNWPREFCFG: \"mode_pref\",LTE", 0);
    ret = rg255_query_network_mode(fixture_channel(), &mode);
    check_case("MODE LTE", ret == 0 && mode == LINKG_CELLULAR_NETWORK_MODE_4G);

    mock_set(MOCK_MODE, "+QNWPREFCFG: \"mode_pref\",NR5G-SA", 0);
    ret = rg255_query_network_mode(fixture_channel(), &mode);
    check_case("MODE NR5G-SA", ret == 0 && mode == LINKG_CELLULAR_NETWORK_MODE_5G);

    mock_set(MOCK_MODE, "+QNWPREFCFG: \"mode_pref\",NR5G-SA:LTE", 0);
    ret = rg255_query_network_mode(fixture_channel(), &mode);
    check_case("MODE NR5G-SA:LTE normalized to AUTO",
               ret == 0 && mode == LINKG_CELLULAR_NETWORK_MODE_AUTO);

    mock_set(MOCK_MODE, "+QNWPREFCFG: \"mode_pref\",LTE:NR5G-SA", 0);
    ret = rg255_query_network_mode(fixture_channel(), &mode);
    check_case("MODE LTE-first explicitly unsupported",
               ret == -EOPNOTSUPP && mode == LINKG_CELLULAR_NETWORK_MODE_UNKNOWN);

    mock_set(MOCK_MODE, "+QNWPREFCFG: \"mode_pref\",WCDMA", 0);
    ret = rg255_query_network_mode(fixture_channel(), &mode);
    check_case("MODE undocumented value rejected", ret == -EBADMSG);

    mock_set(MOCK_MODE, "+QNWPREFCFG: \"wrong\",LTE", 0);
    ret = rg255_query_network_mode(fixture_channel(), &mode);
    check_case("MODE wrong key rejected", ret == -EBADMSG);
}

static void test_registration_domain(mock_id_t id, bool nr)
{
    static const linkg_cellular_registration_state_t expected[] =
    {
        LINKG_CELLULAR_REGISTRATION_STATE_NOT_REGISTERED,
        LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED,
        LINKG_CELLULAR_REGISTRATION_STATE_REGISTERING,
        LINKG_CELLULAR_REGISTRATION_STATE_DENIED,
        LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN,
        LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED
    };
    char response[64];
    char name[64];
    linkg_cellular_registration_state_t state;
    int stat;
    int ret;

    for (stat = 0; stat <= 5; stat++)
    {
        mock_reset();
        snprintf(response, sizeof(response), "%s 0,%d", nr ? "+C5GREG:" : "+CEREG:", stat);
        mock_set(id, response, 0);
        ret = rg255_query_registration(
            fixture_channel(),
            nr ? LINKG_CELLULAR_NETWORK_MODE_5G : LINKG_CELLULAR_NETWORK_MODE_4G,
            LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN,
            &state);
        snprintf(name, sizeof(name), "%s stat=%d", nr ? "C5GREG" : "CEREG", stat);
        check_case(name, ret == 0 && state == expected[stat]);
    }
}

static void test_registration(void)
{
    linkg_cellular_registration_state_t state;
    int ret;

    test_registration_domain(MOCK_CEREG, false);
    test_registration_domain(MOCK_C5GREG, true);

    mock_reset();
    mock_set(MOCK_CEREG, "+CEREG: 0,0", 0);
    mock_set(MOCK_C5GREG, "+C5GREG: 0,1", 0);
    ret = rg255_query_registration(fixture_channel(), LINKG_CELLULAR_NETWORK_MODE_AUTO,
                                   LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN, &state);
    check_case("AUTO merges EPS and 5GS registration",
               ret == 0 && state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED);

    mock_reset();
    mock_set(MOCK_CEREG, "+CEREG: 0,1", 0);
    mock_set(MOCK_C5GREG, "+C5GREG: 0,0", 0);
    ret = rg255_query_registration(fixture_channel(), LINKG_CELLULAR_NETWORK_MODE_AUTO,
                                   LINKG_CELLULAR_NETWORK_TYPE_LTE, &state);
    check_case("AUTO known LTE queries CEREG only",
               ret == 0 && state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED &&
               g_calls[MOCK_CEREG] == 1U && g_calls[MOCK_C5GREG] == 0U);

    mock_reset();
    mock_set(MOCK_C5GREG, "+C5GREG: 0,1", 0);
    ret = rg255_query_registration(fixture_channel(), LINKG_CELLULAR_NETWORK_MODE_AUTO,
                                   LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA, &state);
    check_case("AUTO known NR queries C5GREG only",
               ret == 0 && state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED &&
               g_calls[MOCK_CEREG] == 0U && g_calls[MOCK_C5GREG] == 1U);

    mock_reset();
    mock_set(MOCK_CEREG, "", -ETIMEDOUT);
    mock_set(MOCK_C5GREG, "+C5GREG: 0,1", 0);
    ret = rg255_query_registration(fixture_channel(), LINKG_CELLULAR_NETWORK_MODE_AUTO,
                                   LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN, &state);
    check_case("AUTO accepts registered domain when other query fails",
               ret == 0 && state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED);

    mock_reset();
    mock_set(MOCK_CEREG, "+CEREG: 0,8", 0);
    ret = rg255_query_registration(fixture_channel(), LINKG_CELLULAR_NETWORK_MODE_4G,
                                   LINKG_CELLULAR_NETWORK_TYPE_LTE, &state);
    check_case("REG stat outside manual 0-5 rejected", ret == -EBADMSG);

    mock_set(MOCK_CEREG, "+CEREG: 9,1", 0);
    ret = rg255_query_registration(fixture_channel(), LINKG_CELLULAR_NETWORK_MODE_4G,
                                   LINKG_CELLULAR_NETWORK_TYPE_LTE, &state);
    check_case("REG n outside manual 0-2 rejected", ret == -EBADMSG);
}

static void test_serving_cell(void)
{
    const char *lte =
        "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,00,848459E,207,3590,8,3,3,550B,-70,-6,-63,13,57";
    const char *nr =
        "+QENG: \"servingcell\",\"NOCONN\",\"NR5G-SA\",\"FDD\",460,00,175E7B0010,424,550B,152650,28,20,-85,-13,2,8,35,0";
    rg255_serving_cell_info_t info;
    int ret;

    mock_reset();
    mock_set(MOCK_QENG, lte, 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG manual LTE field layout",
               ret == 0 && info.network_type == LINKG_CELLULAR_NETWORK_TYPE_LTE &&
               info.band == 8U && info.rsrp_valid && info.rsrp_dbm == -70 &&
               info.rsrq_valid && info.rsrq_db == -6 &&
               info.sinr_valid && info.sinr_db == 13);

    mock_set(MOCK_QENG, nr, 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG manual NR5G-SA field layout",
               ret == 0 && info.network_type == LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA &&
               info.band == 28U && info.rsrp_valid && info.rsrp_dbm == -85 &&
               info.rsrq_valid && info.rsrq_db == -13 &&
               info.sinr_valid && info.sinr_db == 2);

    mock_set(MOCK_QENG,
             "+QNETDEVSTATUS: 0\r\n"
             "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,00,848459E,207,3590,8,3,3,550B,-70,-6,-63,13,57", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG unrelated URC ignored", ret == 0 && info.band == 8U);

    mock_set(MOCK_QENG, "+QENG: \"servingcell\",\"SEARCH\"", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG SEARCH returns no data", ret == -ENODATA);

    mock_set(MOCK_QENG,
             "+QENG: \"servingcell\",\"LIMSRV\",\"NR5G-SA\",\"TDD\",460,00,17DAED001,292,46550B,504990,41,20,-83,-10,3,19,34,1", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG LIMSRV metrics accepted",
               ret == 0 && info.band == 41U && info.rsrp_dbm == -83 &&
               info.rsrq_db == -10 && info.sinr_db == 3);

    mock_set(MOCK_QENG,
             "+QENG: \"servingcell\",\"CONNECT\",\"LTE\",\"FDD\",460,00,848459E,207,3590,8,3,3,550B,-70,-6,-63,13,57", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG CONNECT accepted", ret == 0);

    mock_set(MOCK_QENG,
             "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,00,848459E,207,3590,8,3,3,550B,-,-,-,\"-\",57", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG dash metrics invalid but parseable",
               ret == 0 && !info.rsrp_valid && !info.rsrq_valid && !info.sinr_valid);

    mock_set(MOCK_QENG,
             "+QENG: \"servingcell\",\"IDLE\",\"LTE\",\"FDD\",460,00,848459E,207,3590,8,3,3,550B,-70,-6,-63,13,57", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG undocumented state rejected", ret == -EBADMSG);

    mock_set(MOCK_QENG, "+QENG: \"servingcell\",\"NOCONN\",\"WCDMA\",1,2,3", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG unsupported RAT rejected", ret == -EOPNOTSUPP);

    mock_set(MOCK_QENG, "+QENG: \"servingcell\",\"NOCONN\",\"LTE\"", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG missing LTE fields rejected", ret == -EBADMSG);
}

static void test_usb_config(void)
{
    rg255_usbnet_mode_t usbnet;
    rg255_network_card_mode_t card_mode;
    int ret;

    mock_reset();
    mock_set(MOCK_USBNET, "+QCFG: \"usbnet\",1", 0);
    ret = rg255_query_usbnet_mode(fixture_channel(), &usbnet);
    check_case("USBNET ECM", ret == 0 && usbnet == RG255_USBNET_MODE_ECM);

    mock_set(MOCK_USBNET, "+QCFG: \"usbnet\",2", 0);
    ret = rg255_query_usbnet_mode(fixture_channel(), &usbnet);
    check_case("USBNET MBIM query value", ret == 0 && usbnet == RG255_USBNET_MODE_MBIM);

    mock_set(MOCK_USBNET, "+QCFG: \"usbnet\",3", 0);
    ret = rg255_query_usbnet_mode(fixture_channel(), &usbnet);
    check_case("USBNET RNDIS", ret == 0 && usbnet == RG255_USBNET_MODE_RNDIS);

    mock_set(MOCK_USBNET, "+QCFG: \"usbnet\",0", 0);
    ret = rg255_query_usbnet_mode(fixture_channel(), &usbnet);
    check_case("USBNET undocumented value rejected", ret == -EBADMSG);

    mock_set(MOCK_NETCARD, "+QCFG: \"nat\",0", 0);
    ret = rg255_query_network_card_mode(fixture_channel(), &card_mode);
    check_case("NETWORK_CARD router", ret == 0 && card_mode == RG255_NETWORK_CARD_MODE_ROUTER);

    mock_set(MOCK_NETCARD, "+QCFG: \"nat\",1", 0);
    ret = rg255_query_network_card_mode(fixture_channel(), &card_mode);
    check_case("NETWORK_CARD NIC", ret == 0 && card_mode == RG255_NETWORK_CARD_MODE_NIC);

    mock_set(MOCK_NETCARD, "+QCFG: \"nat\",2", 0);
    ret = rg255_query_network_card_mode(fixture_channel(), &card_mode);
    check_case("NETWORK_CARD invalid value rejected", ret == -EBADMSG);
}

static void test_pdp_config(void)
{
    const char *multi =
        "+CGDCONT: 8,\"IPV4V6\",\"IMS\",\"0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0\",0,0\r\n"
        "+CGDCONT: 1,\"IPV4V6\",\"ctnet\",\"0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0\",0,0";
    rg255_pdp_config_t config;
    int ret;

    mock_reset();
    mock_set(MOCK_CGDCONT, multi, 0);
    ret = rg255_query_pdp_config(fixture_channel(), &config);
    check_case("CGDCONT multi-line selects CID 1",
               ret == 0 && config.cid == 1U &&
               config.pdp_type == RG255_PDP_TYPE_IPV4V6 &&
               strcmp(config.apn, "ctnet") == 0);

    mock_set(MOCK_CGDCONT, "+CGDCONT: 8,\"IPV4V6\",\"IMS\"", 0);
    ret = rg255_query_pdp_config(fixture_channel(), &config);
    check_case("CGDCONT missing CID 1", ret == -ENOENT);

    mock_set(MOCK_CGDCONT, "+CGDCONT: 1,\"PPP\",\"bad\"", 0);
    ret = rg255_query_pdp_config(fixture_channel(), &config);
    check_case("CGDCONT unsupported PDP type", ret == -EOPNOTSUPP);
}

static void test_pdp_active(void)
{
    bool active;
    int ret;

    mock_reset();
    mock_set(MOCK_CGACT, "+CGACT: 8,0\r\n+CGACT: 1,1", 0);
    ret = rg255_query_pdp_active(fixture_channel(), &active);
    check_case("CGACT multi-line selects active CID 1", ret == 0 && active);

    mock_set(MOCK_CGACT, "+CGACT: 8,1\r\n+CGACT: 1,0", 0);
    ret = rg255_query_pdp_active(fixture_channel(), &active);
    check_case("CGACT inactive CID 1", ret == 0 && !active);

    mock_set(MOCK_CGACT, "+CGACT: 1,2", 0);
    ret = rg255_query_pdp_active(fixture_channel(), &active);
    check_case("CGACT reserved state rejected", ret == -EBADMSG);
}

static void test_pdp_address(void)
{
    rg255_pdp_address_t address;
    struct in_addr expected4;
    struct in6_addr expected6;
    int ret;

    mock_reset();
    mock_set(MOCK_CGPADDR,
             "+CGPADDR: 1,\"10.195.207.138\",\"240e:476:8c6:6010:0:0:0:1\"", 0);
    ret = rg255_query_pdp_address(fixture_channel(), &address);
    (void)inet_pton(AF_INET, "10.195.207.138", &expected4);
    (void)inet_pton(AF_INET6, "240e:476:8c6:6010::1", &expected6);
    check_case("CGPADDR dual stack",
               ret == 0 && address.ipv4_valid && address.global_ipv6_valid &&
               memcmp(&address.ipv4, &expected4, sizeof(expected4)) == 0 &&
               memcmp(&address.global_ipv6, &expected6, sizeof(expected6)) == 0);

    mock_set(MOCK_CGPADDR, "+CGPADDR: 1,\"0.0.0.0\",\"::\"", 0);
    ret = rg255_query_pdp_address(fixture_channel(), &address);
    check_case("CGPADDR zero addresses invalid",
               ret == 0 && !address.ipv4_valid && !address.global_ipv6_valid);

    mock_set(MOCK_CGPADDR, "+CGPADDR: 1,\"10.1.2.3\",\"FE80::1\"", 0);
    ret = rg255_query_pdp_address(fixture_channel(), &address);
    check_case("CGPADDR link-local IPv6 is not global",
               ret == 0 && address.ipv4_valid && !address.global_ipv6_valid);

    mock_set(MOCK_CGPADDR, "+CGPADDR: 1", 0);
    ret = rg255_query_pdp_address(fixture_channel(), &address);
    check_case("CGPADDR omitted address means no address",
               ret == 0 && !address.ipv4_valid && !address.global_ipv6_valid);

    mock_set(MOCK_CGPADDR, "+CGPADDR: 8,\"10.1.2.3\"", 0);
    ret = rg255_query_pdp_address(fixture_channel(), &address);
    check_case("CGPADDR wrong CID rejected", ret == -EBADMSG);

    mock_set(MOCK_CGPADDR, "+CGPADDR: 1,\"not-an-ip\"", 0);
    ret = rg255_query_pdp_address(fixture_channel(), &address);
    check_case("CGPADDR malformed address rejected", ret == -EBADMSG);
}

static void test_netdev(void)
{
    rg255_netdev_status_t status;
    int ret;

    mock_reset();
    mock_set(MOCK_QNETDEV, "+QNETDEVCTL: 3,1,1,1", 0);
    ret = rg255_query_netdev_status(fixture_channel(), &status);
    check_case("QNETDEV AUTO connected",
               ret == 0 && status.type == RG255_NETDEV_TYPE_AUTO &&
               status.cid == 1U && status.urc_enabled && status.connected);

    mock_set(MOCK_QNETDEV, "+QNETDEVCTL: 0,11,0,0", 0);
    ret = rg255_query_netdev_status(fixture_channel(), &status);
    check_case("QNETDEV disconnected",
               ret == 0 && status.type == RG255_NETDEV_TYPE_DISCONNECT &&
               status.cid == 11U && !status.urc_enabled && !status.connected);

    mock_set(MOCK_QNETDEV, "+QNETDEVCTL: 3,1,1", 0);
    ret = rg255_query_netdev_status(fixture_channel(), &status);
    check_case("QNETDEV missing state rejected", ret == -EBADMSG);

    mock_set(MOCK_QNETDEV, "+QNETDEVCTL: 2,1,1,1", 0);
    ret = rg255_query_netdev_status(fixture_channel(), &status);
    check_case("QNETDEV reserved type rejected", ret == -EBADMSG);

    mock_set(MOCK_QNETDEV, "+QNETDEVCTL: 3,12,1,1", 0);
    ret = rg255_query_netdev_status(fixture_channel(), &status);
    check_case("QNETDEV CID outside 1-11 rejected", ret == -EBADMSG);

    mock_set(MOCK_QNETDEV, "+QNETDEVCTL: 3,1,2,1", 0);
    ret = rg255_query_netdev_status(fixture_channel(), &status);
    check_case("QNETDEV invalid URC flag rejected", ret == -EBADMSG);

    mock_set(MOCK_QNETDEV, "+QNETDEVCTL: 3,1,1,2", 0);
    ret = rg255_query_netdev_status(fixture_channel(), &status);
    check_case("QNETDEV invalid state rejected", ret == -EBADMSG);
}

int main(void)
{
    test_sim();
    test_network_mode();
    test_registration();
    test_serving_cell();
    test_usb_config();
    test_pdp_config();
    test_pdp_active();
    test_pdp_address();
    test_netdev();

    printf("\nRG255 QUERY FIXTURE TEST: %d PASS / %d FAIL\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

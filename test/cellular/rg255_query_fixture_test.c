/**
 * @file rg255_query_fixture_test.c
 * @brief RG255 query parser fixture tests using captured RG255AA responses.
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
    MOCK_NAT,
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
            memcpy(response, g_mock[id].response, (size_t)response_size - 1U);
            response[response_size - 1] = '\0';
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

MOCK_QUERY(rg255_cmd_query_sim_status, MOCK_SIM)
MOCK_QUERY(rg255_cmd_query_network_mode, MOCK_MODE)
MOCK_QUERY(rg255_cmd_query_eps_registration, MOCK_CEREG)
MOCK_QUERY(rg255_cmd_query_5g_registration, MOCK_C5GREG)
MOCK_QUERY(rg255_cmd_query_serving_cell, MOCK_QENG)
MOCK_QUERY(rg255_cmd_query_usbnet, MOCK_USBNET)
MOCK_QUERY(rg255_cmd_query_nat, MOCK_NAT)
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
    linkg_cellular_sim_state_t state;
    int ret;

    mock_reset();
    mock_set(MOCK_SIM, "+CPIN: READY", 0);
    ret = rg255_query_sim_state(fixture_channel(), &state);
    check_case("SIM READY", ret == 0 && state == LINKG_CELLULAR_SIM_STATE_READY);

    mock_set(MOCK_SIM, "+CME ERROR: 10", -EREMOTEIO);
    ret = rg255_query_sim_state(fixture_channel(), &state);
    check_case("SIM CME numeric absent", ret == 0 && state == LINKG_CELLULAR_SIM_STATE_ABSENT);

    mock_set(MOCK_SIM, "", 0);
    ret = rg255_query_sim_state(fixture_channel(), &state);
    check_case("SIM empty response rejected", ret < 0);

    mock_set(MOCK_SIM, "+WRONG: READY", 0);
    ret = rg255_query_sim_state(fixture_channel(), &state);
    check_case("SIM wrong prefix rejected", ret < 0);

    mock_set(MOCK_SIM, "", -ETIMEDOUT);
    ret = rg255_query_sim_state(fixture_channel(), &state);
    check_case("SIM command error propagated", ret == -ETIMEDOUT);
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

    mock_set(MOCK_MODE, "+QNWPREFCFG: \"mode_pref\"", 0);
    ret = rg255_query_network_mode(fixture_channel(), &mode);
    check_case("MODE insufficient fields rejected", ret < 0);

    mock_set(MOCK_MODE, "+QCFG: \"mode_pref\",LTE", 0);
    ret = rg255_query_network_mode(fixture_channel(), &mode);
    check_case("MODE wrong prefix rejected", ret < 0);

    mock_set(MOCK_MODE, "+QNWPREFCFG: \"wrong\",LTE", 0);
    ret = rg255_query_network_mode(fixture_channel(), &mode);
    check_case("MODE wrong key rejected", ret < 0);
}

static void test_registration_domain(mock_id_t id, bool five_g)
{
    static const struct
    {
        int stat;
        linkg_cellular_registration_state_t expected;
    } cases[] =
    {
        {0, LINKG_CELLULAR_REGISTRATION_STATE_NOT_REGISTERED},
        {1, LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED},
        {2, LINKG_CELLULAR_REGISTRATION_STATE_REGISTERING},
        {3, LINKG_CELLULAR_REGISTRATION_STATE_FAILED},
        {4, LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN},
        {5, LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED}
    };
    char response[64];
    char name[80];
    linkg_cellular_registration_state_t state;
    linkg_cellular_network_mode_t mode;
    size_t index;
    int ret;

    mode = five_g ? LINKG_CELLULAR_NETWORK_MODE_5G : LINKG_CELLULAR_NETWORK_MODE_4G;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        snprintf(response, sizeof(response), "%s 0,%d", five_g ? "+C5GREG:" : "+CEREG:", cases[index].stat);
        mock_reset();
        mock_set(id, response, 0);
        ret = rg255_query_registration(fixture_channel(), mode, LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN, &state);
        snprintf(name, sizeof(name), "%s stat=%d mapping", five_g ? "C5GREG" : "CEREG", cases[index].stat);
        check_case(name, ret == 0 && state == cases[index].expected);
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
    check_case("AUTO registration merges C5GREG", ret == 0 && state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED);

    mock_set(MOCK_CEREG, "+CEREG: 0", 0);
    ret = rg255_query_registration(fixture_channel(), LINKG_CELLULAR_NETWORK_MODE_4G,
                                   LINKG_CELLULAR_NETWORK_TYPE_LTE, &state);
    check_case("REG insufficient fields rejected", ret < 0);

    mock_set(MOCK_CEREG, "+CEREG: 0,x", 0);
    ret = rg255_query_registration(fixture_channel(), LINKG_CELLULAR_NETWORK_MODE_4G,
                                   LINKG_CELLULAR_NETWORK_TYPE_LTE, &state);
    check_case("REG illegal number rejected", ret < 0);
}

static void test_serving_cell(void)
{
    const char *lte =
        "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,11,E7A14B3,140,1850,3,5,5,9C11,-99,-11,-44,10,21";
    const char *nr =
        "+QENG: \"servingcell\",\"NOCONN\",\"NR5G-SA\",\"FDD\",460,11,17BDC1402,6,174000,428910,1,20,-100,-12,5,23,21,0";
    const char *lte_with_urc =
        "+QNETDEVSTATUS: 0\r\n"
        "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,11,E7A14B3,140,1850,3,5,5,9C11,-99,-11,-44,10,21";
    rg255_serving_cell_info_t info;
    int ret;

    mock_reset();
    mock_set(MOCK_QENG, lte, 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG LTE field layout",
               ret == 0 &&
               info.network_type == LINKG_CELLULAR_NETWORK_TYPE_LTE &&
               info.band == 3U &&
               info.rsrp_valid && info.rsrp_dbm == -99 &&
               info.rsrq_valid && info.rsrq_db == -11 &&
               info.sinr_valid && info.sinr_db == 10);

    mock_set(MOCK_QENG, nr, 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG NR5G-SA field layout",
               ret == 0 &&
               info.network_type == LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA &&
               info.band == 1U &&
               info.rsrp_valid && info.rsrp_dbm == -100 &&
               info.rsrq_valid && info.rsrq_db == -12 &&
               info.sinr_valid && info.sinr_db == 5);

    mock_set(MOCK_QENG, lte_with_urc, 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG unrelated URC ignored",
               ret == 0 && info.network_type == LINKG_CELLULAR_NETWORK_TYPE_LTE && info.band == 3U);

    mock_set(MOCK_QENG, "+QENG: \"servingcell\",\"SEARCH\"", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG SEARCH is unavailable", ret < 0);

    mock_set(MOCK_QENG, "+QENG: \"servingcell\",\"NOCONN\",\"WCDMA\",1,2,3", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG unknown network type rejected", ret < 0);

    mock_set(MOCK_QENG, "+QENG: \"servingcell\",\"NOCONN\",\"LTE\"", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG insufficient fields rejected", ret < 0);

    mock_set(MOCK_QENG,
             "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,11,E7A14B3,140,1850,x,5,5,9C11,-99,-11,-44,10,21", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG illegal band rejected", ret < 0);

    mock_set(MOCK_QENG, "+WRONG: \"servingcell\",\"NOCONN\",\"LTE\"", 0);
    ret = rg255_query_serving_cell(fixture_channel(), &info);
    check_case("QENG wrong prefix rejected", ret < 0);
}

static void test_usb_nat(void)
{
    int mode;
    bool enabled;
    int ret;

    mock_reset();
    mock_set(MOCK_USBNET, "+QCFG: \"usbnet\",1", 0);
    ret = rg255_query_usbnet_mode(fixture_channel(), &mode);
    check_case("USBNET mode", ret == 0 && mode == 1);

    mock_set(MOCK_NAT, "+QCFG: \"nat\",1", 0);
    ret = rg255_query_nat_enabled(fixture_channel(), &enabled);
    check_case("NAT enabled", ret == 0 && enabled);

    mock_set(MOCK_NAT, "+QCFG: \"nat\",0", 0);
    ret = rg255_query_nat_enabled(fixture_channel(), &enabled);
    check_case("NAT disabled", ret == 0 && !enabled);

    mock_set(MOCK_NAT, "+QCFG: \"nat\",2", 0);
    ret = rg255_query_nat_enabled(fixture_channel(), &enabled);
    check_case("NAT invalid value rejected", ret < 0);

    mock_set(MOCK_USBNET, "+QCFG: \"usbnet\"", 0);
    ret = rg255_query_usbnet_mode(fixture_channel(), &mode);
    check_case("USBNET insufficient fields rejected", ret < 0);
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
    check_case("CGDCONT missing CID 1 rejected", ret < 0);

    mock_set(MOCK_CGDCONT, "+CGDCONT: 1,\"IPV4V6\"", 0);
    ret = rg255_query_pdp_config(fixture_channel(), &config);
    check_case("CGDCONT insufficient fields rejected", ret < 0);

    mock_set(MOCK_CGDCONT, "+CGDCONT: x,\"IPV4V6\",\"ctnet\"", 0);
    ret = rg255_query_pdp_config(fixture_channel(), &config);
    check_case("CGDCONT illegal CID rejected", ret < 0);
}

static void test_pdp_active(void)
{
    bool active;
    int ret;

    mock_reset();
    mock_set(MOCK_CGACT, "+CGACT: 8,0\r\n+CGACT: 1,1", 0);
    ret = rg255_query_pdp_active(fixture_channel(), &active);
    check_case("CGACT selects active CID 1", ret == 0 && active);

    mock_set(MOCK_CGACT, "+CGACT: 8,1\r\n+CGACT: 1,0", 0);
    ret = rg255_query_pdp_active(fixture_channel(), &active);
    check_case("CGACT selects inactive CID 1", ret == 0 && !active);

    mock_set(MOCK_CGACT, "+CGACT: 8,1", 0);
    ret = rg255_query_pdp_active(fixture_channel(), &active);
    check_case("CGACT missing CID 1 rejected", ret < 0);

    mock_set(MOCK_CGACT, "+CGACT: 1,2", 0);
    ret = rg255_query_pdp_active(fixture_channel(), &active);
    check_case("CGACT invalid state rejected", ret < 0);
}

static void test_pdp_address(void)
{
    rg255_pdp_address_t address;
    struct in_addr expected4;
    struct in6_addr expected6;
    char ipv4[INET_ADDRSTRLEN];
    char ipv6[INET6_ADDRSTRLEN];
    int ret;

    mock_reset();
    mock_set(MOCK_CGPADDR,
             "+CGPADDR: 1,\"10.195.207.138\",\"240e:476:8c6:6010:0:0:0:1\"", 0);
    ret = rg255_query_pdp_address(fixture_channel(), &address);
    (void)inet_pton(AF_INET, "10.195.207.138", &expected4);
    (void)inet_pton(AF_INET6, "240e:476:8c6:6010:0:0:0:1", &expected6);
    memset(ipv4, 0, sizeof(ipv4));
    memset(ipv6, 0, sizeof(ipv6));

    if (ret == 0)
    {
        (void)inet_ntop(AF_INET, &address.ipv4, ipv4, sizeof(ipv4));
        (void)inet_ntop(AF_INET6, &address.ipv6, ipv6, sizeof(ipv6));
    }

    check_case("CGPADDR valid dual stack",
               ret == 0 && address.ipv4_valid && address.ipv6_valid &&
               memcmp(&address.ipv4, &expected4, sizeof(expected4)) == 0 &&
               memcmp(&address.ipv6, &expected6, sizeof(expected6)) == 0 &&
               strcmp(ipv4, "10.195.207.138") == 0 &&
               strcmp(ipv6, "240e:476:8c6:6010::1") == 0);

    mock_set(MOCK_CGPADDR, "+CGPADDR: 1,\"0.0.0.0\",\"0:0:0:0:0:0:0:0\"", 0);
    ret = rg255_query_pdp_address(fixture_channel(), &address);
    check_case("CGPADDR zero addresses invalid", ret == 0 && !address.ipv4_valid && !address.ipv6_valid);

    mock_set(MOCK_CGPADDR, "+CGPADDR: 8,\"10.1.2.3\",\"240e::1\"", 0);
    ret = rg255_query_pdp_address(fixture_channel(), &address);
    check_case("CGPADDR wrong CID rejected", ret < 0);

    mock_set(MOCK_CGPADDR, "+CGPADDR: 1,\"not-an-ip\",\"240e::1\"", 0);
    ret = rg255_query_pdp_address(fixture_channel(), &address);
    check_case("CGPADDR malformed IP rejected", ret < 0);
}

static void test_netdev(void)
{
    bool active;
    int ret;

    mock_reset();
    mock_set(MOCK_QNETDEV, "+QNETDEVCTL: 3,1,1,1", 0);
    ret = rg255_query_netdev_active(fixture_channel(), &active);
    check_case("QNETDEV active", ret == 0 && active);

    mock_set(MOCK_QNETDEV, "+QNETDEVCTL: 3,1,1,0", 0);
    ret = rg255_query_netdev_active(fixture_channel(), &active);
    check_case("QNETDEV inactive", ret == 0 && !active);

    mock_set(MOCK_QNETDEV, "+QNETDEVCTL: 3,1,1", 0);
    ret = rg255_query_netdev_active(fixture_channel(), &active);
    check_case("QNETDEV insufficient fields rejected", ret < 0);

    mock_set(MOCK_QNETDEV, "+QNETDEVCTL: 3,1,1,x", 0);
    ret = rg255_query_netdev_active(fixture_channel(), &active);
    check_case("QNETDEV illegal state rejected", ret < 0);
}

int main(void)
{
    test_sim();
    test_network_mode();
    test_registration();
    test_serving_cell();
    test_usb_nat();
    test_pdp_config();
    test_pdp_active();
    test_pdp_address();
    test_netdev();

    printf("\nRG255 QUERY FIXTURE TEST: %d PASS / %d FAIL\n", g_pass, g_fail);

    return g_fail == 0 ? 0 : 1;
}

/**
 * @file rg255_query_network_card_fixture_test.c
 * @brief Fixture tests for QCFG netmaskset IPv4/IPv6 query parsing.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "rg255_cmd.h"
#include "rg255_query.h"

typedef struct
{
    const char *response;
    int result;
} mock_response_t;

static mock_response_t g_ipv4;
static mock_response_t g_ipv6;
static unsigned char g_dummy_channel;
static int g_pass;
static int g_fail;

static at_channel_t *fixture_channel(void)
{
    return (at_channel_t *)&g_dummy_channel;
}

static int mock_reply(const mock_response_t *mock, char *response, int response_size)
{
    size_t length;

    if (response == NULL || response_size <= 0)
    {
        return -EINVAL;
    }

    response[0] = '\0';
    if (mock->response == NULL)
    {
        return mock->result;
    }

    length = strlen(mock->response);
    if (length >= (size_t)response_size)
    {
        return -ENOSPC;
    }

    memcpy(response, mock->response, length + 1U);
    return mock->result;
}

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

#define STUB_QUERY(function_name) \
    int function_name(at_channel_t *channel, char *response, int response_size) \
    { \
        (void)channel; \
        if (response != NULL && response_size > 0) response[0] = '\0'; \
        return -ENOSYS; \
    }

STUB_QUERY(rg255_cmd_query_sim_pin_status)
STUB_QUERY(rg255_cmd_query_network_mode)
STUB_QUERY(rg255_cmd_query_eps_registration)
STUB_QUERY(rg255_cmd_query_5g_registration)
STUB_QUERY(rg255_cmd_query_serving_cell)
STUB_QUERY(rg255_cmd_query_usbnet)
STUB_QUERY(rg255_cmd_query_network_card_mode)
STUB_QUERY(rg255_cmd_query_pdp_config)
STUB_QUERY(rg255_cmd_query_pdp_state)
STUB_QUERY(rg255_cmd_query_pdp_address)
STUB_QUERY(rg255_cmd_query_netdev)

int rg255_cmd_query_network_card_ipv4(at_channel_t *channel, char *response,
                                      int response_size)
{
    (void)channel;
    return mock_reply(&g_ipv4, response, response_size);
}

int rg255_cmd_query_network_card_ipv6(at_channel_t *channel, char *response,
                                      int response_size)
{
    (void)channel;
    return mock_reply(&g_ipv6, response, response_size);
}

static void test_ipv4(void)
{
    rg255_network_card_ipv4_info_t info;
    struct in_addr expected_address;
    struct in_addr expected_netmask;
    struct in_addr expected_gateway;
    int ret;

    g_ipv4.response =
        "+QCFG: \"netmaskset\",2,\"dongle\",\"10.104.253.137\",\"255.0.0.0\",\"10.0.0.1\"";
    g_ipv4.result = 0;
    ret = rg255_query_network_card_ipv4(fixture_channel(), &info);
    (void)inet_pton(AF_INET, "10.104.253.137", &expected_address);
    (void)inet_pton(AF_INET, "255.0.0.0", &expected_netmask);
    (void)inet_pton(AF_INET, "10.0.0.1", &expected_gateway);
    check_case("NETMASKSET IPv4 fields",
               ret == 0 &&
               memcmp(&info.address, &expected_address, sizeof(expected_address)) == 0 &&
               memcmp(&info.netmask, &expected_netmask, sizeof(expected_netmask)) == 0 &&
               memcmp(&info.gateway, &expected_gateway, sizeof(expected_gateway)) == 0);

    g_ipv4.response =
        "+QCFG: \"netmaskset\",3,\"dongle\",\"10.1.2.3\",\"255.255.255.0\",\"10.1.2.1\"";
    ret = rg255_query_network_card_ipv4(fixture_channel(), &info);
    check_case("NETMASKSET IPv4 wrong opt rejected", ret == -EBADMSG);

    g_ipv4.response =
        "+QCFG: \"netmaskset\",2,\"router\",\"10.1.2.3\",\"255.255.255.0\",\"10.1.2.1\"";
    ret = rg255_query_network_card_ipv4(fixture_channel(), &info);
    check_case("NETMASKSET IPv4 wrong mode rejected", ret == -EBADMSG);

    g_ipv4.response =
        "+QCFG: \"netmaskset\",2,\"dongle\",\"0.0.0.0\",\"255.0.0.0\",\"10.0.0.1\"";
    ret = rg255_query_network_card_ipv4(fixture_channel(), &info);
    check_case("NETMASKSET IPv4 zero address unavailable", ret == -ENODATA);
}

static void test_ipv6(void)
{
    rg255_network_card_ipv6_info_t info;
    struct in6_addr expected_prefix;
    struct in6_addr expected_gateway;
    int ret;

    g_ipv6.response =
        "+QCFG: \"netmaskset\",3,\"dongle\",\"240e:476:882:3e87::1234/64\",\"fe80::1234\",\"240c::6666\",\"240c::6644\"";
    g_ipv6.result = 0;
    ret = rg255_query_network_card_ipv6(fixture_channel(), &info);
    (void)inet_pton(AF_INET6, "240e:476:882:3e87::", &expected_prefix);
    (void)inet_pton(AF_INET6, "fe80::1234", &expected_gateway);
    check_case("NETMASKSET IPv6 prefix normalized",
               ret == 0 && info.prefix_length == 64U &&
               memcmp(&info.prefix, &expected_prefix, sizeof(expected_prefix)) == 0 &&
               memcmp(&info.gateway, &expected_gateway, sizeof(expected_gateway)) == 0);

    g_ipv6.response =
        "+QCFG: \"netmaskset\",3,\"dongle\",\"240e::/129\",\"fe80::1234\"";
    ret = rg255_query_network_card_ipv6(fixture_channel(), &info);
    check_case("NETMASKSET IPv6 prefix length rejected", ret == -ERANGE);

    g_ipv6.response =
        "+QCFG: \"netmaskset\",3,\"dongle\",\"240e::/64\",\"::\"";
    ret = rg255_query_network_card_ipv6(fixture_channel(), &info);
    check_case("NETMASKSET IPv6 missing gateway rejected", ret == -ENODATA);
}

int main(void)
{
    test_ipv4();
    test_ipv6();
    printf("\nRG255 NETWORK CARD FIXTURE TEST: %d PASS / %d FAIL\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

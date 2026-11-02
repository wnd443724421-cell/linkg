/**
 * @file test_config.c
 * @brief 配置解析、边界和地址计算逻辑测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-02
 */

#include "test_assert.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "config_internal.h"
#include "linkg_cellular_config.h"
#include "linkg_device_config.h"
#include "linkg_links_config.h"
#include "linkg_network_config.h"
#include "linkg_paths_config.h"
#include "linkg_uart_config.h"
#include "linkg_wifi_config.h"

static void test_common_and_device(void)
{
    linkg_device_config_t decoded;
    linkg_device_config_t device;
    cJSON                *node;
    cJSON                *root;

    TEST_CHECK(config_string_length_valid("abc", 1U, 3U));
    TEST_CHECK(!config_string_length_valid("abcd", 1U, 3U));
    TEST_CHECK(config_int_range_valid(-1, -1, 1));
    TEST_CHECK(!config_int_range_valid(2, -1, 1));
    TEST_CHECK(config_uint32_range_valid(3U, 1U, 3U));
    TEST_CHECK(!config_uint32_range_valid(0U, 1U, 3U));
    TEST_CHECK(strcmp(config_error_string(CONFIG_ERR_VALIDATE), "配置校验失败") == 0);
    TEST_CHECK(strcmp(config_error_string(-9999), "未知配置错误") == 0);

    linkg_device_config_set_default(&device);
    TEST_CHECK_EQ(device.role, LINKG_DEVICE_ROLE_STA);
    TEST_CHECK_EQ(linkg_device_config_validate(&device), CONFIG_OK);

    root = cJSON_CreateObject();
    TEST_CHECK(root != NULL);
    TEST_CHECK_EQ(linkg_device_config_to_json(root, "device", &device), CONFIG_OK);
    node = cJSON_GetObjectItemCaseSensitive(root, "device");
    TEST_CHECK(node != NULL);
    memset(&decoded, 0, sizeof(decoded));
    TEST_CHECK_EQ(linkg_device_config_parse(node, &decoded), CONFIG_OK);
    TEST_CHECK_EQ(decoded.role, LINKG_DEVICE_ROLE_STA);
    cJSON_Delete(root);

    device.role = LINKG_DEVICE_ROLE_UNKNOWN;
    TEST_CHECK_EQ(linkg_device_config_validate(&device), CONFIG_ERR_VALIDATE);

    root = cJSON_Parse("{\"role\":123}");
    TEST_CHECK(root != NULL);
    TEST_CHECK_EQ(linkg_device_config_parse(root, &decoded), CONFIG_ERR_PARSE);
    cJSON_Delete(root);
}

static void test_network_addresses_and_rules(void)
{
    linkg_network_config_t     decoded;
    linkg_network_config_t     network;
    linkg_network_ipv4_config_t subnet;
    struct in_addr             address;
    cJSON                     *node;
    cJSON                     *root;
    char                       text[INET_ADDRSTRLEN];
    uint8_t                    node_id;

    linkg_network_config_set_default(&network);
    network.node_id = 1U;
    TEST_CHECK_EQ(inet_pton(AF_INET, "172.30.0.0", &network.virtual_network), 1);
    TEST_CHECK_EQ(inet_pton(AF_INET, "192.168.10.0", &network.ethernet_network), 1);
    TEST_CHECK_EQ(linkg_network_config_validate(&network), CONFIG_OK);

    TEST_CHECK_EQ(linkg_network_config_get_node_address(&network, 2U, &address), CONFIG_OK);
    TEST_CHECK(inet_ntop(AF_INET, &address, text, sizeof(text)) != NULL);
    TEST_CHECK(strcmp(text, "172.30.2.1") == 0);

    TEST_CHECK_EQ(linkg_network_config_get_virtual_node_id(&network, &address, &node_id), CONFIG_OK);
    TEST_CHECK_EQ(node_id, 2U);
    TEST_CHECK_EQ(linkg_network_config_get_node_virtual_subnet(&network, 254U, &subnet), CONFIG_OK);

    root = cJSON_CreateObject();
    TEST_CHECK(root != NULL);
    TEST_CHECK_EQ(linkg_network_config_to_json(root, "network", &network), CONFIG_OK);
    node = cJSON_GetObjectItemCaseSensitive(root, "network");
    TEST_CHECK(node != NULL);
    TEST_CHECK_EQ(linkg_network_config_parse(node, &decoded), CONFIG_OK);
    TEST_CHECK_EQ(decoded.node_id, network.node_id);
    TEST_CHECK_EQ(decoded.virtual_network.s_addr, network.virtual_network.s_addr);
    TEST_CHECK_EQ(decoded.ethernet_network.s_addr, network.ethernet_network.s_addr);
    cJSON_Delete(root);

    network.node_id = 0U;
    TEST_CHECK_EQ(linkg_network_config_validate(&network), CONFIG_ERR_VALIDATE);
    network.node_id = 1U;

    network.traffic.count                  = 2U;
    network.traffic.rules[0].traffic_class = LINKG_NETWORK_TRAFFIC_CLASS_REALTIME;
    network.traffic.rules[0].protocol      = LINKG_NETWORK_PORT_PROTOCOL_UDP;
    network.traffic.rules[0].start_port    = 5000U;
    network.traffic.rules[0].end_port      = 5100U;
    network.traffic.rules[1]               = network.traffic.rules[0];
    network.traffic.rules[1].start_port    = 5050U;
    network.traffic.rules[1].end_port      = 5200U;
    TEST_CHECK_EQ(linkg_network_config_validate(&network), CONFIG_ERR_VALIDATE);
}

static void test_link_path_and_radio_config(void)
{
    linkg_cellular_config_t cellular;
    linkg_links_config_t    decoded_links;
    linkg_links_config_t    links;
    linkg_paths_config_t    decoded_paths;
    linkg_paths_config_t    paths;
    linkg_wifi_config_t     wifi;
    cJSON                  *node;
    cJSON                  *root;

    linkg_paths_config_set_default(&paths);
    TEST_CHECK_EQ(linkg_paths_config_validate(&paths), CONFIG_OK);
    root = cJSON_CreateObject();
    TEST_CHECK(root != NULL);
    TEST_CHECK_EQ(linkg_paths_config_to_json(root, "paths", &paths), CONFIG_OK);
    node = cJSON_GetObjectItemCaseSensitive(root, "paths");
    TEST_CHECK_EQ(linkg_paths_config_parse(node, &decoded_paths), CONFIG_OK);
    TEST_CHECK_EQ(decoded_paths.wifi.enabled, paths.wifi.enabled);
    TEST_CHECK_EQ(decoded_paths.cellular.priority, paths.cellular.priority);
    cJSON_Delete(root);

    paths.wifi.enabled     = false;
    paths.cellular.enabled = false;
    TEST_CHECK_EQ(linkg_paths_config_validate(&paths), CONFIG_ERR_VALIDATE);
    linkg_paths_config_set_default(&paths);
    paths.cellular.enabled  = true;
    paths.cellular.priority = paths.wifi.priority;
    TEST_CHECK_EQ(linkg_paths_config_validate(&paths), CONFIG_ERR_VALIDATE);

    linkg_wifi_config_set_default(&wifi);
    TEST_CHECK_EQ(linkg_wifi_config_validate(&wifi), CONFIG_OK);
    wifi.ap.security = LINKG_WIFI_SECURITY_WPA2_PSK;
    strcpy(wifi.ap.password, "short");
    TEST_CHECK_EQ(linkg_wifi_config_validate(&wifi), CONFIG_ERR_VALIDATE);
    linkg_wifi_config_set_default(&wifi);
    wifi.wideband.work_mode                = LINKG_WIFI_WORK_MODE_WIDE;
    wifi.wideband.wide_params.protocol_mode = LINKG_WIFI_PROTOCOL_N;
    wifi.wideband.wide_params.ap_bandwidth  = LINKG_WIFI_WIDE_BANDWIDTH_80_MHZ;
    TEST_CHECK_EQ(linkg_wifi_config_validate(&wifi), CONFIG_ERR_VALIDATE);

    linkg_cellular_config_set_default(&cellular);
    TEST_CHECK_EQ(linkg_cellular_config_validate(&cellular), CONFIG_OK);
    strcpy(cellular.pin, "12");
    TEST_CHECK_EQ(linkg_cellular_config_validate(&cellular), CONFIG_ERR_VALIDATE);

    linkg_links_config_set_default(&links);
    TEST_CHECK_EQ(linkg_links_config_validate(&links), CONFIG_OK);
    root = cJSON_CreateObject();
    TEST_CHECK(root != NULL);
    TEST_CHECK_EQ(linkg_links_config_to_json(root, "links", &links), CONFIG_OK);
    node = cJSON_GetObjectItemCaseSensitive(root, "links");
    TEST_CHECK_EQ(linkg_links_config_parse(LINKG_DEVICE_ROLE_AP, node, &decoded_links), CONFIG_OK);
    TEST_CHECK_EQ(decoded_links.wifi.enabled, links.wifi.enabled);
    TEST_CHECK_EQ(decoded_links.cellular.network_mode, links.cellular.network_mode);
    cJSON_Delete(root);
}

static void test_uart_config(void)
{
    linkg_uart_config_t decoded;
    linkg_uart_config_t uart;
    cJSON              *node;
    cJSON              *root;

    linkg_uart_config_set_default(&uart);
    TEST_CHECK_EQ(linkg_uart_config_validate(&uart), CONFIG_OK);
    TEST_CHECK(linkg_uart_config_reserved_port(LINKG_UART_ROUTE_PROTO_UDP, 29600U));
    TEST_CHECK(linkg_uart_config_reserved_port(LINKG_UART_ROUTE_PROTO_TCP, 29608U));
    TEST_CHECK(!linkg_uart_config_reserved_port(LINKG_UART_ROUTE_PROTO_UDP, 5600U));

    uart.count                          = 1U;
    uart.items[0].uart_num              = 0U;
    uart.items[0].port_config.type      = LINKG_UART_PORT_TYPE_NORMAL;
    uart.items[0].port_config.baudrate  = 115200U;
    uart.items[0].port_config.data_bits = 8U;
    uart.items[0].port_config.stop_bits = 1U;
    uart.items[0].port_config.parity    = 'n';
    uart.items[0].route_count           = 1U;
    uart.items[0].routes[0].proto       = LINKG_UART_ROUTE_PROTO_UDP;
    uart.items[0].routes[0].endpoint    = LINKG_UART_ROUTE_ENDPOINT_REMOTE;
    uart.items[0].routes[0].dir         = LINKG_UART_ROUTE_DIR_BOTH;
    uart.items[0].routes[0].port        = 5600U;
    TEST_CHECK_EQ(inet_pton(AF_INET, "172.30.2.1", &uart.items[0].routes[0].ip), 1);
    TEST_CHECK_EQ(linkg_uart_config_validate(&uart), CONFIG_OK);

    root = cJSON_CreateObject();
    TEST_CHECK(root != NULL);
    TEST_CHECK_EQ(linkg_uart_config_to_json(root, "uart", &uart), CONFIG_OK);
    node = cJSON_GetObjectItemCaseSensitive(root, "uart");
    TEST_CHECK(node != NULL);
    TEST_CHECK_EQ(linkg_uart_config_parse(node, &decoded), CONFIG_OK);
    TEST_CHECK(linkg_uart_config_equal(&uart, &decoded));
    cJSON_Delete(root);

    uart.items[0].routes[0].port = 29600U;
    uart.items[0].routes[0].endpoint = LINKG_UART_ROUTE_ENDPOINT_LOCAL;
    TEST_CHECK_EQ(linkg_uart_config_validate(&uart), CONFIG_ERR_VALIDATE);

    linkg_uart_config_set_default(&uart);
    uart.count                          = 1U;
    uart.items[0].uart_num              = 0U;
    uart.items[0].port_config.type      = LINKG_UART_PORT_TYPE_SBUS;
    uart.items[0].port_config.baudrate  = 115200U;
    uart.items[0].port_config.data_bits = 8U;
    uart.items[0].port_config.stop_bits = 2U;
    uart.items[0].port_config.parity    = 'e';
    TEST_CHECK_EQ(linkg_uart_config_validate(&uart), CONFIG_ERR_VALIDATE);
}

int main(void)
{
    TEST_RUN(test_common_and_device);
    TEST_RUN(test_network_addresses_and_rules);
    TEST_RUN(test_link_path_and_radio_config);
    TEST_RUN(test_uart_config);

    return linkg_test_finish("config");
}

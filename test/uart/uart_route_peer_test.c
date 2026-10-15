/**
 * @file uart_route_peer_test.c
 * @brief UART远端节点发现门控与故障退避测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-15
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "linkg_config.h"
#include "linkg_discovery.h"
#include "linkg_log.h"
#include "uart_internal.h"

/****************************** 测试状态 ******************************/

static linkg_network_config_t g_network;
static uint64_t               g_now_ms;
static uint32_t               g_discovery_calls;
static uint32_t               g_socket_calls;
static uint32_t               g_connect_calls;
static uint32_t               g_close_calls;
static uint32_t               g_warning_logs;
static bool                   g_node_online;
static int                    g_config_error;
static int                    g_connect_error;
static char                   g_last_warning[256];

/****************************** 生产依赖替身 ******************************/

/**
 * @brief 返回测试控制的单调时钟。
 */
uint64_t uart_now_ms(void)
{
    return g_now_ms;
}

/**
 * @brief 返回固定LinkG网络配置。
 */
int linkg_config_get_network(linkg_network_config_t *out)
{
    if (out == NULL)
    {
        return -EINVAL;
    }

    if (g_config_error != 0)
    {
        return g_config_error;
    }

    *out = g_network;
    return 0;
}

/**
 * @brief 返回测试控制的节点在线状态。
 */
int linkg_discovery_node_online(uint8_t node_id, bool *online)
{
    assert(node_id == 2U);
    assert(online != NULL);

    g_discovery_calls++;
    *online = g_node_online;
    return 0;
}

/**
 * @brief 记录测试期间的告警日志数量。
 */
void linkg_log_write_internal(linkg_log_level_t level, const char *file, int line, const char *format, ...)
{
    va_list arguments;

    (void)file;
    (void)line;

    if (level >= LINKG_LOG_LEVEL_WARNING)
    {
        g_warning_logs++;
        va_start(arguments, format);
        (void)vsnprintf(g_last_warning, sizeof(g_last_warning), format, arguments);
        va_end(arguments);
    }
}

/**
 * @brief 统计Socket创建并返回虚拟描述符。
 */
int __wrap_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;

    g_socket_calls++;
    return 100 + (int)g_socket_calls;
}

/**
 * @brief 统计连接并按需注入连接错误。
 */
int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    (void)fd;
    (void)address;
    (void)length;

    g_connect_calls++;
    if (g_connect_error != 0)
    {
        errno = g_connect_error;
        return -1;
    }

    return 0;
}

/**
 * @brief 统计虚拟Socket关闭操作。
 */
int __wrap_close(int fd)
{
    (void)fd;
    g_close_calls++;
    return 0;
}

/****************************** 测试辅助 ******************************/

/**
 * @brief 重置故障注入和调用计数。
 */
static void _uart_route_test_reset(void)
{
    memset(&g_network, 0, sizeof(g_network));
    assert(inet_pton(AF_INET, "172.28.0.0", &g_network.virtual_network) == 1);
    g_network.node_id = 1U;

    g_now_ms          = 0U;
    g_discovery_calls = 0U;
    g_socket_calls    = 0U;
    g_connect_calls   = 0U;
    g_close_calls     = 0U;
    g_warning_logs    = 0U;
    g_node_online     = false;
    g_config_error    = 0;
    g_connect_error   = 0;
    g_last_warning[0] = '\0';
}

/**
 * @brief 初始化一条远端路由和所属串口。
 */
static void _uart_route_test_init(linkg_uart_route_t *route, linkg_uart_port_t *port,
                                  linkg_uart_item_config_t *port_config, linkg_uart_route_config_t *route_config,
                                  linkg_uart_route_proto_t proto, const char *address)
{
    memset(route, 0, sizeof(*route));
    memset(port, 0, sizeof(*port));
    memset(port_config, 0, sizeof(*port_config));
    memset(route_config, 0, sizeof(*route_config));

    port_config->uart_num  = 0U;
    port->config           = port_config;
    route_config->proto    = proto;
    route_config->endpoint = LINKG_UART_ROUTE_ENDPOINT_REMOTE;
    route_config->dir      = LINKG_UART_ROUTE_DIR_BOTH;
    route_config->port     = 7000U;
    assert(inet_pton(AF_INET, address, &route_config->ip) == 1);
    uart_route_init(route, port, route_config);
}

/**
 * @brief 在指定虚拟时间执行一次路由维护。
 */
static void _uart_route_test_maintain(linkg_uart_route_t *route, uint64_t now_ms)
{
    g_now_ms = now_ms;
    uart_route_maintenance(route, now_ms);
}

/****************************** 测试用例 ******************************/

/**
 * @brief 验证虚拟地址到节点编号的反向解析边界。
 */
static void _uart_route_test_virtual_address(void)
{
    struct in_addr address;
    uint8_t        node_id;

    assert(inet_pton(AF_INET, "172.28.2.1", &address) == 1);
    assert(linkg_network_config_get_virtual_node_id(&g_network, &address, &node_id) == 0);
    assert(node_id == 2U);

    assert(inet_pton(AF_INET, "172.28.2.0", &address) == 1);
    assert(linkg_network_config_get_virtual_node_id(&g_network, &address, &node_id) != 0);
    assert(inet_pton(AF_INET, "172.28.2.255", &address) == 1);
    assert(linkg_network_config_get_virtual_node_id(&g_network, &address, &node_id) != 0);
    assert(inet_pton(AF_INET, "192.168.2.1", &address) == 1);
    assert(linkg_network_config_get_virtual_node_id(&g_network, &address, &node_id) != 0);
}

/**
 * @brief 验证离线节点每秒静默检查且上线后立即建链。
 */
static void _uart_route_test_offline_wait(void)
{
    linkg_uart_item_config_t  port_config;
    linkg_uart_route_config_t route_config;
    linkg_uart_route_t        route;
    linkg_uart_port_t         port;

    _uart_route_test_init(&route, &port, &port_config, &route_config, LINKG_UART_ROUTE_PROTO_UDP, "172.28.2.1");

    _uart_route_test_maintain(&route, 0U);
    assert(route.state == LINKG_UART_ROUTE_IDLE);
    assert(route.waiting_for_peer);
    assert(route.retry_at_ms == 1000U);
    assert(route.retry_delay_ms == LINKG_UART_RETRY_INITIAL_MS);
    assert(g_discovery_calls == 1U && g_socket_calls == 0U && g_connect_calls == 0U);
    assert(atomic_load(&port.stats.errors) == 0U && g_warning_logs == 0U);

    _uart_route_test_maintain(&route, 999U);
    assert(g_discovery_calls == 1U && g_socket_calls == 0U);
    _uart_route_test_maintain(&route, 1000U);
    assert(g_discovery_calls == 2U && route.retry_at_ms == 2000U);

    g_node_online = true;
    _uart_route_test_maintain(&route, 1999U);
    assert(g_connect_calls == 0U);
    _uart_route_test_maintain(&route, 2000U);
    assert(route.state == LINKG_UART_ROUTE_READY);
    assert(!route.waiting_for_peer);
    assert(g_discovery_calls == 3U && g_socket_calls == 1U && g_connect_calls == 1U);
    assert(atomic_load(&port.stats.errors) == 0U && g_warning_logs == 0U);

    uart_route_close(&route);
}

/**
 * @brief 验证TCP远端同样在节点离线时不创建Socket。
 */
static void _uart_route_test_tcp_offline(void)
{
    linkg_uart_item_config_t  port_config;
    linkg_uart_route_config_t route_config;
    linkg_uart_route_t        route;
    linkg_uart_port_t         port;

    _uart_route_test_init(&route, &port, &port_config, &route_config, LINKG_UART_ROUTE_PROTO_TCP, "172.28.2.1");
    _uart_route_test_maintain(&route, 0U);

    assert(route.state == LINKG_UART_ROUTE_IDLE);
    assert(route.waiting_for_peer);
    assert(g_discovery_calls == 1U && g_socket_calls == 0U && g_connect_calls == 0U);
    assert(atomic_load(&port.stats.errors) == 0U && g_warning_logs == 0U);
}

/**
 * @brief 验证普通远端地址不受Discovery门控。
 */
static void _uart_route_test_external_address(void)
{
    linkg_uart_item_config_t  port_config;
    linkg_uart_route_config_t route_config;
    linkg_uart_route_t        route;
    linkg_uart_port_t         port;

    _uart_route_test_init(&route, &port, &port_config, &route_config, LINKG_UART_ROUTE_PROTO_UDP, "192.168.2.1");
    _uart_route_test_maintain(&route, 0U);

    assert(route.state == LINKG_UART_ROUTE_READY);
    assert(g_discovery_calls == 0U && g_socket_calls == 1U && g_connect_calls == 1U);
    uart_route_close(&route);
}

/**
 * @brief 验证网络配置暂时不可读取时禁止尝试连接。
 */
static void _uart_route_test_config_unavailable(void)
{
    linkg_uart_item_config_t  port_config;
    linkg_uart_route_config_t route_config;
    linkg_uart_route_t        route;
    linkg_uart_port_t         port;

    _uart_route_test_init(&route, &port, &port_config, &route_config, LINKG_UART_ROUTE_PROTO_UDP, "172.28.2.1");
    g_config_error = -ENODEV;
    _uart_route_test_maintain(&route, 0U);

    assert(route.state == LINKG_UART_ROUTE_IDLE && route.waiting_for_peer);
    assert(route.retry_at_ms == LINKG_UART_PEER_CHECK_MS);
    assert(g_discovery_calls == 0U && g_socket_calls == 0U && g_connect_calls == 0U);
    assert(atomic_load(&port.stats.errors) == 0U && g_warning_logs == 0U);

    g_config_error = 0;
    g_node_online  = true;
    _uart_route_test_maintain(&route, LINKG_UART_PEER_CHECK_MS);
    assert(route.state == LINKG_UART_ROUTE_READY);
    assert(g_discovery_calls == 1U && g_connect_calls == 1U);
    uart_route_close(&route);
}

/**
 * @brief 验证在线节点真实连接错误仍计数并使用故障退避。
 */
static void _uart_route_test_real_error(void)
{
    linkg_uart_item_config_t  port_config;
    linkg_uart_route_config_t route_config;
    linkg_uart_route_t        route;
    linkg_uart_port_t         port;

    _uart_route_test_init(&route, &port, &port_config, &route_config, LINKG_UART_ROUTE_PROTO_UDP, "172.28.2.1");
    g_node_online   = true;
    g_connect_error = ENETUNREACH;

    _uart_route_test_maintain(&route, 0U);
    assert(route.state == LINKG_UART_ROUTE_IDLE && !route.waiting_for_peer);
    assert(route.retry_at_ms == LINKG_UART_RETRY_INITIAL_MS);
    assert(route.retry_delay_ms == 1000U);
    assert(g_discovery_calls == 1U && g_connect_calls == 1U);
    assert(atomic_load(&port.stats.errors) == 1U && g_warning_logs == 1U);
    assert(strstr(g_last_warning, "retry_ms=500") != NULL);

    _uart_route_test_maintain(&route, 499U);
    assert(g_discovery_calls == 1U && g_connect_calls == 1U);
    _uart_route_test_maintain(&route, 500U);
    assert(route.retry_at_ms == 1500U && route.retry_delay_ms == 2000U);
    assert(g_discovery_calls == 2U && g_connect_calls == 2U);
    assert(atomic_load(&port.stats.errors) == 2U && g_warning_logs == 2U);
    assert(strstr(g_last_warning, "retry_ms=1000") != NULL);

    g_node_online = false;
    _uart_route_test_maintain(&route, 1500U);
    assert(route.waiting_for_peer && route.retry_at_ms == 2500U);
    assert(route.retry_delay_ms == LINKG_UART_RETRY_INITIAL_MS);
    assert(g_discovery_calls == 3U && g_connect_calls == 2U);
    assert(atomic_load(&port.stats.errors) == 2U && g_warning_logs == 2U);

    g_node_online   = true;
    g_connect_error = 0;
    _uart_route_test_maintain(&route, 2499U);
    assert(g_connect_calls == 2U);
    _uart_route_test_maintain(&route, 2500U);
    assert(route.state == LINKG_UART_ROUTE_READY && !route.waiting_for_peer);
    assert(g_discovery_calls == 4U && g_connect_calls == 3U);
    assert(atomic_load(&port.stats.errors) == 2U && g_warning_logs == 2U);

    uart_route_close(&route);
}

/**
 * @brief 执行UART远端节点门控测试。
 */
int main(void)
{
    _uart_route_test_reset();
    _uart_route_test_virtual_address();

    _uart_route_test_reset();
    _uart_route_test_offline_wait();

    _uart_route_test_reset();
    _uart_route_test_tcp_offline();

    _uart_route_test_reset();
    _uart_route_test_external_address();

    _uart_route_test_reset();
    _uart_route_test_config_unavailable();

    _uart_route_test_reset();
    _uart_route_test_real_error();

    puts("PASS UART remote peer discovery gating and retry behavior");
    return 0;
}

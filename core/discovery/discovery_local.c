/**
 * @file discovery_local.c
 * @brief LinkG设备发现本机状态实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-25
 */

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/random.h>

#include "linkg_link.h"
#include "linkg_link_manager.h"
#include "linkg_network_ops.h"
#include "linkg_node.h"
#include "linkg_system_resources.h"

#include "discovery_internal.h"

/****************************** 内部状态 ******************************/

static pthread_mutex_t g_discovery_local_refresh_lock = PTHREAD_MUTEX_INITIALIZER;

/****************************** 版本与Endpoint辅助 ******************************/

/** @brief 推进本机Report版本；调用方必须持有Discovery状态锁。 */
static int _linkg_discovery_advance_local_revision_locked(void)
{
    if (g_discovery.local_report.revision == UINT64_MAX)
    {
        return -EOVERFLOW;
    }

    g_discovery.local_report.revision++;

    return 0;
}

/** @brief 按地址族、地址及端口比较Endpoint，不比较sockaddr填充字节。 */
static bool _linkg_discovery_endpoint_equal(const linkg_path_endpoint_t *left, const linkg_path_endpoint_t *right)
{
    const struct sockaddr_in  *left_ipv4;
    const struct sockaddr_in  *right_ipv4;
    const struct sockaddr_in6 *left_ipv6;
    const struct sockaddr_in6 *right_ipv6;
    const struct sockaddr     *left_address;
    const struct sockaddr     *right_address;

    if (left == NULL || right == NULL)
    {
        return false;
    }

    if (left->length != right->length)
    {
        return false;
    }

    if (left->length == 0U)
    {
        return true;
    }

    if (left->length > sizeof(left->address))
    {
        return false;
    }

    left_address  = (const struct sockaddr *)&left->address;
    right_address = (const struct sockaddr *)&right->address;

    if (left_address->sa_family != right_address->sa_family)
    {
        return false;
    }

    if (left_address->sa_family == AF_INET)
    {
        if (left->length < sizeof(struct sockaddr_in))
        {
            return false;
        }

        left_ipv4  = (const struct sockaddr_in *)&left->address;
        right_ipv4 = (const struct sockaddr_in *)&right->address;

        return left_ipv4->sin_port == right_ipv4->sin_port &&
               left_ipv4->sin_addr.s_addr == right_ipv4->sin_addr.s_addr;
    }

    if (left_address->sa_family == AF_INET6)
    {
        if (left->length < sizeof(struct sockaddr_in6))
        {
            return false;
        }

        left_ipv6  = (const struct sockaddr_in6 *)&left->address;
        right_ipv6 = (const struct sockaddr_in6 *)&right->address;

        return left_ipv6->sin6_port == right_ipv6->sin6_port &&
               left_ipv6->sin6_scope_id == right_ipv6->sin6_scope_id &&
               memcmp(&left_ipv6->sin6_addr, &right_ipv6->sin6_addr, sizeof(left_ipv6->sin6_addr)) == 0;
    }

    return false;
}

/****************************** 会话标识 ******************************/

/** @brief 使用系统随机源生成非零Discovery Session ID。 */
static int _linkg_discovery_generate_session_id(uint64_t *session_id)
{
    uint8_t *buffer;
    size_t   offset;
    ssize_t  length;

    if (session_id == NULL)
    {
        return -EINVAL;
    }

    do
    {
        *session_id = 0U;
        buffer      = (uint8_t *)session_id;
        offset      = 0U;

        while (offset < sizeof(*session_id))
        {
            length = getrandom(buffer + offset, sizeof(*session_id) - offset, 0);
            if (length < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                return -errno;
            }

            if (length == 0)
            {
                return -EIO;
            }

            offset += (size_t)length;
        }
    }
    while (*session_id == 0U);

    return 0;
}

/****************************** 数据Endpoint采集 ******************************/

/**
 * @brief 采集本机Wi-Fi数据Endpoint；不依赖Wi-Fi Discovery Channel是否运行。
 * @return 1有效；0明确未就绪；负值表示本次状态查询失败。
 */
static int _linkg_discovery_prepare_wifi_endpoint(linkg_path_endpoint_t *endpoint)
{
    struct sockaddr_in *address;
    struct in_addr      wifi_address;
    uint32_t            link_id;
    bool                interface_up;
    int                 ret;

    if (endpoint == NULL)
    {
        return -EINVAL;
    }

    memset(endpoint, 0, sizeof(*endpoint));

    link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);
    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    ret = linkg_network_interface_is_up(LINKG_RESOURCE_INTERFACE_WIFI, &interface_up);
    if (ret == -ENODEV || ret == -ENXIO)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    if (!interface_up)
    {
        return 0;
    }

    ret = linkg_network_interface_get_ipv4(LINKG_RESOURCE_INTERFACE_WIFI, &wifi_address);
    if (ret == -ENODEV || ret == -EADDRNOTAVAIL)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    if (!linkg_network_ipv4_address_valid(&wifi_address))
    {
        return 0;
    }

    address = (struct sockaddr_in *)&endpoint->address;

    address->sin_family = AF_INET;
    address->sin_addr   = wifi_address;
    address->sin_port   = htons(LINKG_RESOURCE_UDP_PORT_WIFI_DATA);

    endpoint->length = sizeof(*address);

    return 1;
}

/**
 * @brief 采集本机Cellular数据Endpoint；独立于Wi-Fi数据Path和控制面。
 * @return 1有效；0明确未就绪；负值表示本次状态查询失败。
 */
static int _linkg_discovery_prepare_cellular_endpoint(linkg_path_endpoint_t *endpoint)
{
    struct sockaddr_in6 *address;
    struct in6_addr      cellular_address;
    uint32_t             link_id;
    bool                 interface_up;
    int                  ret;

    if (endpoint == NULL)
    {
        return -EINVAL;
    }

    memset(endpoint, 0, sizeof(*endpoint));

    link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);
    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    ret = linkg_network_interface_is_up(LINKG_RESOURCE_INTERFACE_CELLULAR, &interface_up);
    if (ret == -ENODEV || ret == -ENXIO)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    if (!interface_up)
    {
        return 0;
    }

    ret = linkg_network_interface_get_global_ipv6(LINKG_RESOURCE_INTERFACE_CELLULAR, &cellular_address);
    if (ret == -ENODEV || ret == -EADDRNOTAVAIL)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    address = (struct sockaddr_in6 *)&endpoint->address;

    address->sin6_family = AF_INET6;
    address->sin6_addr   = cellular_address;
    address->sin6_port   = htons(LINKG_RESOURCE_UDP_PORT_CELLULAR_DATA);

    endpoint->length = sizeof(*address);

    return 1;
}

/****************************** 本机Session ******************************/

/** @brief 创建本机Session初始Report；不探测网络，Endpoint在运行期动态发布。 */
int _linkg_discovery_build_local_report(linkg_discovery_report_t *report)
{
    const linkg_node_info_t *local_node;
    int ret;

    if (report == NULL)
    {
        return -EINVAL;
    }

    local_node = linkg_node_get_local();
    if (local_node == NULL)
    {
        return -ENODEV;
    }

    memset(report, 0, sizeof(*report));

    report->node = *local_node;

    ret = _linkg_discovery_generate_session_id(&report->session_id);
    if (ret != 0)
    {
        return ret;
    }

    report->revision = 1U;

    return 0;
}

/****************************** 运行期Endpoint刷新 ******************************/

/**
 * @brief 独立刷新Wi-Fi与Cellular数据Endpoint。
 * 明确未就绪时撤销对应Path；查询失败保留旧状态；每轮实际变化只推进一次revision。
 */
int _linkg_discovery_refresh_local_endpoints(void)
{
    linkg_path_endpoint_t wifi_endpoint;
    linkg_path_endpoint_t cellular_endpoint;
    bool                  wifi_changed;
    bool                  cellular_changed;
    bool                  wifi_valid;
    bool                  cellular_valid;
    int                   wifi_state;
    int                   cellular_state;
    int                   ret;

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_discovery_local_refresh_lock);

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        pthread_mutex_unlock(&g_discovery_local_refresh_lock);
        return -ESHUTDOWN;
    }

    pthread_mutex_unlock(&g_discovery.lock);

    memset(&wifi_endpoint, 0, sizeof(wifi_endpoint));
    memset(&cellular_endpoint, 0, sizeof(cellular_endpoint));

    wifi_state     = _linkg_discovery_prepare_wifi_endpoint(&wifi_endpoint);
    cellular_state = _linkg_discovery_prepare_cellular_endpoint(&cellular_endpoint);

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        pthread_mutex_unlock(&g_discovery_local_refresh_lock);
        return -ESHUTDOWN;
    }

    wifi_changed     = false;
    cellular_changed = false;

    if (wifi_state >= 0)
    {
        wifi_valid = (g_discovery.local_report.path_flags & LINKG_DISCOVERY_PATH_WIFI_VALID) != 0U;

        if (wifi_state > 0)
        {
            wifi_changed = !wifi_valid || !_linkg_discovery_endpoint_equal(&g_discovery.local_report.wifi_endpoint, &wifi_endpoint);
        }
        else
        {
            wifi_changed = wifi_valid || g_discovery.local_report.wifi_endpoint.length != 0U;
        }
    }

    if (cellular_state >= 0)
    {
        cellular_valid = (g_discovery.local_report.path_flags & LINKG_DISCOVERY_PATH_CELLULAR_VALID) != 0U;

        if (cellular_state > 0)
        {
            cellular_changed = !cellular_valid || !_linkg_discovery_endpoint_equal(&g_discovery.local_report.cellular_endpoint, &cellular_endpoint);
        }
        else
        {
            cellular_changed = cellular_valid || g_discovery.local_report.cellular_endpoint.length != 0U;
        }
    }

    if (!wifi_changed && !cellular_changed)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        pthread_mutex_unlock(&g_discovery_local_refresh_lock);
        return 0;
    }

    ret = _linkg_discovery_advance_local_revision_locked();
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        pthread_mutex_unlock(&g_discovery_local_refresh_lock);
        return ret;
    }

    if (wifi_state >= 0)
    {
        if (wifi_state > 0)
        {
            g_discovery.local_report.wifi_endpoint = wifi_endpoint;
            g_discovery.local_report.path_flags |= LINKG_DISCOVERY_PATH_WIFI_VALID;
        }
        else
        {
            memset(&g_discovery.local_report.wifi_endpoint, 0, sizeof(g_discovery.local_report.wifi_endpoint));
            g_discovery.local_report.path_flags &= (uint8_t)~LINKG_DISCOVERY_PATH_WIFI_VALID;
        }
    }

    if (cellular_state >= 0)
    {
        if (cellular_state > 0)
        {
            g_discovery.local_report.cellular_endpoint = cellular_endpoint;
            g_discovery.local_report.path_flags |= LINKG_DISCOVERY_PATH_CELLULAR_VALID;
        }
        else
        {
            memset(&g_discovery.local_report.cellular_endpoint, 0, sizeof(g_discovery.local_report.cellular_endpoint));
            g_discovery.local_report.path_flags &= (uint8_t)~LINKG_DISCOVERY_PATH_CELLULAR_VALID;
        }
    }

    pthread_mutex_unlock(&g_discovery.lock);
    pthread_mutex_unlock(&g_discovery_local_refresh_lock);

    return 0;
}

/****************************** 主动离开 ******************************/

/** @brief 构造本机LEAVE状态；调用方必须持有Discovery状态锁。 */
int _linkg_discovery_build_local_leave_locked(linkg_discovery_leave_t *leave)
{
    if (leave == NULL)
    {
        return -EINVAL;
    }

    if (!g_discovery.running)
    {
        return -ESHUTDOWN;
    }

    memset(leave, 0, sizeof(*leave));

    leave->session_id = g_discovery.local_report.session_id;
    leave->revision   = g_discovery.local_report.revision;
    leave->node_id    = g_discovery.local_report.node.node_id;

    return 0;
}

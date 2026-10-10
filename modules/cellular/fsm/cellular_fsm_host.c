/**
 * @file cellular_fsm_host.c
 * @brief LinkG蜂窝Linux Host网络配置及就绪判断实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-10
 */

#include "cellular_fsm_internal.h"

#include <errno.h>
#include <linux/if_addr.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "linkg_network_ops.h"
#include "linkg_os.h"
#include "linkg_system_resources.h"

#include "cellular_internal.h"

/****************************** Host常量 ******************************/

#define CELLULAR_FSM_HOST_INTERFACE_WAIT_MS     3000U  // Linux蜂窝接口首次出现等待时间
#define CELLULAR_FSM_HOST_EXPECTED_REFRESH_MS   1500U  // Host尚未收敛时重查模组参数的间隔
#define CELLULAR_FSM_HOST_IPV4_DHCP_RETRY_MS    5000U  // DHCP客户端异常退出后的重试间隔
#define CELLULAR_FSM_HOST_IPV4_MISMATCH_MS     15000U  // IPv4配置持续不匹配后的修复等待时间
#define CELLULAR_FSM_HOST_IPV4_DHCP_STOP_MS     1000U  // DHCP进程停止等待上限

/****************************** IPv4运行上下文 ******************************/

typedef struct
{
    pid_t    dhcp_pid;           // 当前Host会话拥有的udhcpc进程ID，-1表示不存在
    uint64_t next_dhcp_retry_ms; // DHCP启动失败后的下一次重试时间
    uint64_t mismatch_since_ms;  // Host IPv4配置开始明确不匹配的时间
} cellular_fsm_host_ipv4_t;

static cellular_fsm_host_ipv4_t g_cellular_fsm_host_ipv4 =
{
    .dhcp_pid = (pid_t)-1 // 初始没有DHCP客户端
};

/****************************** IPv4配置维护 ******************************/

/**
 * @brief 计算IPv4维护的下一次绝对时间。
 */
static uint64_t _cellular_fsm_host_next_time(uint64_t now_ms, uint64_t delay_ms)
{
    if (now_ms > UINT64_MAX - delay_ms)
    {
        return UINT64_MAX;
    }

    return now_ms + delay_ms;
}

/**
 * @brief 判断Linux Host IPv4是否与模组为当前数据会话提供的参数相符。
 */
static bool _cellular_fsm_host_ipv4_matches(const cellular_status_info_t *info)
{
    const cellular_status_expected_ipv4_info_t *expected;
    const cellular_status_host_info_t          *host;

    if (info == NULL)
    {
        return false;
    }

    expected = &info->netdev.expected_ipv4;
    host     = &info->host;

    if (!_cellular_fsm_meta_current(&expected->meta) || !expected->valid ||
        !_cellular_fsm_meta_current(&host->ipv4_meta) || !host->ipv4_valid ||
        !_cellular_fsm_meta_current(&host->ipv4_netmask_meta) || !host->ipv4_netmask_valid ||
        !_cellular_fsm_meta_current(&host->ipv4_route_meta) || !host->ipv4_gateway_valid)
    {
        return false;
    }

    return memcmp(&host->ipv4, &expected->address, sizeof(host->ipv4)) == 0 &&
           memcmp(&host->ipv4_netmask, &expected->netmask, sizeof(host->ipv4_netmask)) == 0 &&
           memcmp(&host->ipv4_gateway, &expected->gateway, sizeof(host->ipv4_gateway)) == 0;
}

/**
 * @brief 停止本模块拥有的IPv4 DHCP进程。
 */
static int _cellular_fsm_host_ipv4_stop_dhcp(void)
{
    int ret;

    if (g_cellular_fsm_host_ipv4.dhcp_pid <= 1)
    {
        return 0;
    }

    ret = linkg_os_process_stop(&g_cellular_fsm_host_ipv4.dhcp_pid, CELLULAR_FSM_HOST_IPV4_DHCP_STOP_MS);
    if (ret != 0)
    {
        CELLULAR_WARN("stop host IPv4 DHCP failed, error=%d", ret);
    }

    return ret;
}

/**
 * @brief 启动受Owner管理的前台DHCP客户端，但不等待IPv4租约。
 *
 * @note udhcpc持续运行并负责租约续期；不允许使用-b或-q使其脱离进程管理。
 */
static void _cellular_fsm_host_ipv4_start_dhcp(uint64_t now_ms)
{
    int ret;

    if (g_cellular_fsm_host_ipv4.dhcp_pid > 1 || now_ms < g_cellular_fsm_host_ipv4.next_dhcp_retry_ms)
    {
        return;
    }

    ret = linkg_os_spawn(&g_cellular_fsm_host_ipv4.dhcp_pid, "udhcpc", "-f", "-i", LINKG_RESOURCE_INTERFACE_CELLULAR, "-t", "5", NULL);
    if (ret != 0)
    {
        g_cellular_fsm_host_ipv4.next_dhcp_retry_ms = _cellular_fsm_host_next_time(now_ms, CELLULAR_FSM_HOST_IPV4_DHCP_RETRY_MS);
        CELLULAR_WARN("start host IPv4 DHCP failed, error=%d", ret);
        return;
    }

    g_cellular_fsm_host_ipv4.next_dhcp_retry_ms = 0U;
    CELLULAR_DEBUG("host IPv4 DHCP started, pid=%ld", (long)g_cellular_fsm_host_ipv4.dhcp_pid);
}

/**
 * @brief 独立维护Host IPv4 DHCP，不改变IPv6建链状态。
 *
 * @note 只能由Owner串行调用；失败时仅记录并退避，不向连接FSM返回失败。
 */
void _cellular_fsm_host_ipv4_maintain(const cellular_status_info_t *info, uint64_t now_ms)
{
    const cellular_status_host_info_t          *host;
    const cellular_status_expected_ipv4_info_t *expected;
    bool                                       running;
    bool                                       ipv4_mismatch;
    int                                        ret;

    if (g_cellular_fsm_host_ipv4.dhcp_pid > 1)
    {
        ret = linkg_os_process_running(&g_cellular_fsm_host_ipv4.dhcp_pid, &running);
        if (ret != 0)
        {
            CELLULAR_WARN("query host IPv4 DHCP process failed, error=%d", ret);
            return;
        }

        if (!running)
        {
            g_cellular_fsm_host_ipv4.next_dhcp_retry_ms = _cellular_fsm_host_next_time(now_ms, CELLULAR_FSM_HOST_IPV4_DHCP_RETRY_MS);
            g_cellular_fsm_host_ipv4.mismatch_since_ms  = 0U;
            CELLULAR_WARN("host IPv4 DHCP exited, retry_ms=%u", CELLULAR_FSM_HOST_IPV4_DHCP_RETRY_MS);
        }
    }

    ipv4_mismatch = false;
    if (info != NULL)
    {
        host     = &info->host;
        expected = &info->netdev.expected_ipv4;

        if (_cellular_fsm_meta_current(&expected->meta) && expected->valid &&
            _cellular_fsm_meta_current(&host->interface_meta) && host->interface_present &&
            _cellular_fsm_meta_current(&host->interface_up_meta) && host->interface_up &&
            _cellular_fsm_meta_current(&host->ipv4_meta) && host->ipv4_valid &&
            _cellular_fsm_meta_current(&host->ipv4_netmask_meta) &&
            _cellular_fsm_meta_current(&host->ipv4_route_meta))
        {
            ipv4_mismatch = !_cellular_fsm_host_ipv4_matches(info);
        }
    }

    if (!ipv4_mismatch)
    {
        g_cellular_fsm_host_ipv4.mismatch_since_ms = 0U;
    }
    else if (g_cellular_fsm_host_ipv4.mismatch_since_ms == 0U)
    {
        g_cellular_fsm_host_ipv4.mismatch_since_ms = now_ms != 0U ? now_ms : 1U;
    }
    else if (now_ms >= g_cellular_fsm_host_ipv4.mismatch_since_ms &&
             now_ms - g_cellular_fsm_host_ipv4.mismatch_since_ms >= CELLULAR_FSM_HOST_IPV4_MISMATCH_MS)
    {
        CELLULAR_WARN("host IPv4 config mismatch persisted, restarting DHCP only");

        ret = _cellular_fsm_host_ipv4_stop_dhcp();
        if (ret != 0)
        {
            return;
        }

        // 只清理本接口的IPv4配置，不能关闭usb0或触碰IPv6地址和路由。
        linkg_os_run_ignore("ip", "-4", "addr", "flush", "dev", LINKG_RESOURCE_INTERFACE_CELLULAR, "scope", "global", NULL);
        linkg_os_run_ignore("ip", "-4", "route", "flush", "dev", LINKG_RESOURCE_INTERFACE_CELLULAR, NULL);

        g_cellular_fsm_host_ipv4.mismatch_since_ms   = 0U;
        g_cellular_fsm_host_ipv4.next_dhcp_retry_ms = 0U;
    }

    _cellular_fsm_host_ipv4_start_dhcp(now_ms);
}

/****************************** Host资源清理 ******************************/

/**
 * @brief 清理一次完整蜂窝数据会话的Host网络资源。
 *
 * @note 只在SIM/整个数据会话停止时调用，不允许用于单独的IPv4公网探测失败。
 */
void _cellular_fsm_cleanup_host_network(void)
{
    (void)_cellular_fsm_host_ipv4_stop_dhcp();
    g_cellular_fsm_host_ipv4.next_dhcp_retry_ms = 0U;
    g_cellular_fsm_host_ipv4.mismatch_since_ms  = 0U;

    if (!linkg_network_interface_exists(LINKG_RESOURCE_INTERFACE_CELLULAR))
    {
        return;
    }

    (void)linkg_network_interface_set_up(LINKG_RESOURCE_INTERFACE_CELLULAR, false);

    linkg_os_run_ignore("ip", "-4", "addr", "flush", "dev", LINKG_RESOURCE_INTERFACE_CELLULAR, "scope", "global", NULL);
    linkg_os_run_ignore("ip", "-6", "addr", "flush", "dev", LINKG_RESOURCE_INTERFACE_CELLULAR, "scope", "global", NULL);
    linkg_os_run_ignore("ip", "-4", "route", "flush", "dev", LINKG_RESOURCE_INTERFACE_CELLULAR, NULL);
    linkg_os_run_ignore("ip", "-6", "route", "flush", "dev", LINKG_RESOURCE_INTERFACE_CELLULAR, NULL);
}

/****************************** IPv6状态校验 ******************************/

/**
 * @brief 将/proc/net/if_inet6地址字段中的单个十六进制字符转换为数值。
 */
static int _cellular_fsm_host_hex_digit(char digit)
{
    if (digit >= '0' && digit <= '9')
    {
        return digit - '0';
    }

    if (digit >= 'a' && digit <= 'f')
    {
        return digit - 'a' + 10;
    }

    if (digit >= 'A' && digit <= 'F')
    {
        return digit - 'A' + 10;
    }

    return -EINVAL;
}

/**
 * @brief 确认指定Host IPv6地址已完成DAD并且没有失败。
 *
 * @note Status当前通过getifaddrs取得地址，无法提供内核IFA_F_TENTATIVE标志。
 */
static bool _cellular_fsm_host_ipv6_dad_ready(const struct in6_addr *address)
{
    char                line[160];
    char                hex_address[33];
    char                interface_name[IFNAMSIZ];
    struct in6_addr     current_address;
    unsigned int        interface_index;
    unsigned int        prefix_length;
    unsigned int        scope;
    unsigned int        flags;
    FILE               *file;
    size_t              index;
    int                 high;
    int                 low;
    bool                ready;

    if (address == NULL)
    {
        return false;
    }

    file = fopen("/proc/net/if_inet6", "r");
    if (file == NULL)
    {
        return false;
    }

    ready = false;
    while (fgets(line, sizeof(line), file) != NULL)
    {
        if (sscanf(line, "%32s %x %x %x %x %15s", hex_address, &interface_index, &prefix_length, &scope, &flags, interface_name) != 6)
        {
            continue;
        }

        if (strcmp(interface_name, LINKG_RESOURCE_INTERFACE_CELLULAR) != 0 || strlen(hex_address) != 32U)
        {
            continue;
        }

        for (index = 0U; index < sizeof(current_address.s6_addr); index++)
        {
            high = _cellular_fsm_host_hex_digit(hex_address[index * 2U]);
            low  = _cellular_fsm_host_hex_digit(hex_address[index * 2U + 1U]);
            if (high < 0 || low < 0)
            {
                break;
            }

            current_address.s6_addr[index] = (uint8_t)((high << 4U) | low);
        }

        if (index != sizeof(current_address.s6_addr) || memcmp(&current_address, address, sizeof(*address)) != 0)
        {
            continue;
        }

        ready = (flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED | IFA_F_OPTIMISTIC)) == 0U;
        break;
    }

    (void)fclose(file);
    return ready;
}

/**
 * @brief 根据Linux实际IPv6地址、路由和DAD判断蜂窝Host是否就绪。
 *
 * @note RG255期望IPv6前缀/网关可能查询失败或与RA结果不一致，不作为成功硬门槛。
 */
bool _cellular_fsm_host_ready(const cellular_status_info_t *info)
{
    const cellular_status_host_info_t *host;

    if (info == NULL)
    {
        return false;
    }

    host = &info->host;

    if (!_cellular_fsm_meta_current(&host->interface_meta) || !host->interface_present ||
        !_cellular_fsm_meta_current(&host->interface_up_meta) || !host->interface_up)
    {
        return false;
    }

    if (!_cellular_fsm_meta_current(&host->ipv6_meta) || !host->global_ipv6_valid ||
        !_cellular_fsm_meta_current(&host->ipv6_route_meta) || !host->ipv6_gateway_valid)
    {
        return false;
    }

    // Linux实际IPv6地址、默认路由及DAD是Host就绪依据；模组期望前缀仅用于辅助诊断。
    return _cellular_fsm_host_ipv6_dad_ready(&host->global_ipv6);
}

/****************************** Host状态步骤 ******************************/

/**
 * @brief 初始化Host IPv6配置并启动非阻塞IPv4 DHCP维护。
 */
cellular_fsm_step_t _cellular_fsm_state_prepare_host(cellular_fsm_t *fsm, uint64_t now_ms)
{
    int ret;

    ret = cellular_runtime_note_attempt(&fsm->runtime, now_ms);
    if (ret != 0)
    {
        return _cellular_fsm_step_fatal(ret);
    }

    ret = linkg_network_interface_wait(LINKG_RESOURCE_INTERFACE_CELLULAR, CELLULAR_FSM_HOST_INTERFACE_WAIT_MS);
    if (ret != 0)
    {
        return _cellular_fsm_step_failed(ret);
    }

    ret = linkg_network_interface_ipv6_accept_ra_set(LINKG_RESOURCE_INTERFACE_CELLULAR, LINKG_NETWORK_IPV6_ACCEPT_RA_FORCE);
    if (ret != 0)
    {
        return _cellular_fsm_step_failed(ret);
    }

    ret = linkg_network_interface_set_up(LINKG_RESOURCE_INTERFACE_CELLULAR, true);
    if (ret != 0)
    {
        return _cellular_fsm_step_failed(ret);
    }

    _cellular_fsm_host_ipv4_maintain(NULL, now_ms);

    _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_HOST | CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK);

    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_HOST);
}

/**
 * @brief 等待Host IPv6收敛，IPv4 DHCP在后台独立运行。
 */
cellular_fsm_step_t _cellular_fsm_state_wait_host(cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms)
{
    int ret;

    _cellular_fsm_host_ipv4_maintain(info, now_ms);

    if (_cellular_fsm_host_ready(info))
    {
        CELLULAR_DEBUG("host IPv6 network configuration confirmed");
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_ONLINE);
    }

    if (cellular_runtime_state_timed_out(&fsm->runtime, now_ms))
    {
        CELLULAR_WARN("host IPv6 convergence timed out, interface=%d, up=%d, address=%d, dad=%d, route=%d, modem_expected=%d, expected_error=%d, address_error=%d, route_error=%d",
                      info != NULL && info->host.interface_present ? 1 : 0,
                      info != NULL && info->host.interface_up ? 1 : 0,
                      info != NULL && info->host.global_ipv6_valid ? 1 : 0,
                      info != NULL && info->host.global_ipv6_valid && _cellular_fsm_host_ipv6_dad_ready(&info->host.global_ipv6) ? 1 : 0,
                      info != NULL && info->host.ipv6_gateway_valid ? 1 : 0,
                      info != NULL && info->netdev.expected_ipv6.valid ? 1 : 0,
                      info != NULL ? info->netdev.expected_ipv6.meta.last_error : -EINVAL,
                      info != NULL ? info->host.ipv6_meta.last_error : -EINVAL,
                      info != NULL ? info->host.ipv6_route_meta.last_error : -EINVAL);
        return _cellular_fsm_step_failed(-ETIMEDOUT);
    }

    if (fsm->runtime.next_action_ms == 0U || cellular_runtime_action_due(&fsm->runtime, now_ms))
    {
        _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_PDP_ADDRESS | CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK | CELLULAR_STATUS_REFRESH_HOST);

        ret = cellular_runtime_schedule_action(&fsm->runtime, now_ms, CELLULAR_FSM_HOST_EXPECTED_REFRESH_MS);
        if (ret != 0)
        {
            return _cellular_fsm_step_fatal(ret);
        }
    }

    return _cellular_fsm_step_wait();
}

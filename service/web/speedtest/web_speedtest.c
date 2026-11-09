/**
 * @file web_speedtest.c
 * @brief LinkG Web网络吞吐量诊断任务及iperf3服务端管理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-09
 */

#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "web_speedtest.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "linkg_cellular.h"
#include "linkg_log.h"
#include "linkg_thread.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_WEB_SPEEDTEST_THREAD_NAME        "web-speedtest"   // 线程名
#define LINKG_WEB_SPEEDTEST_POLL_MS            200               // 子进程轮询周期，单位毫秒
#define LINKG_WEB_SPEEDTEST_CONNECT_EXTRA_MS   20000ULL          // 测试连接及收尾额外超时
#define LINKG_WEB_SPEEDTEST_KILL_GRACE_MS      1500ULL           // 终止后等待强制退出的时间
#define LINKG_WEB_SPEEDTEST_LINE_SIZE          512U              // 单行输出最大缓冲长度

#define LINKG_WEB_SPEEDTEST_PUBLIC_SERVER      "116.169.118.91"  // 默认公共节点IP
#define LINKG_WEB_SPEEDTEST_PUBLIC_PORT_FIRST  5201U             // 公共端口范围起始
#define LINKG_WEB_SPEEDTEST_PUBLIC_PORT_LAST   5220U             // 公共端口范围终点
#define LINKG_WEB_SPEEDTEST_RETRY_DELAY_MS     300               // 端口轮换间隔毫秒

/****************************** 内部类型 ******************************/

typedef struct
{
    pthread_mutex_t                 lock;              // 保护任务状态和参数
    linkg_thread_t                  worker;            // 常驻测速Worker线程
    bool                            initialized;       // 线程资源是否完成初始化
    bool                            started;           // Web测速模块是否已启动
    bool                            stopping;          // Web测速模块正在停止
    bool                            pending;           // 是否存在尚未执行的任务
    bool                            server_busy;       // 当前公网端口是否明确报告繁忙
    bool                            last_interval_sum; // 当前秒是否已采用SUM汇总
    uint64_t                        started_ms;        // 当前测速开始时刻
    uint32_t                        next_job_id;       // 下一个任务编号
    linkg_web_speedtest_params_t    pending_params;    // 即将执行的参数副本
    linkg_web_speedtest_status_t    status;            // 最近任务状态快照
} linkg_web_speedtest_context_t;

/****************************** 全局上下文 ******************************/

static linkg_web_speedtest_context_t g_web_speedtest =
{
    .lock = PTHREAD_MUTEX_INITIALIZER // 状态互斥锁
};

/****************************** 参数检查 ******************************/

/**
 * @brief 检查服务器是否为合法IPv4、IPv6或ASCII域名。
 */
static bool _linkg_web_speedtest_server_valid(const char *server)
{
    struct in_addr  ipv4;
    struct in6_addr ipv6;
    size_t          length;
    size_t          index;
    size_t          label_length;
    unsigned char   ch;

    if (server == NULL)
    {
        return false;
    }

    length = strnlen(server, sizeof(g_web_speedtest.pending_params.server));
    if (length == 0U || length >= sizeof(g_web_speedtest.pending_params.server))
    {
        return false;
    }

    if (inet_pton(AF_INET, server, &ipv4) == 1 ||
        inet_pton(AF_INET6, server, &ipv6) == 1)
    {
        return true;
    }

    label_length = 0U;
    for (index = 0U; index < length; index++)
    {
        ch = (unsigned char)server[index];

        if (ch == '.')
        {
            if (label_length == 0U || label_length > 63U || server[index - 1U] == '-')
            {
                return false;
            }
            label_length = 0U;
            continue;
        }

        if (!isalnum(ch) && ch != '-')
        {
            return false;
        }

        if (label_length == 0U && ch == '-')
        {
            return false;
        }
        label_length++;
    }

    return label_length > 0U && label_length <= 63U && server[length - 1U] != '-';
}

/**
 * @brief 校验服务端和客户端两种任务参数。
 */
static bool _linkg_web_speedtest_params_valid(const linkg_web_speedtest_params_t *params)
{
    if (params == NULL || params->port == 0U ||
        (params->mode != LINKG_WEB_SPEEDTEST_MODE_CLIENT &&
         params->mode != LINKG_WEB_SPEEDTEST_MODE_SERVER))
    {
        return false;
    }

    if (params->mode == LINKG_WEB_SPEEDTEST_MODE_SERVER)
    {
        return !params->auto_port;
    }

    if (!_linkg_web_speedtest_server_valid(params->server) ||
        params->duration_sec < 5U || params->duration_sec > 60U ||
        (params->protocol != LINKG_WEB_SPEEDTEST_TCP && params->protocol != LINKG_WEB_SPEEDTEST_UDP) ||
        (params->direction != LINKG_WEB_SPEEDTEST_UPLOAD && params->direction != LINKG_WEB_SPEEDTEST_DOWNLOAD))
    {
        return false;
    }

    // 只允许已知公共IP由5201逐步尝试至5220，自定义服务器端口保持固定。
    if (params->auto_port &&
        (strcmp(params->server, LINKG_WEB_SPEEDTEST_PUBLIC_SERVER) != 0 ||
         params->port != LINKG_WEB_SPEEDTEST_PUBLIC_PORT_FIRST))
    {
        return false;
    }

    if (params->protocol == LINKG_WEB_SPEEDTEST_UDP &&
        (!isfinite(params->bandwidth_mbps) || params->bandwidth_mbps < 0.1 ||
         params->bandwidth_mbps > 200.0 || params->packet_length < 200U ||
         params->packet_length > 1472U))
    {
        return false;
    }

    return true;
}

/****************************** 状态辅助 ******************************/

/**
 * @brief 判断测速任务是否占用唯一Worker。
 */
static bool _linkg_web_speedtest_state_active(linkg_web_speedtest_state_t state)
{
    return state == LINKG_WEB_SPEEDTEST_STARTING || state == LINKG_WEB_SPEEDTEST_RUNNING ||
           state == LINKG_WEB_SPEEDTEST_LISTENING;
}

/**
 * @brief 更新最近一次任务提示信息。
 */
static void _linkg_web_speedtest_set_message(const char *message)
{
    pthread_mutex_lock(&g_web_speedtest.lock);
    snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message), "%s", message);
    pthread_mutex_unlock(&g_web_speedtest.lock);
}

/**
 * @brief 判断任务是否被取消或模块正在停止。
 */
static bool _linkg_web_speedtest_cancelled(const linkg_thread_t *thread)
{
    bool cancel;

    pthread_mutex_lock(&g_web_speedtest.lock);
    cancel = g_web_speedtest.status.cancel_requested;
    pthread_mutex_unlock(&g_web_speedtest.lock);

    return cancel || !linkg_thread_is_running(thread);
}

/****************************** 输出解析 ******************************/

/**
 * @brief 将1秒统计和最终收发汇总转换为线程安全的任务快照。
 *
 * 服务器模式允许连续多个测试会话，收到Accepted时清空上一轮曲线。
 * 客户端实时为本机区间速率，最终优先采用接收端汇总。
 */
static void _linkg_web_speedtest_parse_line(const char *line)
{
    const char *close_bracket;
    const char *percent;
    const char *open_paren;
    double      start_sec;
    double      end_sec;
    double      transfer;
    double      bitrate;
    double      mbps;
    double      loss;
    char        transfer_unit[32];
    char        rate_unit[32];
    bool        receiver;
    bool        sender;
    bool        is_sum;
    bool        server_mode;
    int         fields;

    if (strncmp(line, "iperf3: error", 13U) == 0)
    {
        pthread_mutex_lock(&g_web_speedtest.lock);
        if (strstr(line, "server is busy running a test") != NULL)
        {
            g_web_speedtest.server_busy = true;
        }
        snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message), "%s", line);
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return;
    }

    if (strstr(line, "Server listening on") != NULL)
    {
        pthread_mutex_lock(&g_web_speedtest.lock);
        if (g_web_speedtest.status.params.mode == LINKG_WEB_SPEEDTEST_MODE_SERVER)
        {
            g_web_speedtest.status.state            = LINKG_WEB_SPEEDTEST_LISTENING;
            g_web_speedtest.status.server_connected = false;
            snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message),
                     "%s", "服务器已监听，等待组网内客户端连接");
        }
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return;
    }

    if (strstr(line, "Accepted connection from") != NULL)
    {
        pthread_mutex_lock(&g_web_speedtest.lock);
        if (g_web_speedtest.status.params.mode == LINKG_WEB_SPEEDTEST_MODE_SERVER)
        {
            g_web_speedtest.status.session_id++;
            g_web_speedtest.status.state               = LINKG_WEB_SPEEDTEST_RUNNING;
            g_web_speedtest.status.server_connected    = true;
            g_web_speedtest.status.current_mbps        = 0.0;
            g_web_speedtest.status.average_mbps        = 0.0;
            g_web_speedtest.status.packet_loss_percent = 0.0;
            g_web_speedtest.status.average_valid       = false;
            g_web_speedtest.status.packet_loss_valid   = false;
            g_web_speedtest.status.sample_count        = 0U;
            g_web_speedtest.status.interval_count      = 0U;
            g_web_speedtest.last_interval_sum          = false;
            snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message),
                     "%s", "客户端已连接，正在接收测试数据");
        }
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return;
    }

    if (line[0] != '[' || strstr(line, "(omitted)") != NULL)
    {
        return;
    }

    close_bracket = strchr(line, ']');
    if (close_bracket == NULL)
    {
        return;
    }

    is_sum = strncmp(line, "[SUM]", 5U) == 0;
    fields = sscanf(close_bracket + 1, " %lf-%lf sec %lf %31s %lf %31s",
                    &start_sec, &end_sec, &transfer, transfer_unit, &bitrate, rate_unit);
    if (fields != 6 || !isfinite(bitrate) || bitrate < 0.0 ||
        !isfinite(start_sec) || !isfinite(end_sec) || end_sec <= start_sec)
    {
        return;
    }

    if (strcmp(rate_unit, "Mbits/sec") == 0)
    {
        mbps = bitrate;
    }
    else if (strcmp(rate_unit, "Kbits/sec") == 0)
    {
        mbps = bitrate / 1000.0;
    }
    else if (strcmp(rate_unit, "Gbits/sec") == 0)
    {
        mbps = bitrate * 1000.0;
    }
    else if (strcmp(rate_unit, "bits/sec") == 0)
    {
        mbps = bitrate / 1000000.0;
    }
    else
    {
        return;
    }

    sender   = strstr(line, " sender") != NULL;
    receiver = strstr(line, " receiver") != NULL;

    pthread_mutex_lock(&g_web_speedtest.lock);
    server_mode = g_web_speedtest.status.params.mode == LINKG_WEB_SPEEDTEST_MODE_SERVER;

    if (receiver || sender)
    {
        // 汇总以接收端优先；服务端的反向测试可能只有本机sender汇总。
        if (receiver || (server_mode && !g_web_speedtest.status.average_valid))
        {
            g_web_speedtest.status.average_mbps      = mbps;
            g_web_speedtest.status.average_valid     = true;
            g_web_speedtest.status.average_is_sender = sender;
        }

        if (receiver)
        {
            percent    = strchr(line, '%');
            open_paren = NULL;
            if (percent != NULL)
            {
                const char *cursor;
                for (cursor = percent; cursor > line; cursor--)
                {
                    if (*cursor == '(')
                    {
                        open_paren = cursor;
                        break;
                    }
                }
            }
            if (open_paren != NULL && sscanf(open_paren + 1, "%lf", &loss) == 1 &&
                isfinite(loss) && loss >= 0.0 && loss <= 100.0)
            {
                g_web_speedtest.status.packet_loss_percent = loss;
                g_web_speedtest.status.packet_loss_valid   = true;
            }
        }
        if (server_mode && receiver)
        {
            g_web_speedtest.status.server_connected = false;
            g_web_speedtest.status.state            = LINKG_WEB_SPEEDTEST_LISTENING;
            snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message),
                     "%s", "本轮测速完成，服务器继续等待连接");
        }
    }
    else if (end_sec - start_sec <= 2.0)
    {
        uint32_t index;
        bool     same_interval;

        same_interval = g_web_speedtest.status.sample_count > 0U &&
                        end_sec <= g_web_speedtest.status.samples[g_web_speedtest.status.sample_count - 1U].second + 0.2;
        if (same_interval)
        {
            // 多流场景[SUM]覆盖单流，普通流不再覆盖已收到的汇总。
            if (is_sum)
            {
                index = g_web_speedtest.status.sample_count - 1U;
                g_web_speedtest.status.samples[index].mbps = mbps;
                g_web_speedtest.status.current_mbps        = mbps;
                g_web_speedtest.last_interval_sum          = true;
            }
        }
        else
        {
            if (g_web_speedtest.status.sample_count >= LINKG_WEB_SPEEDTEST_SAMPLE_MAX)
            {
                memmove(&g_web_speedtest.status.samples[0], &g_web_speedtest.status.samples[1],
                        (LINKG_WEB_SPEEDTEST_SAMPLE_MAX - 1U) * sizeof(g_web_speedtest.status.samples[0]));
                index = LINKG_WEB_SPEEDTEST_SAMPLE_MAX - 1U;
            }
            else
            {
                index = g_web_speedtest.status.sample_count++;
            }
            g_web_speedtest.status.samples[index].second = end_sec;
            g_web_speedtest.status.samples[index].mbps   = mbps;
            g_web_speedtest.status.current_mbps          = mbps;
            g_web_speedtest.status.interval_count++;
            g_web_speedtest.last_interval_sum            = is_sum;
        }
    }

    pthread_mutex_unlock(&g_web_speedtest.lock);
}

/**
 * @brief 将输出流按换行拆分，容忍拆包、CRLF和过长日志行。
 */
static void _linkg_web_speedtest_consume_bytes(const char *bytes, size_t count, char *line, size_t *used)
{
    size_t index;

    for (index = 0U; index < count; index++)
    {
        if (bytes[index] == '\n')
        {
            line[*used] = '\0';
            _linkg_web_speedtest_parse_line(line);
            *used = 0U;
            continue;
        }

        if (bytes[index] == '\r')
        {
            continue;
        }

        if (*used < LINKG_WEB_SPEEDTEST_LINE_SIZE - 1U)
        {
            line[(*used)++] = bytes[index];
        }
        else
        {
            // 单行超限时丢弃该行，避免错误解析截断的统计。
            *used = 0U;
        }
    }
}

/****************************** 子进程执行 ******************************/

/**
 * @brief 判断目标是否为LinkG虚拟聚合网段172.28.0.0/16。
 */
static bool _linkg_web_speedtest_overlay_target(const char *server)
{
    struct in_addr address;
    uint32_t       ip;

    if (inet_pton(AF_INET, server, &address) != 1)
    {
        return false;
    }
    ip = ntohl(address.s_addr);
    return (ip & 0xFFFF0000U) == 0xAC1C0000U;
}

/**
 * @brief 获取本机172.28网段的地址，用于iperf3服务端监听。
 *
 * 优先匹配主机地址为1的虚拟子网网关，避免要求用户填写本机地址。
 */
static int _linkg_web_speedtest_local_overlay(char *address, size_t size)
{
    struct ifaddrs *list;
    struct ifaddrs *current;
    struct in_addr selected;
    uint32_t       value;
    bool           found;
    int            ret;

    if (getifaddrs(&list) != 0)
    {
        return -errno;
    }

    memset(&selected, 0, sizeof(selected));
    found = false;
    ret   = -EADDRNOTAVAIL;
    for (current = list; current != NULL; current = current->ifa_next)
    {
        const struct sockaddr_in *sockaddr_ipv4;

        if (current->ifa_addr == NULL || current->ifa_addr->sa_family != AF_INET ||
            (current->ifa_flags & IFF_UP) == 0U)
        {
            continue;
        }
        sockaddr_ipv4 = (const struct sockaddr_in *)current->ifa_addr;
        value = ntohl(sockaddr_ipv4->sin_addr.s_addr);
        if ((value & 0xFFFF0000U) != 0xAC1C0000U)
        {
            continue;
        }
        selected = sockaddr_ipv4->sin_addr;
        found = true;
        if ((value & 0xFFU) == 1U)
        {
            break;
        }
    }

    if (found && inet_ntop(AF_INET, &selected, address, (socklen_t)size) != NULL)
    {
        ret = 0;
    }
    freeifaddrs(list);
    return ret;
}

/**
 * @brief 获取公网客户端蜂窝源地址；组网目标由现有路由自行选择源地址。
 */
static int _linkg_web_speedtest_bind_address(const linkg_web_speedtest_params_t *params,
                                             char *address, size_t address_size, const char **family)
{
    linkg_cellular_status_snapshot_t snapshot;
    struct in_addr                   ipv4;
    struct in6_addr                  ipv6;
    int                              ret;

    address[0] = '\0';
    *family    = "-4";

    // 172.28目标应走LinkG虚拟路由，禁止强制绑定RG255A物理蜂窝IP。
    if (_linkg_web_speedtest_overlay_target(params->server))
    {
        return 0;
    }

    ret = linkg_cellular_get_status(&snapshot);
    if (ret != 0 || !snapshot.data.pdp_active)
    {
        return -ENETDOWN;
    }

    if (inet_pton(AF_INET, params->server, &ipv4) == 1)
    {
        if (!snapshot.data.ipv4_valid ||
            inet_ntop(AF_INET, &snapshot.data.ipv4, address, (socklen_t)address_size) == NULL)
        {
            return -EADDRNOTAVAIL;
        }
        return 0;
    }

    if (inet_pton(AF_INET6, params->server, &ipv6) == 1)
    {
        if (!snapshot.data.global_ipv6_valid ||
            inet_ntop(AF_INET6, &snapshot.data.global_ipv6, address, (socklen_t)address_size) == NULL)
        {
            return -EADDRNOTAVAIL;
        }
        *family = "-6";
        return 0;
    }

    if (snapshot.data.ipv4_valid &&
        inet_ntop(AF_INET, &snapshot.data.ipv4, address, (socklen_t)address_size) != NULL)
    {
        return 0;
    }

    if (snapshot.data.global_ipv6_valid &&
        inet_ntop(AF_INET6, &snapshot.data.global_ipv6, address, (socklen_t)address_size) != NULL)
    {
        *family = "-6";
        return 0;
    }

    return -EADDRNOTAVAIL;
}

/**
 * @brief 执行一次客户端测速或持续运行组网iperf3服务端。
 *
 * 由常驻Worker持有fork子进程；取消、模块关闭均确保waitpid完成回收。
 */
static int _linkg_web_speedtest_run(const linkg_web_speedtest_params_t *params, const linkg_thread_t *thread)
{
    char           bind_address[INET6_ADDRSTRLEN];
    char           port[8];
    char           duration[8];
    char           bandwidth[32];
    char           packet_length[8];
    char           line[LINKG_WEB_SPEEDTEST_LINE_SIZE];
    char           chunk[512];
    char          *argv[24];
    const char    *family;
    struct pollfd  descriptor;
    uint64_t       start_ms;
    uint64_t       terminate_ms;
    uint64_t       reaped_ms;
    pid_t          pid;
    pid_t          waited;
    size_t         used;
    uint32_t       burst_level;
    uint32_t       burst_count;
    int            pipe_fd[2];
    int            arg_count;
    int            status;
    int            ret;
    int            count;
    bool           reaped;
    bool           eof;
    bool           stopping;
    bool           timed_out;
    bool           sent_kill;
    bool           server_busy;
    bool           server_mode;

    if (_linkg_web_speedtest_cancelled(thread))
    {
        return -ECANCELED;
    }

    server_mode = params->mode == LINKG_WEB_SPEEDTEST_MODE_SERVER;
    if (server_mode)
    {
        ret    = _linkg_web_speedtest_local_overlay(bind_address, sizeof(bind_address));
        family = "-4";
        if (ret != 0)
        {
            _linkg_web_speedtest_set_message("本机没有可用的172.28.x.x地址，无法启动组网服务器");
            return ret;
        }
    }
    else
    {
        ret = _linkg_web_speedtest_bind_address(params, bind_address, sizeof(bind_address), &family);
        if (ret != 0)
        {
            _linkg_web_speedtest_set_message("蜂窝网络不可用或缺少匹配地址；172.28组网目标无需蜂窝绑定");
            return ret;
        }
    }

    pthread_mutex_lock(&g_web_speedtest.lock);
    snprintf(g_web_speedtest.status.bind_address, sizeof(g_web_speedtest.status.bind_address),
             "%s", bind_address);
    pthread_mutex_unlock(&g_web_speedtest.lock);

    snprintf(port, sizeof(port), "%u", (unsigned int)params->port);
    snprintf(duration, sizeof(duration), "%u", (unsigned int)params->duration_sec);
    snprintf(packet_length, sizeof(packet_length), "%u", (unsigned int)params->packet_length);
    /****************************** UDP突发发送分档 ******************************/

    // 50Mbps以内保持普通发送；超过50Mbps后，每26Mbps提高一个突发档位。
    // 最大32档，超过180Mbps也不再增加突发长度。
    burst_count = 0U;

    if (!server_mode && params->protocol == LINKG_WEB_SPEEDTEST_UDP &&
        params->bandwidth_mbps > 50.0)
    {
        burst_level = 1U;

        while (burst_level < 5U &&
               params->bandwidth_mbps > 50.0 + 26.0 * (double)burst_level)
        {
            burst_level++;
        }

        burst_count = 1U << burst_level;
    }

    if (burst_count != 0U)
    {
        snprintf(bandwidth, sizeof(bandwidth), "%.3fM/%u",
                 params->bandwidth_mbps, (unsigned int)burst_count);
    }
    else
    {
        snprintf(bandwidth, sizeof(bandwidth), "%.3fM", params->bandwidth_mbps);
    }

    arg_count         = 0;
    argv[arg_count++] = "iperf3";
    argv[arg_count++] = server_mode ? "-s" : "-c";
    if (!server_mode)
    {
        argv[arg_count++] = (char *)params->server;
    }
    argv[arg_count++] = "-p";
    argv[arg_count++] = port;
    if (!server_mode)
    {
        argv[arg_count++] = "-t";
        argv[arg_count++] = duration;
    }
    argv[arg_count++] = "-i";
    argv[arg_count++] = "1";
    argv[arg_count++] = "-f";
    argv[arg_count++] = "m";
    argv[arg_count++] = "--forceflush";
    argv[arg_count++] = (char *)family;
    if (bind_address[0] != '\0')
    {
        argv[arg_count++] = "-B";
        argv[arg_count++] = bind_address;
    }
    if (!server_mode && params->direction == LINKG_WEB_SPEEDTEST_DOWNLOAD)
    {
        argv[arg_count++] = "-R";
    }
    if (!server_mode && params->protocol == LINKG_WEB_SPEEDTEST_UDP)
    {
        argv[arg_count++] = "-u";
        argv[arg_count++] = "-b";
        argv[arg_count++] = bandwidth;
        argv[arg_count++] = "-l";
        argv[arg_count++] = packet_length;
    }
    argv[arg_count] = NULL;

    if (_linkg_web_speedtest_cancelled(thread))
    {
        return -ECANCELED;
    }
    if (pipe(pipe_fd) != 0)
    {
        return -errno;
    }
    pid = fork();
    if (pid < 0)
    {
        ret = -errno;
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return ret;
    }
    if (pid == 0)
    {
        long fd;
        // fork后只调用异步信号安全函数，再exec替换进程。
        close(pipe_fd[0]);
        if (dup2(pipe_fd[1], STDOUT_FILENO) < 0 ||
            dup2(pipe_fd[1], STDERR_FILENO) < 0)
        {
            _exit(127);
        }
        if (pipe_fd[1] > STDERR_FILENO)
        {
            close(pipe_fd[1]);
        }
        for (fd = 3; fd < 1024; fd++)
        {
            close((int)fd);
        }
        execvp("iperf3", argv);
        _exit(127);
    }

    close(pipe_fd[1]);
    descriptor.fd     = pipe_fd[0];
    descriptor.events = POLLIN | POLLHUP;
    used              = 0U;
    status            = 0;
    reaped            = false;
    eof               = false;
    stopping          = false;
    timed_out         = false;
    sent_kill         = false;
    terminate_ms      = 0U;
    reaped_ms         = 0U;
    start_ms          = linkg_time_elapsed_ms();

    while (!reaped || !eof)
    {
        uint64_t now_ms;

        now_ms = linkg_time_elapsed_ms();
        if (!stopping && (_linkg_web_speedtest_cancelled(thread) ||
            (!server_mode && now_ms - start_ms >
             (uint64_t)params->duration_sec * 1000ULL + LINKG_WEB_SPEEDTEST_CONNECT_EXTRA_MS)))
        {
            timed_out    = !_linkg_web_speedtest_cancelled(thread);
            stopping     = true;
            terminate_ms = now_ms;
            kill(pid, SIGTERM);
        }
        if (stopping && !reaped && !sent_kill &&
            now_ms - terminate_ms >= LINKG_WEB_SPEEDTEST_KILL_GRACE_MS)
        {
            kill(pid, SIGKILL);
            sent_kill = true;
        }
        if (!eof)
        {
            descriptor.revents = 0;
            ret = poll(&descriptor, 1, LINKG_WEB_SPEEDTEST_POLL_MS);
            if (ret > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            {
                count = (int)read(pipe_fd[0], chunk, sizeof(chunk));
                if (count > 0)
                {
                    _linkg_web_speedtest_consume_bytes(chunk, (size_t)count, line, &used);
                }
                else if (count == 0)
                {
                    eof = true;
                }
                else if (errno != EINTR)
                {
                    eof = true;
                }
            }
        }
        if (eof && !reaped)
        {
            poll(NULL, 0, LINKG_WEB_SPEEDTEST_POLL_MS);
        }
        if (!reaped)
        {
            waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno == ECHILD))
            {
                reaped    = true;
                reaped_ms = linkg_time_elapsed_ms();
            }
        }
        else if (!eof && linkg_time_elapsed_ms() - reaped_ms > 2000ULL)
        {
            eof = true;
        }
    }
    close(pipe_fd[0]);
    if (used != 0U)
    {
        line[used] = '\0';
        _linkg_web_speedtest_parse_line(line);
    }
    if (stopping)
    {
        if (timed_out)
        {
            _linkg_web_speedtest_set_message("测速执行超时，已终止iperf3；请检查路由及远端收尾");
            return -ETIMEDOUT;
        }
        return -ECANCELED;
    }

    pthread_mutex_lock(&g_web_speedtest.lock);
    server_busy = g_web_speedtest.server_busy;
    pthread_mutex_unlock(&g_web_speedtest.lock);
    if (server_busy)
    {
        return -EAGAIN;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        pthread_mutex_lock(&g_web_speedtest.lock);
        if (g_web_speedtest.status.message[0] == '\0')
        {
            snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message),
                     "%s", "iperf3进程退出异常，检查服务器端口和客户端配置");
        }
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return -EIO;
    }
    if (server_mode)
    {
        _linkg_web_speedtest_set_message("iperf3服务端已退出");
        return -EPIPE;
    }
    pthread_mutex_lock(&g_web_speedtest.lock);
    ret = g_web_speedtest.status.average_valid ? 0 : -ENODATA;
    pthread_mutex_unlock(&g_web_speedtest.lock);
    if (ret != 0)
    {
        _linkg_web_speedtest_set_message("iperf3未返回完整的接收端最终统计");
    }
    return ret;
}

/**
 * @brief 公共节点仅在收到明确繁忙错误后自动尝试下一个端口。
 */
static int _linkg_web_speedtest_run_with_retry(const linkg_web_speedtest_params_t *params, const linkg_thread_t *thread)
{
    linkg_web_speedtest_params_t attempt;
    uint16_t                      previous_port;
    int                           ret;

    attempt = *params;
    for (;;)
    {
        if (_linkg_web_speedtest_cancelled(thread))
        {
            return -ECANCELED;
        }
        pthread_mutex_lock(&g_web_speedtest.lock);
        g_web_speedtest.server_busy                = false;
        g_web_speedtest.last_interval_sum          = false;
        g_web_speedtest.status.params.port         = attempt.port;
        g_web_speedtest.status.current_mbps        = 0.0;
        g_web_speedtest.status.average_mbps        = 0.0;
        g_web_speedtest.status.packet_loss_percent = 0.0;
        g_web_speedtest.status.sample_count        = 0U;
        g_web_speedtest.status.interval_count      = 0U;
        g_web_speedtest.status.average_valid       = false;
        g_web_speedtest.status.packet_loss_valid   = false;
        g_web_speedtest.started_ms                 = linkg_time_elapsed_ms();
        if (params->auto_port)
        {
            snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message),
                     "正在尝试公共服务器端口 %u（5201–5220）", (unsigned int)attempt.port);
        }
        else
        {
            g_web_speedtest.status.message[0] = '\0';
        }
        pthread_mutex_unlock(&g_web_speedtest.lock);

        ret = _linkg_web_speedtest_run(&attempt, thread);
        if (ret != -EAGAIN || !params->auto_port)
        {
            return ret;
        }
        if (attempt.port >= LINKG_WEB_SPEEDTEST_PUBLIC_PORT_LAST)
        {
            _linkg_web_speedtest_set_message("公共服务器5201～5220全部繁忙，请稍后再试");
            return -EBUSY;
        }
        previous_port = attempt.port;
        attempt.port++;
        pthread_mutex_lock(&g_web_speedtest.lock);
        g_web_speedtest.status.params.port = attempt.port;
        snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message),
                 "端口 %u 繁忙，正在自动尝试 %u", (unsigned int)previous_port, (unsigned int)attempt.port);
        pthread_mutex_unlock(&g_web_speedtest.lock);
        poll(NULL, 0, LINKG_WEB_SPEEDTEST_RETRY_DELAY_MS);
    }
}

/****************************** Worker线程 ******************************/

static void _linkg_web_speedtest_worker(linkg_thread_t *thread, void *user_data)
{
    linkg_web_speedtest_params_t params;
    struct pollfd                 descriptor;
    uint64_t                      start_ms;
    int                           ret;
    bool                          pending;

    (void)user_data;

    descriptor.fd     = linkg_thread_get_wakeup_fd(thread);
    descriptor.events = POLLIN;

    while (linkg_thread_is_running(thread))
    {
        pthread_mutex_lock(&g_web_speedtest.lock);
        pending = g_web_speedtest.pending;
        if (pending)
        {
            params = g_web_speedtest.pending_params;
            g_web_speedtest.pending       = false;
            g_web_speedtest.started_ms    = linkg_time_elapsed_ms();
            g_web_speedtest.status.state = LINKG_WEB_SPEEDTEST_RUNNING;
        }
        pthread_mutex_unlock(&g_web_speedtest.lock);

        if (!pending)
        {
            ret = poll(&descriptor, 1, 500);
            if (ret > 0 && (descriptor.revents & POLLIN) != 0)
            {
                linkg_thread_clear_wakeup(thread);
            }
            continue;
        }

        start_ms = linkg_time_elapsed_ms();
        ret      = _linkg_web_speedtest_run_with_retry(&params, thread);

        pthread_mutex_lock(&g_web_speedtest.lock);
        g_web_speedtest.status.elapsed_ms = linkg_time_elapsed_ms() - start_ms;

        if (ret == 0)
        {
            g_web_speedtest.status.state = LINKG_WEB_SPEEDTEST_COMPLETED;
            snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message), "%s", "测速完成");
        }
        else if (ret == -ECANCELED)
        {
            g_web_speedtest.status.state = LINKG_WEB_SPEEDTEST_CANCELLED;
            snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message), "%s", "测速已取消");
        }
        else
        {
            g_web_speedtest.status.state = LINKG_WEB_SPEEDTEST_FAILED;
            if (g_web_speedtest.status.message[0] == '\0')
            {
                snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message),
                         "测速失败，错误码=%d", ret);
            }
        }
        pthread_mutex_unlock(&g_web_speedtest.lock);

        LINKG_LOG_INFO("WEB-SPEEDTEST: job finished, error=%d", ret);
    }
}

/****************************** 生命周期 ******************************/

int _linkg_web_speedtest_init(void)
{
    int ret;

    pthread_mutex_lock(&g_web_speedtest.lock);
    if (g_web_speedtest.initialized)
    {
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return 0;
    }
    pthread_mutex_unlock(&g_web_speedtest.lock);

    ret = linkg_thread_init(&g_web_speedtest.worker, LINKG_WEB_SPEEDTEST_THREAD_NAME,
                            _linkg_web_speedtest_worker, NULL);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_web_speedtest.lock);
    g_web_speedtest.initialized = true;
    g_web_speedtest.status.state = LINKG_WEB_SPEEDTEST_IDLE;
    pthread_mutex_unlock(&g_web_speedtest.lock);

    return 0;
}

/**
 * @brief 启动Web测速Worker线程。
 */
int _linkg_web_speedtest_start(void)
{
    int ret;

    pthread_mutex_lock(&g_web_speedtest.lock);
    if (!g_web_speedtest.initialized || g_web_speedtest.stopping)
    {
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return -EINVAL;
    }
    pthread_mutex_unlock(&g_web_speedtest.lock);

    ret = linkg_thread_start(&g_web_speedtest.worker);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_web_speedtest.lock);
    g_web_speedtest.started = true;
    pthread_mutex_unlock(&g_web_speedtest.lock);
    return 0;
}

/**
 * @brief 取消测速任务并等待Worker线程退出。
 */
int _linkg_web_speedtest_stop(void)
{
    int ret;

    pthread_mutex_lock(&g_web_speedtest.lock);
    if (!g_web_speedtest.started)
    {
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return 0;
    }
    g_web_speedtest.stopping = true;
    g_web_speedtest.status.cancel_requested = true;
    pthread_mutex_unlock(&g_web_speedtest.lock);

    ret = linkg_thread_stop(&g_web_speedtest.worker);

    pthread_mutex_lock(&g_web_speedtest.lock);
    if (ret == 0)
    {
        g_web_speedtest.started  = false;
        g_web_speedtest.stopping = false;
        g_web_speedtest.pending  = false;
        if (_linkg_web_speedtest_state_active(g_web_speedtest.status.state))
        {
            g_web_speedtest.status.state = LINKG_WEB_SPEEDTEST_CANCELLED;
        }
    }
    pthread_mutex_unlock(&g_web_speedtest.lock);
    return ret;
}

/**
 * @brief 释放Web测速Worker线程资源。
 */
void _linkg_web_speedtest_deinit(void)
{
    if (_linkg_web_speedtest_stop() != 0)
    {
        return;
    }

    pthread_mutex_lock(&g_web_speedtest.lock);
    if (!g_web_speedtest.initialized)
    {
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return;
    }
    g_web_speedtest.initialized = false;
    pthread_mutex_unlock(&g_web_speedtest.lock);
    linkg_thread_deinit(&g_web_speedtest.worker);
}

/****************************** 任务控制 ******************************/

int _linkg_web_speedtest_submit(const linkg_web_speedtest_params_t *params, uint32_t *job_id)
{
    int ret;

    if (!_linkg_web_speedtest_params_valid(params) || job_id == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_web_speedtest.lock);
    if (!g_web_speedtest.started || g_web_speedtest.stopping)
    {
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return -ESHUTDOWN;
    }
    if (_linkg_web_speedtest_state_active(g_web_speedtest.status.state))
    {
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return -EBUSY;
    }

    g_web_speedtest.next_job_id++;
    if (g_web_speedtest.next_job_id == 0U)
    {
        g_web_speedtest.next_job_id = 1U;
    }
    memset(&g_web_speedtest.status, 0, sizeof(g_web_speedtest.status));
    g_web_speedtest.status.job_id = g_web_speedtest.next_job_id;
    g_web_speedtest.status.state  = LINKG_WEB_SPEEDTEST_STARTING;
    g_web_speedtest.status.params         = *params;
    g_web_speedtest.status.rate_is_sender = params->mode == LINKG_WEB_SPEEDTEST_MODE_CLIENT &&
                                              params->direction == LINKG_WEB_SPEEDTEST_UPLOAD;
    g_web_speedtest.pending_params = *params;
    g_web_speedtest.pending        = true;
    *job_id                        = g_web_speedtest.next_job_id;
    pthread_mutex_unlock(&g_web_speedtest.lock);

    ret = linkg_thread_wakeup(&g_web_speedtest.worker);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_web_speedtest.lock);
        g_web_speedtest.pending      = false;
        g_web_speedtest.status.state = LINKG_WEB_SPEEDTEST_FAILED;
        snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message),
                 "%s", "唤醒测速任务失败");
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return ret;
    }

    LINKG_LOG_INFO("WEB-SPEEDTEST: job=%u server=%s:%u protocol=%s direction=%s duration=%u",
                   (unsigned int)*job_id, params->server, (unsigned int)params->port,
                   params->protocol == LINKG_WEB_SPEEDTEST_UDP ? "udp" : "tcp",
                   params->direction == LINKG_WEB_SPEEDTEST_DOWNLOAD ? "download" : "upload",
                   (unsigned int)params->duration_sec);
    return 0;
}

/**
 * @brief 获取指定任务或最近一次测速状态快照。
 */
int _linkg_web_speedtest_get(uint32_t job_id, linkg_web_speedtest_status_t *status)
{
    if (status == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_web_speedtest.lock);
    if (job_id != 0U && job_id != g_web_speedtest.status.job_id)
    {
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return -ENOENT;
    }

    *status = g_web_speedtest.status;
    if (_linkg_web_speedtest_state_active(status->state) &&
        g_web_speedtest.started_ms != 0U)
    {
        status->elapsed_ms = linkg_time_elapsed_ms() - g_web_speedtest.started_ms;
    }
    pthread_mutex_unlock(&g_web_speedtest.lock);
    return 0;
}

/**
 * @brief 请求取消指定测速任务。
 */
int _linkg_web_speedtest_cancel(uint32_t job_id)
{
    pthread_mutex_lock(&g_web_speedtest.lock);
    if (job_id == 0U || job_id != g_web_speedtest.status.job_id)
    {
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return -ENOENT;
    }
    if (!_linkg_web_speedtest_state_active(g_web_speedtest.status.state))
    {
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return -EALREADY;
    }
    g_web_speedtest.status.cancel_requested = true;
    pthread_mutex_unlock(&g_web_speedtest.lock);
    return 0;
}

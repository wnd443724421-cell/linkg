/**
 * @file web_speedtest.c
 * @brief LinkG Web 5G网络测速任务执行与iperf3输出解析
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-09
 */

#define _POSIX_C_SOURCE 200809L

#include "web_speedtest.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
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

#define LINKG_WEB_SPEEDTEST_THREAD_NAME      "web-speedtest" // 线程名
#define LINKG_WEB_SPEEDTEST_POLL_MS          200            // 子进程轮询周期，单位毫秒
#define LINKG_WEB_SPEEDTEST_CONNECT_EXTRA_MS 20000ULL       // 测试连接及收尾额外超时
#define LINKG_WEB_SPEEDTEST_KILL_GRACE_MS    1500ULL        // 终止后等待强制退出的时间
#define LINKG_WEB_SPEEDTEST_LINE_SIZE        512U           // 单行输出最大缓冲长度

/****************************** 内部类型 ******************************/

typedef struct
{
    pthread_mutex_t                 lock;           // 保护任务状态和参数
    linkg_thread_t                  worker;         // 常驻测速Worker线程
    bool                            initialized;    // 线程资源是否完成初始化
    bool                            started;        // Web测速模块是否已启动
    bool                            stopping;       // Web测速模块正在停止
    bool                            pending;        // 是否存在尚未执行的任务
    uint64_t                        started_ms;     // 当前测速开始时刻
    uint32_t                        next_job_id;    // 下一个任务编号
    linkg_web_speedtest_params_t    pending_params; // 即将执行的参数副本
    linkg_web_speedtest_status_t    status;         // 最近任务状态快照
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
 * @brief 校验测速任务参数，不接受隐式默认值。
 */
static bool _linkg_web_speedtest_params_valid(const linkg_web_speedtest_params_t *params)
{
    if (params == NULL || !_linkg_web_speedtest_server_valid(params->server) ||
        params->port == 0U || params->duration_sec < 5U || params->duration_sec > 60U ||
        (params->protocol != LINKG_WEB_SPEEDTEST_TCP && params->protocol != LINKG_WEB_SPEEDTEST_UDP) ||
        (params->direction != LINKG_WEB_SPEEDTEST_UPLOAD && params->direction != LINKG_WEB_SPEEDTEST_DOWNLOAD))
    {
        return false;
    }

    if (params->protocol == LINKG_WEB_SPEEDTEST_UDP &&
        (!isfinite(params->bandwidth_mbps) || params->bandwidth_mbps < 0.1 ||
         params->bandwidth_mbps > 200.0 || params->packet_length < 200U ||
         params->packet_length > 1400U))
    {
        return false;
    }

    return true;
}

/****************************** 状态辅助 ******************************/

static bool _linkg_web_speedtest_state_active(linkg_web_speedtest_state_t state)
{
    return state == LINKG_WEB_SPEEDTEST_STARTING || state == LINKG_WEB_SPEEDTEST_RUNNING;
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
 * @brief 解析iperf3普通文本模式中的区间吞吐量与最终Receiver统计。
 *
 * 单流模式每秒只有一个区间行；sender/receiver汇总行不作为实时样本。
 * UDP上行的区间吞吐量是本地发送速率，最终以远端receiver为准。
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
    int         fields;

    if (strncmp(line, "iperf3: error", 13U) == 0)
    {
        _linkg_web_speedtest_set_message(line);
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
    else if (strcmp(rate_unit, "bits/sec") == 0)
    {
        mbps = bitrate / 1000000.0;
    }
    else if (strcmp(rate_unit, "Gbits/sec") == 0)
    {
        mbps = bitrate * 1000.0;
    }
    else
    {
        return;
    }

    receiver = strstr(line, " receiver") != NULL;
    sender   = strstr(line, " sender") != NULL;

    pthread_mutex_lock(&g_web_speedtest.lock);

    if (receiver)
    {
        // 最终结果必须取接收端平均吞吐量，不采用UDP发送端的目标速率。
        g_web_speedtest.status.average_mbps  = mbps;
        g_web_speedtest.status.average_valid = true;

        if (g_web_speedtest.status.params.protocol == LINKG_WEB_SPEEDTEST_UDP)
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
    }
    else if (!sender && end_sec - start_sec <= 2.0)
    {
        g_web_speedtest.status.current_mbps = mbps;
        g_web_speedtest.status.interval_count++;
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
 * @brief 使用蜂窝状态中的本机地址绑定iperf3，避免使用默认Wi-Fi源地址。
 *
 * IPv6字面量强制IPv6；域名默认使用IPv4，必要时改用IPv6。
 */
static int _linkg_web_speedtest_bind_address(const linkg_web_speedtest_params_t *params,
                                              char *address, size_t address_size, const char **family)
{
    linkg_cellular_status_snapshot_t snapshot;
    struct in_addr                   ipv4;
    struct in6_addr                  ipv6;
    int                              ret;

    ret = linkg_cellular_get_status(&snapshot);
    if (ret != 0 || !snapshot.data.pdp_active)
    {
        return -ENETDOWN;
    }

    // IPv4字面量必须使用蜂窝IPv4，不能错误回退到IPv6地址族。
    if (inet_pton(AF_INET, params->server, &ipv4) == 1)
    {
        if (!snapshot.data.ipv4_valid ||
            inet_ntop(AF_INET, &snapshot.data.ipv4, address, (socklen_t)address_size) == NULL)
        {
            return -EADDRNOTAVAIL;
        }
        *family = "-4";
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
        *family = "-4";
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
 * @brief 建立子进程并收集其文本流，响应取消及执行超时。
 */
static int _linkg_web_speedtest_run(const linkg_web_speedtest_params_t *params, const linkg_thread_t *thread)
{
    char        bind_address[INET6_ADDRSTRLEN];
    char        port[8];
    char        duration[8];
    char        bandwidth[32];
    char        packet_length[8];
    char        line[LINKG_WEB_SPEEDTEST_LINE_SIZE];
    char        chunk[512];
    char       *argv[24];
    const char *family;
    struct pollfd descriptor;
    uint64_t    start_ms;
    uint64_t    terminate_ms;
    pid_t       pid;
    pid_t       waited;
    size_t      used;
    int         pipe_fd[2];
    int         arg_count;
    int         status;
    int         ret;
    int         count;
    bool        reaped;
    bool        eof;
    bool        stopping;
    bool        timed_out;
    bool        sent_kill;
    uint64_t    reaped_ms;

    if (_linkg_web_speedtest_cancelled(thread))
    {
        return -ECANCELED;
    }

    ret = _linkg_web_speedtest_bind_address(params, bind_address, sizeof(bind_address), &family);
    if (ret != 0)
    {
        _linkg_web_speedtest_set_message("5G数据链路或蜂窝IP地址不可用");
        return ret;
    }

    snprintf(port, sizeof(port), "%u", (unsigned int)params->port);
    snprintf(duration, sizeof(duration), "%u", (unsigned int)params->duration_sec);
    snprintf(packet_length, sizeof(packet_length), "%u", (unsigned int)params->packet_length);
    snprintf(bandwidth, sizeof(bandwidth), "%.3fM", params->bandwidth_mbps);

    arg_count         = 0;
    argv[arg_count++] = "iperf3";
    argv[arg_count++] = "-c";
    argv[arg_count++] = (char *)params->server;
    argv[arg_count++] = "-p";
    argv[arg_count++] = port;
    argv[arg_count++] = "-t";
    argv[arg_count++] = duration;
    argv[arg_count++] = "-i";
    argv[arg_count++] = "1";
    argv[arg_count++] = "-f";
    argv[arg_count++] = "m";
    argv[arg_count++] = "--forceflush";
    argv[arg_count++] = (char *)family;
    argv[arg_count++] = "-B";
    argv[arg_count++] = bind_address;

    if (params->direction == LINKG_WEB_SPEEDTEST_DOWNLOAD)
    {
        argv[arg_count++] = "-R";
    }

    if (params->protocol == LINKG_WEB_SPEEDTEST_UDP)
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
        long max_fd;
        long fd;

        // 多线程进程fork后只执行异步信号安全操作，随后直接exec。
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

        max_fd = 1024;
        for (fd = 3; fd < max_fd; fd++)
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
            now_ms - start_ms > (uint64_t)params->duration_sec * 1000ULL + LINKG_WEB_SPEEDTEST_CONNECT_EXTRA_MS))
        {
            timed_out     = !_linkg_web_speedtest_cancelled(thread);
            stopping      = true;
            terminate_ms  = now_ms;
            kill(pid, SIGTERM);
        }

        if (stopping && !reaped && !sent_kill && now_ms - terminate_ms >= LINKG_WEB_SPEEDTEST_KILL_GRACE_MS)
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
            // 防止其他继承管道的进程长期持有写端造成Worker无法退出。
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
            _linkg_web_speedtest_set_message("测速执行超时，已停止iperf3");
            return -ETIMEDOUT;
        }
        return -ECANCELED;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        pthread_mutex_lock(&g_web_speedtest.lock);
        if (g_web_speedtest.status.message[0] == '\0')
        {
            snprintf(g_web_speedtest.status.message, sizeof(g_web_speedtest.status.message),
                     "%s", "iperf3运行失败，检查服务器、端口及程序是否安装");
        }
        pthread_mutex_unlock(&g_web_speedtest.lock);
        return -EIO;
    }

    pthread_mutex_lock(&g_web_speedtest.lock);
    ret = g_web_speedtest.status.average_valid ? 0 : -ENODATA;
    pthread_mutex_unlock(&g_web_speedtest.lock);

    if (ret != 0)
    {
        _linkg_web_speedtest_set_message("iperf3未返回有效的接收端汇总结果");
    }
    return ret;
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
        ret      = _linkg_web_speedtest_run(&params, thread);

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
    g_web_speedtest.status.params = *params;
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
    if (status->state == LINKG_WEB_SPEEDTEST_RUNNING && g_web_speedtest.started_ms != 0U)
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

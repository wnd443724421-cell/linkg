/**
 * @file cellular_keepalive.c
 * @brief 蜂窝低流量保活间隔测试工具
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-03
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/****************************** 程序常量 ******************************/

#define KEEPALIVE_DEFAULT_INTERVAL_MS 100U
#define KEEPALIVE_MAX_INTERVAL_MS     3600000U
#define KEEPALIVE_DEFAULT_TARGET      "180.76.76.76"
#define KEEPALIVE_DEFAULT_PORT        53U
#define KEEPALIVE_DEFAULT_INTERFACE   "usb0"
#define KEEPALIVE_REPORT_INTERVAL_MS  10000ULL
#define KEEPALIVE_INPUT_SIZE          64U
#define KEEPALIVE_DRAIN_SIZE          512U

/****************************** 全局状态 ******************************/

static volatile sig_atomic_t g_keepalive_stop = 0;

/****************************** 信号处理 ******************************/

/**
 * @brief 请求停止保活程序。
 */
static void _keepalive_handle_signal(int signal_number)
{
    (void)signal_number;
    g_keepalive_stop = 1;
}

/****************************** 时间辅助 ******************************/

/**
 * @brief 获取单调时钟毫秒值。
 */
static uint64_t _keepalive_now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }

    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

/****************************** 参数解析 ******************************/

/**
 * @brief 解析指定范围内的无符号整数。
 */
static int _keepalive_parse_u32(const char *text, uint32_t minimum, uint32_t maximum,
                                uint32_t *value)
{
    unsigned long parsed;
    char         *end;

    if (text == NULL || value == NULL || text[0] == '\0')
    {
        return -EINVAL;
    }

    errno  = 0;
    end    = NULL;
    parsed = strtoul(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > maximum)
    {
        return -EINVAL;
    }

    *value = (uint32_t)parsed;

    return 0;
}

/**
 * @brief 输出程序使用方法。
 */
static void _keepalive_print_usage(const char *program)
{
    printf("用法: %s [间隔毫秒] [目标IPv4] [目标端口] [出口接口]\n", program);
    printf("默认: %u ms, %s:%u, interface=%s\n",
           KEEPALIVE_DEFAULT_INTERVAL_MS,
           KEEPALIVE_DEFAULT_TARGET,
           KEEPALIVE_DEFAULT_PORT,
           KEEPALIVE_DEFAULT_INTERFACE);
}

/****************************** 网络辅助 ******************************/

/**
 * @brief 创建并绑定指定出口接口的UDP套接字。
 */
static int _keepalive_open_socket(const char *interface_name,
                                  const struct sockaddr_in *target)
{
    int socket_fd;
    int ret;

    socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (socket_fd < 0)
    {
        return -errno;
    }

    ret = setsockopt(socket_fd,
                     SOL_SOCKET,
                     SO_BINDTODEVICE,
                     interface_name,
                     (socklen_t)(strlen(interface_name) + 1U));
    if (ret != 0)
    {
        ret = -errno;
        close(socket_fd);
        return ret;
    }

    ret = connect(socket_fd, (const struct sockaddr *)target, sizeof(*target));
    if (ret != 0)
    {
        ret = -errno;
        close(socket_fd);
        return ret;
    }

    return socket_fd;
}

/**
 * @brief 发送一个www.baidu.com的IPv4 DNS查询包。
 */
static int _keepalive_send_packet(int socket_fd, uint16_t sequence)
{
    uint8_t packet[] =
    {
        0x00U, 0x00U,             // Transaction ID
        0x01U, 0x00U,             // Standard query, recursion desired
        0x00U, 0x01U,             // QDCOUNT = 1
        0x00U, 0x00U,             // ANCOUNT = 0
        0x00U, 0x00U,             // NSCOUNT = 0
        0x00U, 0x00U,             // ARCOUNT = 0
        0x03U, 'w', 'w', 'w',
        0x05U, 'b', 'a', 'i', 'd', 'u',
        0x03U, 'c', 'o', 'm',
        0x00U,
        0x00U, 0x01U,             // QTYPE = A
        0x00U, 0x01U              // QCLASS = IN
    };
    ssize_t sent;

    packet[0] = (uint8_t)(sequence >> 8U);
    packet[1] = (uint8_t)(sequence & 0xFFU);

    sent = send(socket_fd, packet, sizeof(packet), MSG_NOSIGNAL);
    if (sent < 0)
    {
        return -errno;
    }

    if ((size_t)sent != sizeof(packet))
    {
        return -EIO;
    }

    return 0;
}

/**
 * @brief 清空已经到达的DNS响应，避免长期测试时接收缓冲堆满。
 */
static void _keepalive_drain_socket(int socket_fd)
{
    uint8_t buffer[KEEPALIVE_DRAIN_SIZE];

    while (recv(socket_fd, buffer, sizeof(buffer), MSG_DONTWAIT) > 0)
    {
    }
}

/****************************** 交互控制 ******************************/

/**
 * @brief 处理一行交互输入。
 */
static int _keepalive_handle_input(char *input, uint32_t *interval_ms, bool *paused)
{
    uint32_t value;
    size_t   length;
    int      ret;

    if (input == NULL || interval_ms == NULL || paused == NULL)
    {
        return -EINVAL;
    }

    length = strlen(input);
    while (length > 0U && (input[length - 1U] == '\n' || input[length - 1U] == '\r'))
    {
        input[length - 1U] = '\0';
        length--;
    }

    if (strcmp(input, "q") == 0 || strcmp(input, "quit") == 0)
    {
        return 1;
    }

    if (strcmp(input, "0") == 0)
    {
        *paused = true;
        printf("发送已暂停；输入正整数恢复，输入q退出。\n");
        return 0;
    }

    ret = _keepalive_parse_u32(input, 1U, KEEPALIVE_MAX_INTERVAL_MS, &value);
    if (ret != 0)
    {
        printf("无效输入：请输入1～%u毫秒，0暂停，q退出。\n", KEEPALIVE_MAX_INTERVAL_MS);
        return 0;
    }

    *interval_ms = value;
    *paused      = false;
    printf("发送间隔已更新为 %u ms。\n", *interval_ms);

    return 0;
}

/****************************** 主循环 ******************************/

/**
 * @brief 运行交互式周期发送循环。
 */
static int _keepalive_run(int socket_fd, uint32_t initial_interval_ms)
{
    struct pollfd descriptors[2];
    uint64_t      next_report_ms;
    uint64_t      next_send_ms;
    uint64_t      sent_count;
    uint64_t      error_count;
    uint64_t      now_ms;
    uint32_t      interval_ms;
    uint16_t      sequence;
    char          input[KEEPALIVE_INPUT_SIZE];
    bool          paused;
    int           timeout_ms;
    int           ret;

    memset(descriptors, 0, sizeof(descriptors));
    descriptors[0].fd     = STDIN_FILENO;
    descriptors[0].events = POLLIN;
    descriptors[1].fd     = socket_fd;
    descriptors[1].events = POLLIN;

    interval_ms   = initial_interval_ms;
    sent_count    = 0U;
    error_count   = 0U;
    sequence      = 1U;
    paused        = false;
    now_ms        = _keepalive_now_ms();
    next_send_ms  = now_ms;
    next_report_ms = now_ms + KEEPALIVE_REPORT_INTERVAL_MS;

    while (!g_keepalive_stop)
    {
        now_ms = _keepalive_now_ms();

        if (!paused && now_ms >= next_send_ms)
        {
            ret = _keepalive_send_packet(socket_fd, sequence++);
            if (ret == 0)
            {
                sent_count++;
            }
            else
            {
                error_count++;
            }

            next_send_ms = now_ms + interval_ms;
        }

        if (now_ms >= next_report_ms)
        {
            printf("sent=%llu, errors=%llu, interval=%u ms%s\n",
                   (unsigned long long)sent_count,
                   (unsigned long long)error_count,
                   interval_ms,
                   paused ? ", paused" : "");
            next_report_ms = now_ms + KEEPALIVE_REPORT_INTERVAL_MS;
        }

        if (paused)
        {
            timeout_ms = 1000;
        }
        else if (next_send_ms <= now_ms)
        {
            timeout_ms = 0;
        }
        else if (next_send_ms - now_ms > (uint64_t)INT_MAX)
        {
            timeout_ms = INT_MAX;
        }
        else
        {
            timeout_ms = (int)(next_send_ms - now_ms);
        }

        ret = poll(descriptors, 2U, timeout_ms);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -errno;
        }

        if ((descriptors[1].revents & POLLIN) != 0)
        {
            _keepalive_drain_socket(socket_fd);
        }

        if ((descriptors[0].revents & POLLIN) != 0)
        {
            if (fgets(input, sizeof(input), stdin) == NULL)
            {
                descriptors[0].fd = -1;
                continue;
            }

            ret = _keepalive_handle_input(input, &interval_ms, &paused);
            if (ret > 0)
            {
                break;
            }

            next_send_ms = _keepalive_now_ms();
        }
    }

    printf("结束：sent=%llu, errors=%llu\n",
           (unsigned long long)sent_count,
           (unsigned long long)error_count);

    return 0;
}

/****************************** 程序入口 ******************************/

int main(int argc, char *argv[])
{
    struct sockaddr_in target;
    const char        *target_text;
    const char        *interface_name;
    uint32_t           interval_ms;
    uint32_t           port;
    int                socket_fd;
    int                ret;

    interval_ms  = KEEPALIVE_DEFAULT_INTERVAL_MS;
    target_text  = KEEPALIVE_DEFAULT_TARGET;
    port         = KEEPALIVE_DEFAULT_PORT;
    interface_name = KEEPALIVE_DEFAULT_INTERFACE;

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
    {
        _keepalive_print_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    if (argc > 5)
    {
        _keepalive_print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (argc > 1 && _keepalive_parse_u32(argv[1], 1U, KEEPALIVE_MAX_INTERVAL_MS,
                                         &interval_ms) != 0)
    {
        fprintf(stderr, "无效发送间隔：%s\n", argv[1]);
        return EXIT_FAILURE;
    }

    if (argc > 2)
    {
        target_text = argv[2];
    }

    if (argc > 3 && _keepalive_parse_u32(argv[3], 1U, 65535U, &port) != 0)
    {
        fprintf(stderr, "无效目标端口：%s\n", argv[3]);
        return EXIT_FAILURE;
    }

    if (argc > 4)
    {
        interface_name = argv[4];
    }

    if (interface_name[0] == '\0' || strlen(interface_name) >= IFNAMSIZ)
    {
        fprintf(stderr, "无效出口接口：%s\n", interface_name);
        return EXIT_FAILURE;
    }

    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port   = htons((uint16_t)port);

    ret = inet_pton(AF_INET, target_text, &target.sin_addr);
    if (ret != 1)
    {
        fprintf(stderr, "无效目标IPv4地址：%s\n", target_text);
        return EXIT_FAILURE;
    }

    signal(SIGINT, _keepalive_handle_signal);
    signal(SIGTERM, _keepalive_handle_signal);
    setvbuf(stdout, NULL, _IOLBF, 0U);

    socket_fd = _keepalive_open_socket(interface_name, &target);
    if (socket_fd < 0)
    {
        fprintf(stderr, "创建UDP套接字失败：error=%d (%s)\n",
                socket_fd,
                strerror(-socket_fd));
        return EXIT_FAILURE;
    }

    printf("蜂窝保活测试已启动：target=%s:%u, interface=%s, interval=%u ms\n",
           target_text,
           port,
           interface_name,
           interval_ms);
    printf("运行中输入新的毫秒间隔；输入0暂停；输入q退出。\n");

    ret = _keepalive_run(socket_fd, interval_ms);
    close(socket_fd);

    if (ret != 0)
    {
        fprintf(stderr, "发送循环失败：error=%d (%s)\n", ret, strerror(-ret));
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

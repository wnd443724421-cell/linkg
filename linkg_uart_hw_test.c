/**
 * @file linkg_uart_hw_test.c
 * @brief LinkG UART3/4/5与UART4 SBUS硬件收发测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-22
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "linkg_gpio.h"
#include "linkg_uart.h"

/****************************** 模块常量 ******************************/

#define LINKG_UART_TEST_SBUS_BAUDRATE 100000U // SBUS固定波特率
#define LINKG_UART_TEST_SBUS_SIZE     25U     // SBUS完整帧长度
#define LINKG_UART_TEST_POLL_MS       1000    // poll等待间隔
#define LINKG_UART_TEST_BUFFER_SIZE   512U    // 单次串口接收缓冲区长度

/****************************** 内部类型 ******************************/

typedef struct
{
    uint8_t  buffer[LINKG_UART_TEST_SBUS_SIZE]; // SBUS组帧缓存
    size_t   count;                             // 已积累的帧字节数
    uint64_t valid_frames;                      // 合法SBUS帧计数
    uint64_t invalid_frames;                    // 不合法候选帧计数
    uint64_t rx_bytes;                          // 原始UART接收总字节数
} linkg_uart_test_sbus_t;

/****************************** 全局上下文 ******************************/

static volatile sig_atomic_t g_uart_test_stop = 0; // 收到退出信号后停止接收循环
static linux_gpio_t         *g_uart_test_sbus = NULL; // PD16模式控制句柄
static linux_gpio_t         *g_uart_test_rx   = NULL; // PD21接收门控句柄
static int                   g_uart_test_fd   = -1;   // 当前被测试的串口描述符

/****************************** 参数处理 ******************************/

/**
 * @brief 打印命令行用法。
 */
static void _uart_test_usage(const char *program)
{
    printf("用法:\n");
    printf("  %s 3 <baudrate>           普通UART3，8N1\n", program);
    printf("  %s 4 <baudrate>           普通UART4，8N1\n", program);
    printf("  %s 5 <baudrate>           普通UART5，8N1\n", program);
    printf("  %s normal 3 <baudrate>    显式指定普通模式\n", program);
    printf("  %s sbus [4]              SBUS模式，仅UART4，固定100000 8E2\n", program);
    printf("\n波特率沿用LinkG能力列表，必须满足驱动上报baud_base的整数分频无误差条件。\n");
    printf("1200 2400 4800 9600 19200 38400 57600 100000 115200 150000 187500\n");
    printf("250000 300000 375000 500000 750000 1500000\n");
    printf("普通模式可在终端输入一行并按回车发送；Ctrl+C退出。\n");
}

/**
 * @brief 校验波特率是否属于LinkG的物理UART支持列表。
 */
static bool _uart_test_baud_supported(uint32_t baudrate)
{
    static const uint32_t baudrates[] =
    {
        1200U, 2400U, 4800U, 9600U, 19200U, 38400U, 57600U,
        100000U, 115200U, 150000U, 187500U, 250000U, 300000U,
        375000U, 500000U, 750000U, 1500000U
    };
    size_t index;

    for (index = 0U; index < sizeof(baudrates) / sizeof(baudrates[0]); index++)
    {
        if (baudrate == baudrates[index])
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 严格解析无符号十进制数。
 */
static int _uart_test_number(const char *text, uint32_t *value)
{
    unsigned long parsed;
    char         *end;

    if (text == NULL || value == NULL || text[0] < '0' || text[0] > '9')
    {
        return -EINVAL;
    }

    errno  = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed == 0UL || parsed > UINT32_MAX)
    {
        return -EINVAL;
    }

    *value = (uint32_t)parsed;
    return 0;
}

/**
 * @brief 将数字3/4/5映射为LinkG板级tty设备路径。
 */
static const char *_uart_test_device(uint32_t uart)
{
    switch (uart)
    {
        case 3U: return "/dev/ttyS3";
        case 4U: return "/dev/ttyS4";
        case 5U: return "/dev/ttyS5";
        default: return NULL;
    }
}

/**
 * @brief 按mode、物理端口与波特率解析测试参数。
 */
static int _uart_test_parse(int argc, char **argv, bool *sbus, uint32_t *uart, uint32_t *baudrate)
{
    int index;

    if (argc < 2 || sbus == NULL || uart == NULL || baudrate == NULL)
    {
        return -EINVAL;
    }

    *sbus = strcmp(argv[1], "sbus") == 0;
    index = strcmp(argv[1], "normal") == 0 || *sbus ? 2 : 1;

    if (*sbus)
    {
        if (argc > 3 || (argc == 3 && strcmp(argv[2], "4") != 0))
        {
            return -EINVAL;
        }

        *uart     = 4U;
        *baudrate = LINKG_UART_TEST_SBUS_BAUDRATE;
        return 0;
    }

    if (argc != index + 2 || _uart_test_number(argv[index], uart) != 0 || _uart_test_number(argv[index + 1], baudrate) != 0)
    {
        return -EINVAL;
    }

    if (_uart_test_device(*uart) == NULL || !_uart_test_baud_supported(*baudrate))
    {
        return -EINVAL;
    }

    return 0;
}

/****************************** 时钟及信号 ******************************/

/**
 * @brief 接收到终止信号时只设置原子退出标志。
 */
static void _uart_test_signal(int signum)
{
    (void)signum;
    g_uart_test_stop = 1;
}

/**
 * @brief 获取单调时钟毫秒数。
 */
static uint64_t _uart_test_now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }

    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/****************************** 硬件初始化 ******************************/

/**
 * @brief 用UART驱动报告的baud_base验证理论整数分频恰好等于目标波特率。
 *
 * 无法取得serial_struct时拒绝测试；这只是驱动时钟的整数分频检查，
 * 不证明晶振误差或真实UART输出频率为零。
 */
static int _uart_test_check_exact_baud(const char *device, uint32_t baudrate, uint32_t *baud_base, uint32_t *divisor)
{
    struct serial_struct serial;
    uint32_t             base;
    int                  fd;
    int                  ret;

    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        return -errno;
    }

    memset(&serial, 0, sizeof(serial));
    ret = ioctl(fd, TIOCGSERIAL, &serial);
    if (ret < 0)
    {
        ret = -errno;
        close(fd);
        return ret;
    }

    close(fd);

    if (serial.baud_base <= 0)
    {
        return -ERANGE;
    }

    base = (uint32_t)serial.baud_base;
    if (base % baudrate != 0U || base / baudrate == 0U || base / baudrate > UINT16_MAX)
    {
        fprintf(stderr, "不允许非零理论分频误差: baud_base=%u, requested=%u\n", base, baudrate);
        return -ERANGE;
    }

    *baud_base = base;
    *divisor   = base / baudrate;
    return 0;
}

/**
 * @brief 将GPIO恢复为接收关闭、SBUS直通模式并释放全部本程序句柄。
 */
static void _uart_test_cleanup(void)
{
    if (g_uart_test_rx != NULL)
    {
        (void)linux_gpio_write(g_uart_test_rx, true); // PD21高电平关闭UART RX
    }

    if (g_uart_test_fd >= 0)
    {
        (void)linux_uart_close(g_uart_test_fd);
        g_uart_test_fd = -1;
    }

    if (g_uart_test_sbus != NULL)
    {
        (void)linux_gpio_write(g_uart_test_sbus, false); // PD16低电平恢复普通模式
    }

    linux_gpio_close(g_uart_test_rx);
    linux_gpio_close(g_uart_test_sbus);
    g_uart_test_rx   = NULL;
    g_uart_test_sbus = NULL;
}

/**
 * @brief 按LinkG UART Manager顺序初始化SBUS模式、串口与接收使能。
 */
static int _uart_test_open(const char *device, bool sbus, uint32_t baudrate)
{
    uart_config_t config;
    uint32_t      baud_base = 0U;
    uint32_t      divisor   = 0U;
    int           ret;

    ret = _uart_test_check_exact_baud(device, baudrate, &baud_base, &divisor);
    if (ret != 0)
    {
        return ret;
    }

    // PD16初始关闭SBUS，PD21初始关闭接收；GPIO句柄由本进程独占。
    ret = linux_gpio_open("PD16", LINUX_GPIO_DIRECTION_OUTPUT, false, &g_uart_test_sbus);
    if (ret != 0)
    {
        return ret;
    }

    ret = linux_gpio_open("PD21", LINUX_GPIO_DIRECTION_OUTPUT, true, &g_uart_test_rx);
    if (ret != 0)
    {
        return ret;
    }

    ret = linux_gpio_write(g_uart_test_sbus, sbus);
    if (ret != 0)
    {
        return ret;
    }

    // MCU侧约1ms采样PD16，等待切换完成后再打开物理串口。
    usleep(5000U);

    memset(&config, 0, sizeof(config));
    config.baudrate        = (int)baudrate;
    config.data_bits       = 8;
    config.stop_bits       = sbus ? 2 : 1;
    config.parity          = sbus ? 'e' : 'n';
    config.hw_flow_control = false;
    config.exclusive       = true;

    g_uart_test_fd = linux_uart_open(device, &config);
    if (g_uart_test_fd < 0)
    {
        return g_uart_test_fd;
    }

    // 必须在UART配置完成后拉低PD21使能接收。
    ret = linux_gpio_write(g_uart_test_rx, false);
    if (ret != 0)
    {
        return ret;
    }

    printf("设备: %s | 模式: %s | 参数: %u 8%c%d\n", device, sbus ? "SBUS" : "Normal", baudrate, sbus ? 'E' : 'N', sbus ? 2 : 1);
    printf("分频: baud_base=%u, divisor=%u, 理论值=%u, 整数分频误差=0\n", baud_base, divisor, baud_base / divisor);
    printf("GPIO: PD16=%d (SBUS模式), PD21=0 (UART接收使能)\n", sbus ? 1 : 0);
    printf("说明: 校验的是驱动公布的整数分频关系，不是示波器实测波特率。\n");
    return 0;
}

/****************************** 普通串口测试 ******************************/

/**
 * @brief 将一段接收字节按十六进制和可打印ASCII输出。
 */
static void _uart_test_hexdump(const uint8_t *data, size_t length)
{
    size_t offset;
    size_t i;

    for (offset = 0U; offset < length; offset += 16U)
    {
        printf("RX  ");
        for (i = offset; i < length && i < offset + 16U; i++)
        {
            printf("%02X ", data[i]);
        }
        for (; i < offset + 16U; i++)
        {
            printf("   ");
        }
        printf(" | ");
        for (i = offset; i < length && i < offset + 16U; i++)
        {
            putchar(data[i] >= 32U && data[i] <= 126U ? (int)data[i] : '.');
        }
        putchar('\n');
    }
}

/**
 * @brief 对普通UART发送全部输入字节，处理非阻塞写入和部分写入。
 */
static int _uart_test_send(const uint8_t *data, size_t length)
{
    struct pollfd descriptor;
    size_t        offset;
    int           ret;

    offset = 0U;
    while (offset < length && !g_uart_test_stop)
    {
        ret = linux_uart_write(g_uart_test_fd, data + offset, (int)(length - offset));
        if (ret > 0)
        {
            offset += (size_t)ret;
            continue;
        }

        if (ret != -EAGAIN)
        {
            return ret == 0 ? -EIO : ret;
        }

        descriptor.fd      = g_uart_test_fd;
        descriptor.events  = POLLOUT;
        descriptor.revents = 0;
        ret = poll(&descriptor, 1U, 1000);
        if (ret < 0 && errno != EINTR)
        {
            return -errno;
        }
        if (ret == 0)
        {
            return -ETIMEDOUT;
        }
    }

    return offset == length ? 0 : -EINTR;
}

/****************************** SBUS接收校验 ******************************/

/**
 * @brief 判断是否为LinkG接受的25字节SBUS帧。
 */
static bool _uart_test_sbus_valid(const uint8_t *frame)
{
    return frame[0] == 0x0FU &&
           (frame[24] == 0x00U || frame[24] == 0x04U || frame[24] == 0x14U);
}

/**
 * @brief 解出16路11bit SBUS原始通道值和状态标志。
 */
static void _uart_test_sbus_print(const uint8_t *frame, uint64_t number)
{
    uint16_t channels[16];
    uint32_t channel;
    uint32_t bit;
    uint32_t offset;

    for (channel = 0U; channel < 16U; channel++)
    {
        channels[channel] = 0U;
        offset = channel * 11U;
        for (bit = 0U; bit < 11U; bit++)
        {
            if ((frame[1U + (offset + bit) / 8U] & (1U << ((offset + bit) % 8U))) != 0U)
            {
                channels[channel] |= (uint16_t)(1U << bit);
            }
        }
    }

    printf("SBUS #%llu", (unsigned long long)number);
    for (channel = 0U; channel < 16U; channel++)
    {
        printf(" CH%u=%u", channel + 1U, channels[channel]);
    }
    printf(" flags=0x%02X frame_lost=%u failsafe=%u\n", frame[23], (frame[23] & 0x04U) != 0U, (frame[23] & 0x08U) != 0U);
}

/**
 * @brief 按字节拼帧并在失步时向后搜索下一个0x0F同步字节。
 */
static void _uart_test_sbus_input(linkg_uart_test_sbus_t *sbus, const uint8_t *data, size_t length)
{
    size_t index;
    size_t next;

    sbus->rx_bytes += length;
    for (index = 0U; index < length; index++)
    {
        if (sbus->count == 0U && data[index] != 0x0FU)
        {
            continue;
        }

        sbus->buffer[sbus->count++] = data[index];
        if (sbus->count < LINKG_UART_TEST_SBUS_SIZE)
        {
            continue;
        }

        if (_uart_test_sbus_valid(sbus->buffer))
        {
            sbus->valid_frames++;
            if (sbus->valid_frames == 1U || sbus->valid_frames % 10U == 0U)
            {
                _uart_test_sbus_print(sbus->buffer, sbus->valid_frames);
            }
            sbus->count = 0U;
            continue;
        }

        sbus->invalid_frames++;
        for (next = 1U; next < LINKG_UART_TEST_SBUS_SIZE; next++)
        {
            if (sbus->buffer[next] == 0x0FU)
            {
                break;
            }
        }

        if (next < LINKG_UART_TEST_SBUS_SIZE)
        {
            sbus->count = LINKG_UART_TEST_SBUS_SIZE - next;
            memmove(sbus->buffer, sbus->buffer + next, sbus->count);
        }
        else
        {
            sbus->count = 0U;
        }
    }
}

/****************************** 测试主循环 ******************************/

/**
 * @brief 持续读取串口；普通模式同时允许从标准输入发送数据。
 */
static int _uart_test_run(bool sbus)
{
    linkg_uart_test_sbus_t sbus_state = {0};
    struct pollfd         descriptors[2];
    uint8_t               buffer[LINKG_UART_TEST_BUFFER_SIZE];
    uint64_t              next_stats_ms;
    int                   count;
    int                   ret;
    int                   n;
    bool                  input_enabled;

    input_enabled = !sbus;
    next_stats_ms = _uart_test_now_ms() + 1000U;
    printf("%s\n", sbus ? "正在检查SBUS原始帧，按Ctrl+C结束..." : "正在接收UART数据；输入一行并回车可发送，按Ctrl+C结束...");

    while (!g_uart_test_stop)
    {
        descriptors[0].fd      = g_uart_test_fd;
        descriptors[0].events  = POLLIN;
        descriptors[0].revents = 0;
        count = 1;

        if (input_enabled)
        {
            descriptors[1].fd      = STDIN_FILENO;
            descriptors[1].events  = POLLIN;
            descriptors[1].revents = 0;
            count = 2;
        }

        ret = poll(descriptors, (nfds_t)count, LINKG_UART_TEST_POLL_MS);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -errno;
        }

        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -EIO;
        }

        if ((descriptors[0].revents & POLLIN) != 0)
        {
            n = linux_uart_read(g_uart_test_fd, buffer, sizeof(buffer));
            if (n < 0)
            {
                return n;
            }
            if (n > 0)
            {
                if (sbus)
                {
                    _uart_test_sbus_input(&sbus_state, buffer, (size_t)n);
                }
                else
                {
                    _uart_test_hexdump(buffer, (size_t)n);
                }
            }
        }

        if (input_enabled && (descriptors[1].revents & POLLIN) != 0)
        {
            n = (int)read(STDIN_FILENO, buffer, sizeof(buffer));
            if (n > 0)
            {
                ret = _uart_test_send(buffer, (size_t)n);
                if (ret != 0)
                {
                    return ret;
                }
                printf("TX: %d bytes\n", n);
            }
            else if (n == 0)
            {
                input_enabled = false;
            }
            else if (errno != EINTR)
            {
                return -errno;
            }
        }

        if (input_enabled && (descriptors[1].revents & (POLLHUP | POLLNVAL)) != 0 && (descriptors[1].revents & POLLIN) == 0)
        {
            input_enabled = false;
        }

        if (sbus && _uart_test_now_ms() >= next_stats_ms)
        {
            printf("统计: RX字节=%llu 有效SBUS帧=%llu 失步候选=%llu\n",
                   (unsigned long long)sbus_state.rx_bytes,
                   (unsigned long long)sbus_state.valid_frames,
                   (unsigned long long)sbus_state.invalid_frames);
            next_stats_ms = _uart_test_now_ms() + 1000U;
        }
    }

    return 0;
}

/****************************** 程序入口 ******************************/

/**
 * @brief 解析参数、启动硬件测试并恢复所有GPIO及串口资源。
 */
int main(int argc, char **argv)
{
    struct sigaction action;
    const char      *device;
    uint32_t         uart;
    uint32_t         baudrate;
    bool             sbus;
    int              ret;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
    {
        _uart_test_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    ret = _uart_test_parse(argc, argv, &sbus, &uart, &baudrate);
    if (ret != 0)
    {
        _uart_test_usage(argv[0]);
        return EXIT_FAILURE;
    }

    device = _uart_test_device(uart);

    memset(&action, 0, sizeof(action));
    action.sa_handler = _uart_test_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    ret = _uart_test_open(device, sbus, baudrate);
    if (ret == 0)
    {
        ret = _uart_test_run(sbus);
    }

    if (ret != 0)
    {
        fprintf(stderr, "UART测试失败: %s (%d)\n", strerror(-ret), ret);
        if (ret == -EBUSY)
        {
            fprintf(stderr, "请先停止LinkG主程序，避免GPIO/串口被其他进程占用。\n");
        }
    }

    _uart_test_cleanup();
    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

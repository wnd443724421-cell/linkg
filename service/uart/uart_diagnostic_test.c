/**
 * @file uart_diagnostic_test.c
 * @brief UART普通回环与SBUS桥接回环测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-31
 */

#include "uart_diagnostic_internal.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "linkg_time.h"
#include "linkg_log.h"
#include "linkg_uart.h"
#include "linkg_uart_manager.h"

/****************************** 测试常量 ******************************/

#define LINKG_UART_DIAG_BAUDRATE          100000U // 普通和SBUS测试统一波特率
#define LINKG_UART_DIAG_DATA_BITS         8U      // 统一数据位
#define LINKG_UART_DIAG_STOP_BITS         2U      // 统一停止位
#define LINKG_UART_DIAG_PARITY            'e'     // 统一偶校验
#define LINKG_UART_DIAG_BLOCK_SIZE        256U    // 普通UART单组测试长度
#define LINKG_UART_DIAG_NORMAL_PATTERNS   6U      // 普通UART测试数据模式数量
#define LINKG_UART_DIAG_SBUS_FRAME_SIZE   25U     // SBUS标准帧长度
#define LINKG_UART_DIAG_SBUS_FRAME_COUNT  20U     // SBUS回环测试帧数
#define LINKG_UART_DIAG_IO_TIMEOUT_MS     300U    // 单次收发超时
#define LINKG_UART_DIAG_POLL_SLICE_MS     50U     // 轮询取消检查间隔
#define LINKG_UART_DIAG_SBUS_INTERVAL_US  7600U   // SBUS帧间隔

/****************************** 内部辅助 ******************************/

/**
 * @brief 拒绝内核正在使用的控制台，TIOCEXCL无法阻止console输出、登录和SysRq抢占。
 */
static int _uart_diag_check_console(const char *device_path)
{
    FILE *consoles;
    char  line[256];
    char  name[64];
    char *flags;
    int   ret;

    consoles = fopen("/proc/consoles", "re");
    if (consoles == NULL)
    {
        return -errno;
    }

    ret = 0;
    while (fgets(line, sizeof(line), consoles) != NULL)
    {
        flags = strchr(line, '(');
        if (sscanf(line, "%63s", name) == 1 && flags != NULL && strchr(flags, 'E') != NULL &&
            strncmp(device_path, "/dev/", 5U) == 0 && strcmp(device_path + 5U, name) == 0)
        {
            LINKG_LOG_WARN("UART diagnostic: %s is an enabled system console; loopback refused", device_path);
            ret = -EBUSY;
            break;
        }
    }
    if (ret == 0 && ferror(consoles))
    {
        ret = -EIO;
    }
    fclose(consoles);
    return ret;
}

/**
 * @brief 等待串口描述符出现指定事件，并周期检查取消请求。
 */
static int _uart_diag_wait_fd_until(int fd, short events, uint64_t deadline_ms)
{
    struct pollfd descriptor;
    uint64_t      now_ms;
    uint32_t      wait_ms;
    int           ret;

    descriptor.fd      = fd;
    descriptor.events  = events;
    descriptor.revents = 0;

    while (true)
    {
        if (uart_diagnostic_cancel_requested())
        {
            return -ECANCELED;
        }

        now_ms = linkg_time_elapsed_ms();
        if (now_ms >= deadline_ms)
        {
            return -ETIMEDOUT;
        }

        wait_ms = (uint32_t)(deadline_ms - now_ms);
        if (wait_ms > LINKG_UART_DIAG_POLL_SLICE_MS)
        {
            wait_ms = LINKG_UART_DIAG_POLL_SLICE_MS;
        }

        descriptor.revents = 0;
        ret = poll(&descriptor, 1U, (int)wait_ms);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -errno;
        }

        if (ret == 0)
        {
            continue;
        }

        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            LINKG_LOG_WARN("UART diagnostic: poll fd=%d events=0x%x revents=0x%x", fd,
                           (unsigned int)events, (unsigned int)descriptor.revents);
            return -EIO;
        }

        if ((descriptor.revents & events) != 0)
        {
            return 0;
        }
    }
}

/**
 * @brief 完整发送指定数据，正确处理非阻塞写入和部分提交。
 */
static int _uart_diag_write_exact(int fd, const uint8_t *data, uint32_t length, uint32_t *written_total)
{
    uint32_t offset;
    uint64_t deadline_ms;
    int      ret;

    offset      = 0U;
    deadline_ms = linkg_time_elapsed_ms() + LINKG_UART_DIAG_IO_TIMEOUT_MS;
    while (offset < length)
    {
        if (uart_diagnostic_cancel_requested())
        {
            return -ECANCELED;
        }

        ret = linux_uart_write(fd, data + offset, (int)(length - offset));
        if (ret > 0)
        {
            offset += (uint32_t)ret;
            *written_total += (uint32_t)ret;
            continue;
        }

        if (ret != -EAGAIN && ret != -EWOULDBLOCK && ret != 0)
        {
            return ret;
        }

        ret = _uart_diag_wait_fd_until(fd, POLLOUT, deadline_ms);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/**
 * @brief 完整接收指定长度的数据，避免把短读误判为完整帧。
 */
static int _uart_diag_read_exact(int fd, uint8_t *data, uint32_t length, uint32_t *received_total)
{
    uint32_t offset;
    uint64_t deadline_ms;
    int      ret;

    offset      = 0U;
    deadline_ms = linkg_time_elapsed_ms() + LINKG_UART_DIAG_IO_TIMEOUT_MS;
    while (offset < length)
    {
        if (uart_diagnostic_cancel_requested())
        {
            return -ECANCELED;
        }

        ret = _uart_diag_wait_fd_until(fd, POLLIN, deadline_ms);
        if (ret != 0)
        {
            return ret;
        }

        ret = linux_uart_read(fd, data + offset, (int)(length - offset));
        if (ret > 0)
        {
            offset += (uint32_t)ret;
            *received_total += (uint32_t)ret;
            continue;
        }

        if (ret < 0)
        {
            LINKG_LOG_WARN("UART diagnostic: read fd=%d error=%d", fd, ret);
            return ret;
        }
    }

    return 0;
}

/**
 * @brief 根据模式生成普通UART测试数据。
 */
static void _uart_diag_build_normal_pattern(uint32_t pattern, uint8_t *data)
{
    uint32_t index;
    uint32_t random_state;

    random_state = 0x6D2B79F5U;

    for (index = 0U; index < LINKG_UART_DIAG_BLOCK_SIZE; index++)
    {
        switch (pattern)
        {
            case 0U:
                data[index] = 0x00U;
                break;

            case 1U:
                data[index] = 0xFFU;
                break;

            case 2U:
                data[index] = (index & 1U) == 0U ? 0x55U : 0xAAU;
                break;

            case 3U:
                data[index] = (uint8_t)index;
                break;

            case 4U:
                data[index] = (uint8_t)(1U << (index & 7U));
                break;

            default:
                random_state ^= random_state << 13U;
                random_state ^= random_state >> 17U;
                random_state ^= random_state << 5U;
                data[index] = (uint8_t)random_state;
                break;
        }
    }
}

/**
 * @brief 将16个11位通道值打包成标准25字节SBUS帧。
 */
static void _uart_diag_build_sbus_frame(uint32_t frame_index, uint8_t *frame)
{
    uint16_t channel_value;
    uint32_t channel;
    uint32_t bit;
    uint32_t bit_offset;
    uint32_t mode;

    memset(frame, 0, LINKG_UART_DIAG_SBUS_FRAME_SIZE);
    frame[0] = 0x0FU;
    mode     = frame_index % 4U;

    for (channel = 0U; channel < 16U; channel++)
    {
        if (mode == 0U)
        {
            channel_value = 172U;
        }
        else if (mode == 1U)
        {
            channel_value = 992U;
        }
        else if (mode == 2U)
        {
            channel_value = 1811U;
        }
        else
        {
            channel_value = (uint16_t)(172U + ((channel * 97U + frame_index * 53U) % 1640U));
        }

        bit_offset = channel * 11U;
        for (bit = 0U; bit < 11U; bit++)
        {
            if ((channel_value & (uint16_t)(1U << bit)) != 0U)
            {
                frame[1U + ((bit_offset + bit) / 8U)] |= (uint8_t)(1U << ((bit_offset + bit) & 7U));
            }
        }
    }

    frame[23] = (uint8_t)(frame_index & 0x03U);
    frame[24] = 0x00U;
}

/**
 * @brief 记录数据不一致位置和数量。
 */
static void _uart_diag_compare(const uint8_t *expected, const uint8_t *actual, uint32_t length, linkg_uart_diagnostic_test_result_t *result)
{
    uint32_t index;
    uint32_t base_offset;

    base_offset = result->tx_bytes - length;

    for (index = 0U; index < length; index++)
    {
        if (expected[index] == actual[index])
        {
            continue;
        }

        if (result->error_count == 0U)
        {
            result->first_error_offset = base_offset + index;
        }

        result->error_count++;
    }
}

/**
 * @brief 执行普通UART多模式数据回环。
 */
static int _uart_diag_test_normal(int fd, linkg_uart_diagnostic_test_result_t *result)
{
    uint8_t  tx_data[LINKG_UART_DIAG_BLOCK_SIZE];
    uint8_t  rx_data[LINKG_UART_DIAG_BLOCK_SIZE];
    uint32_t pattern;
    int      ret;

    for (pattern = 0U; pattern < LINKG_UART_DIAG_NORMAL_PATTERNS; pattern++)
    {
        if (uart_diagnostic_cancel_requested())
        {
            return -ECANCELED;
        }

        _uart_diag_build_normal_pattern(pattern, tx_data);
        memset(rx_data, 0, sizeof(rx_data));

        ret = _uart_diag_write_exact(fd, tx_data, sizeof(tx_data), &result->tx_bytes);
        if (ret != 0)
        {
            return ret;
        }

        ret = _uart_diag_read_exact(fd, rx_data, sizeof(rx_data), &result->rx_bytes);
        if (ret != 0)
        {
            return ret;
        }

        _uart_diag_compare(tx_data, rx_data, sizeof(tx_data), result);
    }

    return 0;
}

/**
 * @brief 执行合法SBUS帧回环，逐帧校验25字节内容。
 */
static int _uart_diag_test_sbus(int fd, linkg_uart_diagnostic_test_result_t *result)
{
    uint8_t tx_frame[LINKG_UART_DIAG_SBUS_FRAME_SIZE];
    uint8_t rx_frame[LINKG_UART_DIAG_SBUS_FRAME_SIZE];
    uint32_t index;
    int ret;

    result->frames_total = LINKG_UART_DIAG_SBUS_FRAME_COUNT;

    for (index = 0U; index < LINKG_UART_DIAG_SBUS_FRAME_COUNT; index++)
    {
        if (uart_diagnostic_cancel_requested())
        {
            return -ECANCELED;
        }

        _uart_diag_build_sbus_frame(index, tx_frame);
        memset(rx_frame, 0, sizeof(rx_frame));

        ret = _uart_diag_write_exact(fd, tx_frame, sizeof(tx_frame), &result->tx_bytes);
        if (ret != 0)
        {
            return ret;
        }

        ret = _uart_diag_read_exact(fd, rx_frame, sizeof(rx_frame), &result->rx_bytes);
        if (ret != 0)
        {
            return ret;
        }

        if (memcmp(tx_frame, rx_frame, sizeof(tx_frame)) == 0)
        {
            result->frames_ok++;
        }

        _uart_diag_compare(tx_frame, rx_frame, sizeof(tx_frame), result);

        ret = linkg_time_sleep_us(LINKG_UART_DIAG_SBUS_INTERVAL_US);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** 测试入口 ******************************/

/**
 * @brief 打开指定物理串口并执行普通或SBUS回环测试。
 */
int uart_diagnostic_test_port(uint8_t uart_num, linkg_uart_diagnostic_path_t path, linkg_uart_diagnostic_test_result_t *result)
{
    uart_config_t physical;
    const char   *device_path;
    uint64_t      started_ms;
    int           fd;
    int           ret;

    if (result == NULL || uart_num >= LINKG_UART_DIAGNOSTIC_UART_COUNT ||
        (path != LINKG_UART_DIAGNOSTIC_PATH_NORMAL && path != LINKG_UART_DIAGNOSTIC_PATH_SBUS))
    {
        return -EINVAL;
    }

    started_ms = linkg_time_elapsed_ms();
    memset(result, 0, sizeof(*result));
    result->uart_num           = uart_num;
    result->path               = path;
    result->state              = LINKG_UART_DIAGNOSTIC_TEST_RUNNING;
    result->first_error_offset = UINT32_MAX;

    device_path = linkg_uart_manager_device_path(uart_num);
    if (device_path == NULL)
    {
        ret = -EINVAL;
        goto finished;
    }

    ret = _uart_diag_check_console(device_path);
    if (ret != 0)
    {
        goto finished;
    }

    memset(&physical, 0, sizeof(physical));
    physical.baudrate        = (int)LINKG_UART_DIAG_BAUDRATE;
    physical.data_bits       = (int)LINKG_UART_DIAG_DATA_BITS;
    physical.stop_bits       = (int)LINKG_UART_DIAG_STOP_BITS;
    physical.parity          = LINKG_UART_DIAG_PARITY;
    physical.hw_flow_control = false;
    physical.exclusive       = true;

    // UART_EN#由底层UART驱动管理；诊断模块只打开设备节点，不申请或操作对应GPIO。
    fd = linux_uart_open(device_path, &physical);
    if (fd < 0)
    {
        ret = fd;
        goto finished;
    }

    linux_uart_flush(fd);

    if (path == LINKG_UART_DIAGNOSTIC_PATH_SBUS)
    {
        ret = _uart_diag_test_sbus(fd, result);
    }
    else
    {
        ret = _uart_diag_test_normal(fd, result);
    }

    linux_uart_close(fd);

finished:
    result->error      = ret;
    result->elapsed_ms = linkg_time_elapsed_ms() - started_ms;

    if (ret == -ECANCELED)
    {
        result->state = LINKG_UART_DIAGNOSTIC_TEST_CANCELLED;
    }
    else if (ret != 0 || result->error_count > 0U)
    {
        result->state = LINKG_UART_DIAGNOSTIC_TEST_FAILED;
    }
    else
    {
        result->state = LINKG_UART_DIAGNOSTIC_TEST_PASSED;
    }

    return ret;
}

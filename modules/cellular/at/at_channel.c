/**
 * @file at_channel.c
 * @brief Cellular AT通道管理实现
 * @author Dawn
 * @version 1.0.2
 * @date 2026-08-28
 */

#define _POSIX_C_SOURCE                200809L  // 启用POSIX.1-2008接口

#include "at_channel.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "linkg_log.h"
#include "linkg_thread.h"
#include "linkg_time.h"

/****************************** 通道常量 ******************************/

#define AT_CHANNEL_RESPONSE_SIZE       4096U    // 单次AT命令内部响应缓存大小
#define AT_CHANNEL_LINE_SIZE           512U     // AT单行最大长度
#define AT_CHANNEL_RX_BUFFER_SIZE      512U     // UART单次接收缓存大小
#define AT_CHANNEL_POLL_FD_COUNT       2U       // RX线程poll描述符数量
#define AT_CHANNEL_COMMAND_SUFFIX_LEN  2U       // AT命令结束符长度
#define AT_CHANNEL_THREAD_NAME         "at-rx"  // AT接收线程名称
#define AT_CHANNEL_COMMAND_SUFFIX      "\r\n"   // AT命令结束符
#define AT_CHANNEL_DRAIN_TIMEOUT_MS    1000U    // 超时后迟到响应丢弃窗口

/****************************** 内部类型 ******************************/

typedef enum
{
    AT_CHANNEL_TRANSACTION_IDLE = 0, // 当前无AT事务
    AT_CHANNEL_TRANSACTION_WAITING,  // 等待当前AT事务响应
    AT_CHANNEL_TRANSACTION_DRAINING  // 丢弃超时事务迟到响应
} at_channel_transaction_state_t;

struct at_channel
{
    pthread_mutex_t                lock;                                  // 通道状态锁，保护事务状态和URC配置
    pthread_mutex_t                exec_lock;                             // AT事务串行锁，保证同步命令单路执行
    pthread_mutex_t                lifecycle_lock;                        // 生命周期锁，串行化start/stop/destroy
    pthread_cond_t                 cond;                                  // AT事务完成条件变量
    linkg_thread_t                 rx_thread;                             // UART接收线程

    at_urc_callback_t              urc_callback;                          // URC回调
    void                          *urc_context;                           // URC回调上下文

    at_command_config_t            current_config;                        // 当前AT命令配置
    at_channel_transaction_state_t transaction_state;                     // 当前AT事务状态

    char                           current_command[AT_CHANNEL_LINE_SIZE]; // 当前AT命令
    char                           response[AT_CHANNEL_RESPONSE_SIZE];    // 当前AT响应
    char                           line[AT_CHANNEL_LINE_SIZE];            // 当前接收行

    int                            uart_fd;                               // AT串口描述符
    int                            response_length;                       // 当前响应长度
    int                            line_length;                           // 当前接收行长度
    int                            transaction_result;                    // 当前事务结果

    uint8_t                        continuation_count;                    // 当前已接收的响应续行数量

    bool                           response_started;                      // 当前事务是否已经收到主响应行
    bool                           transaction_done;                      // 当前事务是否完成
    bool                           line_discarding;                       // 是否正在丢弃超长行
    bool                           started;                               // 通道是否已经启动
    uint64_t                       drain_deadline_us;                     // DRAINING截止时间
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断响应行是否具有指定前缀。
 */
static bool _at_channel_has_prefix(const char *line, const char *prefix)
{
    size_t prefix_length;

    if (line == NULL)
    {
        return false;
    }

    if (prefix == NULL || prefix[0] == '\0')
    {
        return false;
    }

    prefix_length = strlen(prefix);

    return strncmp(line, prefix, prefix_length) == 0;
}

/**
 * @brief 判断是否为成功终止响应。
 */
static bool _at_channel_is_success_terminal(const char *line)
{
    if (line == NULL)
    {
        return false;
    }

    return strcmp(line, "OK") == 0;
}

/**
 * @brief 判断是否为失败终止响应。
 */
static bool _at_channel_is_error_terminal(const char *line)
{
    if (line == NULL)
    {
        return false;
    }

    if (strcmp(line, "ERROR") == 0)
    {
        return true;
    }

    if (strcmp(line, "NO CARRIER") == 0)
    {
        return true;
    }

    if (strcmp(line, "NO ANSWER") == 0)
    {
        return true;
    }

    if (strcmp(line, "BUSY") == 0)
    {
        return true;
    }

    if (strcmp(line, "NO DIALTONE") == 0)
    {
        return true;
    }

    if (_at_channel_has_prefix(line, "+CME ERROR:"))
    {
        return true;
    }

    return _at_channel_has_prefix(line, "+CMS ERROR:");
}

/**
 * @brief 判断当前调用线程是否为AT RX线程。
 */
static bool _at_channel_is_rx_thread(const at_channel_t *channel)
{
    if (channel == NULL)
    {
        return false;
    }

    if (!linkg_thread_is_started(&channel->rx_thread) &&
        !linkg_thread_is_running(&channel->rx_thread))
    {
        return false;
    }

    return pthread_equal(pthread_self(), channel->rx_thread.tid) != 0;
}

/**
 * @brief 清空当前AT事务上下文。
 *
 * @note 调用方必须持有channel->lock。
 */
static void _at_channel_reset_transaction_locked(at_channel_t *channel)
{
    memset(&channel->current_config, 0, sizeof(channel->current_config));
    memset(channel->current_command, 0, sizeof(channel->current_command));
    memset(channel->response, 0, sizeof(channel->response));

    channel->transaction_state   = AT_CHANNEL_TRANSACTION_IDLE;
    channel->response_length     = 0;
    channel->transaction_result  = 0;
    channel->continuation_count  = 0U;
    channel->response_started    = false;
    channel->transaction_done    = false;
    channel->drain_deadline_us   = 0U;
}

/**
 * @brief 进入DRAINING状态。
 *
 * @note 调用方必须持有channel->lock。
 */
static void _at_channel_enter_draining_locked(at_channel_t *channel)
{
    memset(&channel->current_config, 0, sizeof(channel->current_config));
    memset(channel->current_command, 0, sizeof(channel->current_command));

    channel->transaction_state  = AT_CHANNEL_TRANSACTION_DRAINING;
    channel->continuation_count = 0U;
    channel->response_started   = false;
    channel->transaction_done   = false;
    channel->drain_deadline_us  = linkg_time_monotonic_us() +
                                  (uint64_t)AT_CHANNEL_DRAIN_TIMEOUT_MS * 1000ULL;
}

/**
 * @brief 离开DRAINING状态。
 *
 * @note 调用方必须持有channel->lock。
 */
static void _at_channel_leave_draining_locked(at_channel_t *channel)
{
    channel->transaction_state  = AT_CHANNEL_TRANSACTION_IDLE;
    channel->continuation_count = 0U;
    channel->response_started   = false;
    channel->drain_deadline_us  = 0U;
}

/**
 * @brief 唤醒RX线程，使其重新计算DRAINING超时。
 */
static void _at_channel_wakeup_rx(at_channel_t *channel)
{
    int ret;

    if (!linkg_thread_is_running(&channel->rx_thread))
    {
        return;
    }

    ret = linkg_thread_wakeup(&channel->rx_thread);

    if (ret != 0)
    {
        LINKG_LOG_WARN("AT_CHANNEL: wake RX thread failed, error=%d", ret);
    }
}

/**
 * @brief 向当前AT响应缓存追加一行。
 *
 * @note 调用方必须持有channel->lock。
 */
static int _at_channel_append_response_locked(at_channel_t *channel, const char *line)
{
    size_t line_length;
    size_t required;

    line_length = strlen(line);
    required = (size_t)channel->response_length + line_length + 1U;

    if (channel->response_length > 0)
    {
        required += 2U;
    }

    if (required > sizeof(channel->response))
    {
        return -ENOSPC;
    }

    if (channel->response_length > 0)
    {
        channel->response[channel->response_length++] = '\r';
        channel->response[channel->response_length++] = '\n';
    }

    memcpy(channel->response + channel->response_length, line, line_length);

    channel->response_length += (int)line_length;
    channel->response[channel->response_length] = '\0';

    return 0;
}

/**
 * @brief 完成当前AT事务并唤醒等待线程。
 *
 * @note 调用方必须持有channel->lock。
 */
static void _at_channel_complete_transaction_locked(at_channel_t *channel, int result)
{
    if (channel->transaction_state != AT_CHANNEL_TRANSACTION_WAITING)
    {
        return;
    }

    if (channel->transaction_result == 0)
    {
        channel->transaction_result = result;
    }

    channel->transaction_state = AT_CHANNEL_TRANSACTION_IDLE;
    channel->transaction_done = true;
    channel->drain_deadline_us = 0U;

    pthread_cond_broadcast(&channel->cond);
}

/**
 * @brief 复制当前AT响应到调用方缓存。
 *
 * @note 调用方必须持有channel->lock。
 */
static int _at_channel_copy_response_locked(at_channel_t *channel, char *response, int response_size)
{
    size_t copy_length;

    if (response == NULL)
    {
        return 0;
    }

    copy_length = (size_t)channel->response_length;

    if (copy_length + 1U > (size_t)response_size)
    {
        copy_length = (size_t)response_size - 1U;

        memcpy(response, channel->response, copy_length);
        response[copy_length] = '\0';

        return -ENOSPC;
    }

    memcpy(response, channel->response, copy_length + 1U);

    return 0;
}

/**
 * @brief 计算AT事务绝对超时时间。
 */
static void _at_channel_make_deadline(int timeout_ms, struct timespec *deadline)
{
    uint64_t deadline_us;

    deadline_us = linkg_time_monotonic_us() + (uint64_t)timeout_ms * 1000ULL;

    deadline->tv_sec = (time_t)(deadline_us / 1000000ULL);
    deadline->tv_nsec = (long)((deadline_us % 1000000ULL) * 1000ULL);
}

/**
 * @brief 根据DRAINING状态计算RX线程poll超时。
 *
 * @return -1表示无限等待，其余值为poll毫秒超时。
 */
static int _at_channel_get_rx_poll_timeout_ms(at_channel_t *channel)
{
    uint64_t now_us;
    uint64_t remaining_us;
    uint64_t remaining_ms;
    int timeout_ms;
    int ret;

    timeout_ms = -1;

    ret = pthread_mutex_lock(&channel->lock);

    if (ret != 0)
    {
        LINKG_LOG_ERROR("AT_CHANNEL: lock state for RX timeout failed, error=%d", -ret);
        return -1;
    }

    if (channel->transaction_state == AT_CHANNEL_TRANSACTION_DRAINING)
    {
        now_us = linkg_time_monotonic_us();

        if (channel->drain_deadline_us == 0U || now_us >= channel->drain_deadline_us)
        {
            timeout_ms = 0;
        }
        else
        {
            remaining_us = channel->drain_deadline_us - now_us;
            remaining_ms = (remaining_us + 999ULL) / 1000ULL;

            if (remaining_ms > (uint64_t)INT_MAX)
            {
                timeout_ms = INT_MAX;
            }
            else
            {
                timeout_ms = (int)remaining_ms;
            }
        }
    }

    pthread_mutex_unlock(&channel->lock);

    return timeout_ms;
}

/**
 * @brief RX线程检查并结束已经超时的DRAINING状态。
 *
 * @note 该函数只能由RX线程调用；成功结束DRAINING后会重置半包行解析状态。
 */
static bool _at_channel_expire_draining_rx(at_channel_t *channel)
{
    bool expired;
    uint64_t now_us;
    int ret;

    expired = false;

    ret = pthread_mutex_lock(&channel->lock);

    if (ret != 0)
    {
        LINKG_LOG_ERROR("AT_CHANNEL: lock state for drain expiry failed, error=%d", -ret);
        return false;
    }

    if (channel->transaction_state == AT_CHANNEL_TRANSACTION_DRAINING)
    {
        now_us = linkg_time_monotonic_us();

        if (channel->drain_deadline_us == 0U || now_us >= channel->drain_deadline_us)
        {
            _at_channel_leave_draining_locked(channel);
            pthread_cond_broadcast(&channel->cond);
            expired = true;
        }
    }

    pthread_mutex_unlock(&channel->lock);

    if (expired)
    {
        channel->line_length = 0;
        channel->line_discarding = false;

        LINKG_LOG_DEBUG("AT_CHANNEL: drain window expired, channel recovered");
    }

    return expired;
}

/****************************** UART发送 ******************************/

/**
 * @brief 完整发送一条AT命令。
 */
static int _at_channel_write_command(at_channel_t *channel, const char *command)
{
    char buffer[AT_CHANNEL_LINE_SIZE + AT_CHANNEL_COMMAND_SUFFIX_LEN];
    size_t command_length;
    size_t total_length;
    size_t offset;
    int ret;

    command_length = strlen(command);
    total_length = command_length + AT_CHANNEL_COMMAND_SUFFIX_LEN;

    memcpy(buffer, command, command_length);
    memcpy(buffer + command_length, AT_CHANNEL_COMMAND_SUFFIX, AT_CHANNEL_COMMAND_SUFFIX_LEN);

    offset = 0U;

    while (offset < total_length)
    {
        ret = linux_uart_write(channel->uart_fd, buffer + offset, (int)(total_length - offset));

        if (ret < 0)
        {
            return ret;
        }

        if (ret == 0)
        {
            return -EIO;
        }

        offset += (size_t)ret;
    }

    return 0;
}

/****************************** 响应处理 ******************************/

/**
 * @brief 处理一条完整AT响应行。
 */
static void _at_channel_process_line(at_channel_t *channel, const char *line)
{
    at_urc_callback_t callback;
    void *context;
    bool drain_expired;
    uint64_t now_us;
    int ret;

    callback = NULL;
    context = NULL;
    drain_expired = false;

    ret = pthread_mutex_lock(&channel->lock);

    if (ret != 0)
    {
        LINKG_LOG_ERROR("AT_CHANNEL: lock RX state failed, error=%d", -ret);
        return;
    }

    /**
     * 上一条命令已经超时。
     * DRAINING窗口内收到的内容全部视为迟到响应并丢弃；
     * 如果窗口已经过期，则立即恢复IDLE，并按正常规则处理当前完整行。
     */
    if (channel->transaction_state == AT_CHANNEL_TRANSACTION_DRAINING)
    {
        now_us = linkg_time_monotonic_us();
        drain_expired = channel->drain_deadline_us == 0U ||
                        now_us >= channel->drain_deadline_us;

        if (!drain_expired)
        {
            if (_at_channel_is_success_terminal(line) || _at_channel_is_error_terminal(line))
            {
                _at_channel_leave_draining_locked(channel);
                pthread_cond_broadcast(&channel->cond);
            }

            pthread_mutex_unlock(&channel->lock);
            return;
        }

        _at_channel_leave_draining_locked(channel);
        pthread_cond_broadcast(&channel->cond);
    }

    /**
     * 当前不存在同步AT事务，收到的数据作为URC上报。
     */
    if (channel->transaction_state != AT_CHANNEL_TRANSACTION_WAITING)
    {
        callback = channel->urc_callback;
        context = channel->urc_context;

        pthread_mutex_unlock(&channel->lock);

        if (callback != NULL)
        {
            callback(line, context);
        }

        return;
    }

    /**
     * 模块开启Echo时会返回原始AT命令，直接忽略。
     */
    if (strcmp(line, channel->current_command) == 0)
    {
        pthread_mutex_unlock(&channel->lock);
        return;
    }

    /**
     * OK结束当前AT事务。
     */
    if (_at_channel_is_success_terminal(line))
    {
        _at_channel_complete_transaction_locked(channel, 0);

        pthread_mutex_unlock(&channel->lock);
        return;
    }

    /**
     * ERROR结束当前AT事务，同时保留错误响应文本。
     */
    if (_at_channel_is_error_terminal(line))
    {
        ret = _at_channel_append_response_locked(channel, line);

        if (ret != 0 && channel->transaction_result == 0)
        {
            channel->transaction_result = ret;
        }

        _at_channel_complete_transaction_locked(channel, -EREMOTEIO);

        pthread_mutex_unlock(&channel->lock);
        return;
    }

    /**
     * 匹配当前命令期望前缀则作为命令响应。
     */
    if (_at_channel_has_prefix(line, channel->current_config.expect_prefix))
    {
        ret = _at_channel_append_response_locked(channel, line);

        if (ret != 0 && channel->transaction_result == 0)
        {
            channel->transaction_result = ret;
        }

        channel->response_started = true;

        pthread_mutex_unlock(&channel->lock);
        return;
    }

    /**
     * 当前命令已经收到主响应后，由命令层判断无前缀续行是否属于当前事务。
     *
     * @note continuation_match运行于RX线程且持有channel->lock，
     *       匹配函数必须快速返回，禁止调用AT通道接口或执行阻塞操作。
     */
    if (channel->response_started &&
        channel->current_config.continuation_match != NULL &&
        channel->continuation_count < channel->current_config.continuation_max_lines &&
        channel->current_config.continuation_match(line))
    {
        ret = _at_channel_append_response_locked(channel, line);

        if (ret != 0 && channel->transaction_result == 0)
        {
            channel->transaction_result = ret;
        }

        channel->continuation_count++;

        pthread_mutex_unlock(&channel->lock);
        return;
    }

    /**
     * 当前命令允许普通文本响应时，将无前缀文本加入响应。
     */
    if (line[0] != '+' && channel->current_config.accept_plain_text)
    {
        ret = _at_channel_append_response_locked(channel, line);

        if (ret != 0 && channel->transaction_result == 0)
        {
            channel->transaction_result = ret;
        }

        pthread_mutex_unlock(&channel->lock);
        return;
    }

    /**
     * 其余内容作为URC上报。
     */
    callback = channel->urc_callback;
    context = channel->urc_context;

    pthread_mutex_unlock(&channel->lock);

    if (callback != NULL)
    {
        callback(line, context);
    }
}

/**
 * @brief 将UART字节流组装为完整AT响应行。
 */
static void _at_channel_feed_rx(at_channel_t *channel, const uint8_t *data, int length)
{
    int index;

    for (index = 0; index < length; index++)
    {
        if (data[index] == '\r' || data[index] == '\0')
        {
            continue;
        }

        if (data[index] == '\n')
        {
            if (channel->line_discarding)
            {
                channel->line_discarding = false;
                channel->line_length = 0;

                continue;
            }

            if (channel->line_length == 0)
            {
                continue;
            }

            channel->line[channel->line_length] = '\0';
            channel->line_length = 0;

            _at_channel_process_line(channel, channel->line);

            continue;
        }

        if (channel->line_discarding)
        {
            continue;
        }

        if (channel->line_length >= (int)sizeof(channel->line) - 1)
        {
            channel->line_discarding = true;
            channel->line_length = 0;

            LINKG_LOG_WARN("AT_CHANNEL: response line too long, action=discard");

            continue;
        }

        channel->line[channel->line_length++] = (char)data[index];
    }
}

/**
 * @brief 记录RX线程故障并终止当前事务。
 */
static void _at_channel_fail_rx(at_channel_t *channel, int error)
{
    int ret;

    ret = pthread_mutex_lock(&channel->lock);

    if (ret != 0)
    {
        return;
    }

    channel->started = false;

    if (channel->transaction_state == AT_CHANNEL_TRANSACTION_WAITING)
    {
        _at_channel_complete_transaction_locked(channel, error);
    }
    else if (channel->transaction_state == AT_CHANNEL_TRANSACTION_DRAINING)
    {
        _at_channel_leave_draining_locked(channel);
        pthread_cond_broadcast(&channel->cond);
    }

    pthread_mutex_unlock(&channel->lock);
}

/****************************** RX线程 ******************************/

/**
 * @brief AT UART接收线程。
 */
static void _at_channel_rx_thread(linkg_thread_t *thread, void *user_data)
{
    struct pollfd descriptors[AT_CHANNEL_POLL_FD_COUNT];
    at_channel_t *channel;
    uint8_t buffer[AT_CHANNEL_RX_BUFFER_SIZE];
    int poll_timeout_ms;
    int wakeup_fd;
    int ret;

    channel = (at_channel_t *)user_data;

    if (channel == NULL)
    {
        return;
    }

    wakeup_fd = linkg_thread_get_wakeup_fd(thread);

    if (wakeup_fd < 0)
    {
        LINKG_LOG_ERROR("AT_CHANNEL: get RX wakeup fd failed, error=%d", wakeup_fd);
        _at_channel_fail_rx(channel, wakeup_fd);

        return;
    }

    memset(descriptors, 0, sizeof(descriptors));

    descriptors[0].fd = wakeup_fd;
    descriptors[0].events = POLLIN;

    descriptors[1].fd = channel->uart_fd;
    descriptors[1].events = POLLIN;

    LINKG_LOG_DEBUG("AT_CHANNEL: RX thread entered");

    while (linkg_thread_is_running(thread))
    {
        poll_timeout_ms = _at_channel_get_rx_poll_timeout_ms(channel);

        ret = poll(descriptors, AT_CHANNEL_POLL_FD_COUNT, poll_timeout_ms);

        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            ret = errno != 0 ? -errno : -EIO;

            LINKG_LOG_ERROR("AT_CHANNEL: RX poll failed, error=%d", ret);

            _at_channel_fail_rx(channel, ret);

            break;
        }

        if (ret == 0)
        {
            (void)_at_channel_expire_draining_rx(channel);
            continue;
        }

        if ((descriptors[0].revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(thread);

            if (ret != 0)
            {
                LINKG_LOG_ERROR("AT_CHANNEL: clear RX wakeup failed, error=%d", ret);

                _at_channel_fail_rx(channel, ret);

                break;
            }
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            LINKG_LOG_ERROR("AT_CHANNEL: RX wakeup fd failed, revents=0x%x", descriptors[0].revents);

            _at_channel_fail_rx(channel, -EIO);

            break;
        }

        if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            LINKG_LOG_ERROR("AT_CHANNEL: UART fd failed, revents=0x%x", descriptors[1].revents);

            _at_channel_fail_rx(channel, -EIO);

            break;
        }

        /**
         * poll可能因DRAINING状态变更被主动唤醒，也可能在deadline附近因UART数据返回。
         * 在读取串口前先做一次过期检查，确保恢复后不会继续丢弃正常URC。
         */
        (void)_at_channel_expire_draining_rx(channel);

        if ((descriptors[1].revents & POLLIN) == 0)
        {
            continue;
        }

        ret = linux_uart_read(channel->uart_fd, buffer, (int)sizeof(buffer));

        if (ret > 0)
        {
            // 非阻塞read前后可能跨过DRAINING deadline，再检查一次。
            (void)_at_channel_expire_draining_rx(channel);

            _at_channel_feed_rx(channel, buffer, ret);

            continue;
        }

        if (ret == 0)
        {
            continue;
        }

        if (ret == -EAGAIN || ret == -EWOULDBLOCK || ret == -EINTR)
        {
            continue;
        }

        LINKG_LOG_ERROR("AT_CHANNEL: UART read failed, error=%d", ret);

        _at_channel_fail_rx(channel, ret);

        break;
    }

    LINKG_LOG_DEBUG("AT_CHANNEL: RX thread exited");
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建AT通道并打开对应UART设备。
 */
at_channel_t *at_channel_create(const char *device, const uart_config_t *config)
{
    pthread_condattr_t cond_attr;
    at_channel_t *channel;
    int uart_fd;
    int ret;
    bool lock_initialized;
    bool exec_lock_initialized;
    bool lifecycle_lock_initialized;
    bool cond_attr_initialized;
    bool cond_initialized;

    if (device == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    if (device[0] == '\0')
    {
        errno = EINVAL;
        return NULL;
    }

    if (config == NULL)
    {
        errno = EINVAL;
        return NULL;
    }

    channel = calloc(1U, sizeof(*channel));

    if (channel == NULL)
    {
        return NULL;
    }

    channel->uart_fd = -1;

    uart_fd = linux_uart_open(device, config);

    if (uart_fd < 0)
    {
        errno = -uart_fd;
        free(channel);
        return NULL;
    }

    channel->uart_fd = uart_fd;

    lock_initialized = false;
    exec_lock_initialized = false;
    lifecycle_lock_initialized = false;
    cond_attr_initialized = false;
    cond_initialized = false;

    ret = pthread_mutex_init(&channel->lock, NULL);

    if (ret != 0)
    {
        errno = ret;
        goto fail;
    }

    lock_initialized = true;

    ret = pthread_mutex_init(&channel->exec_lock, NULL);

    if (ret != 0)
    {
        errno = ret;
        goto fail;
    }

    exec_lock_initialized = true;

    ret = pthread_mutex_init(&channel->lifecycle_lock, NULL);

    if (ret != 0)
    {
        errno = ret;
        goto fail;
    }

    lifecycle_lock_initialized = true;

    ret = pthread_condattr_init(&cond_attr);

    if (ret != 0)
    {
        errno = ret;
        goto fail;
    }

    cond_attr_initialized = true;

    ret = pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);

    if (ret != 0)
    {
        errno = ret;
        goto fail;
    }

    ret = pthread_cond_init(&channel->cond, &cond_attr);

    if (ret != 0)
    {
        errno = ret;
        goto fail;
    }

    cond_initialized = true;

    pthread_condattr_destroy(&cond_attr);
    cond_attr_initialized = false;

    ret = linkg_thread_init(&channel->rx_thread, AT_CHANNEL_THREAD_NAME, _at_channel_rx_thread, channel);

    if (ret != 0)
    {
        errno = ret < 0 ? -ret : ret;
        goto fail;
    }

    return channel;

fail:

    if (cond_attr_initialized)
    {
        pthread_condattr_destroy(&cond_attr);
    }

    if (cond_initialized)
    {
        pthread_cond_destroy(&channel->cond);
    }

    if (lifecycle_lock_initialized)
    {
        pthread_mutex_destroy(&channel->lifecycle_lock);
    }

    if (exec_lock_initialized)
    {
        pthread_mutex_destroy(&channel->exec_lock);
    }

    if (lock_initialized)
    {
        pthread_mutex_destroy(&channel->lock);
    }

    linux_uart_close(channel->uart_fd);

    free(channel);

    return NULL;
}

/**
 * @brief 启动AT通道接收线程。
 */
int at_channel_start(at_channel_t *channel)
{
    int ret;

    if (channel == NULL)
    {
        return -EINVAL;
    }

    if (_at_channel_is_rx_thread(channel))
    {
        return -EDEADLK;
    }

    ret = pthread_mutex_lock(&channel->lifecycle_lock);

    if (ret != 0)
    {
        return -ret;
    }

    ret = pthread_mutex_lock(&channel->lock);

    if (ret != 0)
    {
        pthread_mutex_unlock(&channel->lifecycle_lock);
        return -ret;
    }

    if (channel->started)
    {
        pthread_mutex_unlock(&channel->lock);
        pthread_mutex_unlock(&channel->lifecycle_lock);

        return 0;
    }

    /**
     * RX线程异常退出后，linkg_thread在join前仍保持started=true。
     * 此时必须先调用at_channel_stop()完成回收，不能直接重复start。
     */
    if (linkg_thread_is_started(&channel->rx_thread))
    {
        pthread_mutex_unlock(&channel->lock);
        pthread_mutex_unlock(&channel->lifecycle_lock);

        return -EBUSY;
    }

    _at_channel_reset_transaction_locked(channel);

    channel->line_length = 0;
    channel->line_discarding = false;

    /**
     * 必须先清串口，再启动RX线程，最后才公开started=true。
     * 这样不会出现exec已经发送命令，而start随后把响应flush掉的窗口。
     */
    linux_uart_flush(channel->uart_fd);

    ret = linkg_thread_start(&channel->rx_thread);

    if (ret != 0)
    {
        pthread_mutex_unlock(&channel->lock);
        pthread_mutex_unlock(&channel->lifecycle_lock);

        return ret;
    }

    channel->started = true;

    pthread_mutex_unlock(&channel->lock);
    pthread_mutex_unlock(&channel->lifecycle_lock);

    LINKG_LOG_DEBUG("AT_CHANNEL: channel started, fd=%d", channel->uart_fd);

    return 0;
}

/**
 * @brief 在持有lifecycle_lock时停止AT通道。
 */
static int _at_channel_stop_lifecycle_locked(at_channel_t *channel)
{
    int ret;
    int stop_ret;

    ret = pthread_mutex_lock(&channel->lock);

    if (ret != 0)
    {
        return -ret;
    }

    channel->started = false;

    if (channel->transaction_state == AT_CHANNEL_TRANSACTION_WAITING)
    {
        _at_channel_complete_transaction_locked(channel, -ECANCELED);
    }
    else if (channel->transaction_state == AT_CHANNEL_TRANSACTION_DRAINING)
    {
        _at_channel_leave_draining_locked(channel);
        pthread_cond_broadcast(&channel->cond);
    }

    pthread_mutex_unlock(&channel->lock);

    stop_ret = 0;

    if (linkg_thread_is_started(&channel->rx_thread))
    {
        stop_ret = linkg_thread_stop(&channel->rx_thread);
    }

    if (stop_ret != 0)
    {
        return stop_ret;
    }

    ret = pthread_mutex_lock(&channel->lock);

    if (ret != 0)
    {
        return -ret;
    }

    channel->line_length = 0;
    channel->line_discarding = false;
    channel->drain_deadline_us = 0U;

    pthread_mutex_unlock(&channel->lock);

    return 0;
}

/**
 * @brief 停止AT通道接收线程并取消当前事务。
 */
int at_channel_stop(at_channel_t *channel)
{
    int ret;

    if (channel == NULL)
    {
        return -EINVAL;
    }

    // RX回调内禁止stop，否则底层线程会尝试join自身。
    if (_at_channel_is_rx_thread(channel))
    {
        return -EDEADLK;
    }

    ret = pthread_mutex_lock(&channel->lifecycle_lock);

    if (ret != 0)
    {
        return -ret;
    }

    ret = _at_channel_stop_lifecycle_locked(channel);

    pthread_mutex_unlock(&channel->lifecycle_lock);

    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_DEBUG("AT_CHANNEL: channel stopped");

    return 0;
}

/**
 * @brief 销毁AT通道并关闭UART设备。
 *
 * @note 调用前必须保证外部不会再发起新的channel访问。
 */
void at_channel_destroy(at_channel_t *channel)
{
    int ret;

    if (channel == NULL)
    {
        return;
    }

    /**
     * RX回调内destroy会释放当前线程仍在使用的channel，必须拒绝。
     * destroy是void接口，因此通过日志报告错误并保持对象有效。
     */
    if (_at_channel_is_rx_thread(channel))
    {
        LINKG_LOG_ERROR("AT_CHANNEL: destroy called from RX thread, action=reject");
        return;
    }

    ret = pthread_mutex_lock(&channel->lifecycle_lock);

    if (ret != 0)
    {
        LINKG_LOG_ERROR("AT_CHANNEL: lock lifecycle for destroy failed, error=%d", -ret);
        return;
    }

    ret = _at_channel_stop_lifecycle_locked(channel);

    if (ret != 0)
    {
        LINKG_LOG_ERROR("AT_CHANNEL: stop during destroy failed, error=%d", ret);
        pthread_mutex_unlock(&channel->lifecycle_lock);
        return;
    }

    // 等待可能仍在收尾的同步AT调用退出。
    ret = pthread_mutex_lock(&channel->exec_lock);

    if (ret != 0)
    {
        LINKG_LOG_ERROR("AT_CHANNEL: lock exec for destroy failed, error=%d", -ret);
        pthread_mutex_unlock(&channel->lifecycle_lock);
        return;
    }

    linkg_thread_deinit(&channel->rx_thread);

    if (channel->uart_fd >= 0)
    {
        (void)linux_uart_close(channel->uart_fd);
        channel->uart_fd = -1;
    }

    pthread_mutex_unlock(&channel->exec_lock);
    pthread_mutex_unlock(&channel->lifecycle_lock);

    pthread_cond_destroy(&channel->cond);
    pthread_mutex_destroy(&channel->lifecycle_lock);
    pthread_mutex_destroy(&channel->exec_lock);
    pthread_mutex_destroy(&channel->lock);

    free(channel);
}

/****************************** 命令接口 ******************************/

/**
 * @brief 同步执行一条AT命令并返回响应内容。
 */
int at_channel_exec(at_channel_t *channel, const char *command, const at_command_config_t *config, char *response, int response_size)
{
    struct timespec deadline;
    size_t command_length;
    int copy_ret;
    int result;
    int ret;

    if (channel == NULL)
    {
        return -EINVAL;
    }

    if (command == NULL)
    {
        return -EINVAL;
    }

    if (command[0] == '\0')
    {
        return -EINVAL;
    }

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (config->timeout_ms <= 0)
    {
        return -EINVAL;
    }

    if (config->expect_prefix != NULL && config->expect_prefix[0] == '\0')
    {
        return -EINVAL;
    }

    if (config->continuation_match == NULL)
    {
        if (config->continuation_max_lines != 0U)
        {
            return -EINVAL;
        }
    }
    else if (config->expect_prefix == NULL || config->continuation_max_lines == 0U)
    {
        return -EINVAL;
    }

    if (response == NULL && response_size != 0)
    {
        return -EINVAL;
    }

    if (response != NULL && response_size <= 0)
    {
        return -EINVAL;
    }

    command_length = strlen(command);

    if (command_length >= AT_CHANNEL_LINE_SIZE)
    {
        return -EMSGSIZE;
    }

    if (strchr(command, '\r') != NULL || strchr(command, '\n') != NULL)
    {
        return -EINVAL;
    }

    /**
     * URC回调运行于RX线程。
     * RX线程不能调用同步AT接口，否则会等待自己接收响应。
     */
    if (_at_channel_is_rx_thread(channel))
    {
        return -EDEADLK;
    }

    if (response != NULL)
    {
        response[0] = '\0';
    }

    // 保证整个系统同一时刻只有一个AT事务。
    ret = pthread_mutex_lock(&channel->exec_lock);

    if (ret != 0)
    {
        return -ret;
    }

    ret = pthread_mutex_lock(&channel->lock);

    if (ret != 0)
    {
        pthread_mutex_unlock(&channel->exec_lock);
        return -ret;
    }

    if (!channel->started)
    {
        pthread_mutex_unlock(&channel->lock);
        pthread_mutex_unlock(&channel->exec_lock);

        return -ENODEV;
    }

    /**
     * DRAINING由RX线程按deadline自动恢复。
     * 在恢复前拒绝发送下一条AT，避免迟到响应污染新事务。
     */
    if (channel->transaction_state == AT_CHANNEL_TRANSACTION_DRAINING)
    {
        pthread_mutex_unlock(&channel->lock);
        pthread_mutex_unlock(&channel->exec_lock);

        return -EAGAIN;
    }

    if (channel->transaction_state != AT_CHANNEL_TRANSACTION_IDLE)
    {
        pthread_mutex_unlock(&channel->lock);
        pthread_mutex_unlock(&channel->exec_lock);

        return -EBUSY;
    }

    memset(channel->response, 0, sizeof(channel->response));
    memcpy(channel->current_command, command, command_length + 1U);

    channel->current_config      = *config;
    channel->response_length     = 0;
    channel->transaction_result  = 0;
    channel->continuation_count  = 0U;
    channel->response_started    = false;
    channel->transaction_done    = false;
    channel->transaction_state   = AT_CHANNEL_TRANSACTION_WAITING;
    channel->drain_deadline_us   = 0U;

    /**
     * 在channel->lock保护下完成命令发送。
     * stop()必须等待发送结束后才能改变事务状态；
     * RX线程即使立即收到响应，也只能在发送完成并释放lock后处理。
     */
    ret = _at_channel_write_command(channel, command);

    if (ret != 0)
    {
        _at_channel_enter_draining_locked(channel);

        pthread_mutex_unlock(&channel->lock);

        _at_channel_wakeup_rx(channel);

        pthread_mutex_unlock(&channel->exec_lock);

        return ret;
    }

    pthread_mutex_unlock(&channel->lock);

    _at_channel_make_deadline(config->timeout_ms, &deadline);

    ret = pthread_mutex_lock(&channel->lock);

    if (ret != 0)
    {
        pthread_mutex_unlock(&channel->exec_lock);
        return -ret;
    }

    while (!channel->transaction_done &&
           channel->transaction_state == AT_CHANNEL_TRANSACTION_WAITING &&
           channel->started)
    {
        ret = pthread_cond_timedwait(&channel->cond, &channel->lock, &deadline);

        if (ret == ETIMEDOUT)
        {
            if (channel->transaction_state == AT_CHANNEL_TRANSACTION_WAITING &&
                !channel->transaction_done)
            {
                // 保留已经收到的部分响应，便于超时诊断。
                (void)_at_channel_copy_response_locked(channel, response, response_size);

                _at_channel_enter_draining_locked(channel);
            }

            pthread_mutex_unlock(&channel->lock);

            _at_channel_wakeup_rx(channel);

            pthread_mutex_unlock(&channel->exec_lock);

            return -ETIMEDOUT;
        }

        if (ret != 0)
        {
            if (channel->transaction_state == AT_CHANNEL_TRANSACTION_WAITING)
            {
                (void)_at_channel_copy_response_locked(channel, response, response_size);
                _at_channel_enter_draining_locked(channel);
            }

            pthread_mutex_unlock(&channel->lock);

            _at_channel_wakeup_rx(channel);

            pthread_mutex_unlock(&channel->exec_lock);

            return -ret;
        }
    }

    if (channel->transaction_done)
    {
        result = channel->transaction_result;
    }
    else if (!channel->started)
    {
        result = -ECANCELED;
    }
    else
    {
        result = -EIO;
    }

    copy_ret = _at_channel_copy_response_locked(channel, response, response_size);

    if (result == 0 && copy_ret != 0)
    {
        result = copy_ret;
    }

    memset(&channel->current_config, 0, sizeof(channel->current_config));
    memset(channel->current_command, 0, sizeof(channel->current_command));

    channel->continuation_count = 0U;
    channel->response_started = false;
    channel->transaction_done = false;

    pthread_mutex_unlock(&channel->lock);
    pthread_mutex_unlock(&channel->exec_lock);

    return result;
}

/****************************** URC接口 ******************************/

/**
 * @brief 注册或替换AT URC回调。
 *
 * @note context必须在回调可能执行期间保持有效。
 */
int at_channel_register_urc(at_channel_t *channel, at_urc_callback_t callback, void *context)
{
    int ret;

    if (channel == NULL)
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&channel->lock);

    if (ret != 0)
    {
        return -ret;
    }

    channel->urc_callback = callback;

    if (callback != NULL)
    {
        channel->urc_context = context;
    }
    else
    {
        channel->urc_context = NULL;
    }

    pthread_mutex_unlock(&channel->lock);

    return 0;
}

/**
 * @file cellular_monitor.c
 * @brief LinkG蜂窝网络异步事件监控实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-01
 */

#include "cellular_monitor.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "linkg_log.h"
#include "linkg_time.h"

/****************************** URC常量 ******************************/

#define CELLULAR_MONITOR_URC_SIM_STATUS       "+QSIMSTAT"       // SIM物理插拔状态URC名称
#define CELLULAR_MONITOR_URC_SIM_STATE        "+CPIN"           // SIM逻辑状态URC名称
#define CELLULAR_MONITOR_URC_EPS_REGISTRATION "+CEREG"          // EPS网络注册状态URC名称
#define CELLULAR_MONITOR_URC_5GS_REGISTRATION "+C5GREG"         // 5GS网络注册状态URC名称
#define CELLULAR_MONITOR_URC_RADIO            "+QCSQ"           // 服务网络和无线质量URC名称
#define CELLULAR_MONITOR_URC_PDP              "+CGEV"           // 分组域事件URC名称
#define CELLULAR_MONITOR_URC_NETDEV           "+QNETDEVSTATUS"  // USB网络设备状态URC名称
#define CELLULAR_MONITOR_URC_MODEM_FUNCTION   "+CFUN"           // Modem功能状态URC名称
#define CELLULAR_MONITOR_URC_POWERED_DOWN     "POWERED DOWN"    // Modem掉电URC文本

/****************************** 内部类型 ******************************/

typedef struct
{
    cellular_monitor_event_mask_t   mask;               // 当前URC对应的语义事件集合
    cellular_monitor_sim_presence_t sim_presence;       // 当前URC携带的SIM物理状态
    bool                            sim_presence_update; // 当前URC是否更新SIM物理状态
    bool                            recognized;          // 当前行是否属于已支持URC
    int                             parse_error;         // 当前已支持URC解析错误码
} cellular_monitor_decoded_urc_t;

typedef struct
{
    pthread_mutex_t                  lock;                    // 监控锁，保护生命周期、待处理事件和SIM物理状态
    at_channel_t                    *channel;                 // 借用AT通道，仅在start到stop期间有效
    cellular_monitor_event_mask_t    pending_mask;            // 等待network-cell Owner消费的事件集合
    cellular_monitor_sim_presence_t  sim_presence;            // 最近一次QSIMSTAT提供的SIM物理状态
    uint64_t                         generation;              // 已识别异步事件累计代数
    uint64_t                         updated_ms;              // 最近一次已识别异步事件时间
    uint64_t                         sim_presence_updated_ms; // 最近一次QSIMSTAT事件时间
    int                              event_fd;                // 通知network-cell Owner的eventfd
    bool                             sim_presence_valid;      // 最近一次QSIMSTAT是否提供确定插拔状态
    bool                             initialized;             // 监控模块是否已经初始化
    bool                             started;                 // URC回调是否已经注册并接受事件
} cellular_monitor_context_t;

/****************************** 全局上下文 ******************************/

static cellular_monitor_context_t g_cellular_monitor =
{
    .lock     = PTHREAD_MUTEX_INITIALIZER, // 监控锁静态初始化
    .event_fd = -1                        // eventfd尚未创建
};

/****************************** 文本辅助 ******************************/

/**
 * @brief 跳过字符串开头的空白字符。
 */
static const char *_cellular_monitor_skip_spaces(const char *text)
{
    if (text == NULL)
    {
        return NULL;
    }

    while (*text != '\0' && isspace((unsigned char)*text))
    {
        text++;
    }

    return text;
}

/**
 * @brief 判断一行文本去除首尾空白后是否与目标完全相同。
 */
static bool _cellular_monitor_line_equal(const char *line, const char *expected)
{
    const char *start;
    size_t       expected_length;
    size_t       line_length;

    if (line == NULL || expected == NULL)
    {
        return false;
    }

    start = _cellular_monitor_skip_spaces(line);
    if (start == NULL)
    {
        return false;
    }

    line_length = strlen(start);
    while (line_length > 0U && isspace((unsigned char)start[line_length - 1U]))
    {
        line_length--;
    }

    expected_length = strlen(expected);

    return line_length == expected_length && strncmp(start, expected, expected_length) == 0;
}

/**
 * @brief 匹配指定URC名称并返回冒号后的参数正文。
 */
static bool _cellular_monitor_match_urc(const char *line, const char *name, const char **payload)
{
    const char *cursor;
    size_t      name_length;

    if (line == NULL || name == NULL || payload == NULL)
    {
        return false;
    }

    cursor = _cellular_monitor_skip_spaces(line);
    if (cursor == NULL)
    {
        return false;
    }

    name_length = strlen(name);
    if (strncmp(cursor, name, name_length) != 0)
    {
        return false;
    }

    cursor += name_length;
    cursor = _cellular_monitor_skip_spaces(cursor);

    if (cursor == NULL || *cursor != ':')
    {
        return false;
    }

    cursor++;
    *payload = _cellular_monitor_skip_spaces(cursor);

    return true;
}

/**
 * @brief 解析QSIMSTAT URC携带的SIM物理插拔状态。
 */
static int _cellular_monitor_parse_sim_presence(const char *payload, cellular_monitor_sim_presence_t *presence, bool *valid)
{
    const char *cursor;
    char       *end;
    long        enabled;
    long        state;

    if (payload == NULL || presence == NULL || valid == NULL)
    {
        return -EINVAL;
    }

    *presence = CELLULAR_MONITOR_SIM_PRESENCE_UNKNOWN;
    *valid    = false;

    cursor = _cellular_monitor_skip_spaces(payload);
    if (cursor == NULL || *cursor == '\0')
    {
        return -EBADMSG;
    }

    errno   = 0;
    end     = NULL;
    enabled = strtol(cursor, &end, 10);

    if (errno == ERANGE || end == cursor)
    {
        return -EBADMSG;
    }

    cursor = _cellular_monitor_skip_spaces(end);
    if (cursor == NULL || *cursor != ',')
    {
        return -EBADMSG;
    }

    cursor++;
    cursor = _cellular_monitor_skip_spaces(cursor);
    if (cursor == NULL || *cursor == '\0')
    {
        return -EBADMSG;
    }

    errno = 0;
    end   = NULL;
    state = strtol(cursor, &end, 10);

    if (errno == ERANGE || end == cursor)
    {
        return -EBADMSG;
    }

    cursor = _cellular_monitor_skip_spaces(end);
    if (cursor == NULL || *cursor != '\0')
    {
        return -EBADMSG;
    }

    if (enabled != 0 && enabled != 1)
    {
        return -EBADMSG;
    }

    switch (state)
    {
        case 0:
            *presence = CELLULAR_MONITOR_SIM_PRESENCE_REMOVED;
            *valid    = true;
            return 0;

        case 1:
            *presence = CELLULAR_MONITOR_SIM_PRESENCE_INSERTED;
            *valid    = true;
            return 0;

        case 2:
            *presence = CELLULAR_MONITOR_SIM_PRESENCE_UNKNOWN;
            *valid    = false;
            return 0;

        default:
            return -EBADMSG;
    }
}

/****************************** 事件辅助 ******************************/

/**
 * @brief 清空一条已解析URC的临时结果。
 */
static void _cellular_monitor_decoded_urc_init(cellular_monitor_decoded_urc_t *decoded)
{
    if (decoded == NULL)
    {
        return;
    }

    memset(decoded, 0, sizeof(*decoded));
    decoded->sim_presence = CELLULAR_MONITOR_SIM_PRESENCE_UNKNOWN;
}

/**
 * @brief 将一条RG255 URC转换为蜂窝语义事件。
 */
static void _cellular_monitor_decode_urc(const char *line, cellular_monitor_decoded_urc_t *decoded)
{
    const char *payload;
    bool        presence_valid;
    int         ret;

    if (line == NULL || decoded == NULL)
    {
        return;
    }

    _cellular_monitor_decoded_urc_init(decoded);

    payload = NULL;
    if (_cellular_monitor_match_urc(line, CELLULAR_MONITOR_URC_SIM_STATUS, &payload))
    {
        decoded->recognized          = true;
        decoded->mask                = CELLULAR_MONITOR_EVENT_SIM_PRESENCE_CHANGED;
        decoded->sim_presence_update = true;

        presence_valid = false;
        ret = _cellular_monitor_parse_sim_presence(payload, &decoded->sim_presence, &presence_valid);
        if (ret != 0)
        {
            decoded->parse_error  = ret;
            decoded->sim_presence = CELLULAR_MONITOR_SIM_PRESENCE_UNKNOWN;
            return;
        }

        if (!presence_valid)
        {
            decoded->sim_presence = CELLULAR_MONITOR_SIM_PRESENCE_UNKNOWN;
        }

        return;
    }

    if (_cellular_monitor_match_urc(line, CELLULAR_MONITOR_URC_SIM_STATE, &payload))
    {
        decoded->recognized = true;
        decoded->mask       = CELLULAR_MONITOR_EVENT_SIM_STATE_CHANGED;
        return;
    }

    if (_cellular_monitor_match_urc(line, CELLULAR_MONITOR_URC_EPS_REGISTRATION, &payload) ||
        _cellular_monitor_match_urc(line, CELLULAR_MONITOR_URC_5GS_REGISTRATION, &payload))
    {
        decoded->recognized = true;
        decoded->mask       = CELLULAR_MONITOR_EVENT_REGISTRATION_CHANGED;
        return;
    }

    if (_cellular_monitor_match_urc(line, CELLULAR_MONITOR_URC_RADIO, &payload))
    {
        decoded->recognized = true;
        decoded->mask       = CELLULAR_MONITOR_EVENT_RADIO_CHANGED;
        return;
    }

    if (_cellular_monitor_match_urc(line, CELLULAR_MONITOR_URC_PDP, &payload))
    {
        decoded->recognized = true;
        decoded->mask       = CELLULAR_MONITOR_EVENT_PDP_CHANGED;
        return;
    }

    if (_cellular_monitor_match_urc(line, CELLULAR_MONITOR_URC_NETDEV, &payload))
    {
        decoded->recognized = true;
        decoded->mask       = CELLULAR_MONITOR_EVENT_NETDEV_CHANGED;
        return;
    }

    if (_cellular_monitor_match_urc(line, CELLULAR_MONITOR_URC_MODEM_FUNCTION, &payload))
    {
        decoded->recognized = true;
        decoded->mask       = CELLULAR_MONITOR_EVENT_MODEM_FUNCTION_CHANGED;
        return;
    }

    if (_cellular_monitor_line_equal(line, CELLULAR_MONITOR_URC_POWERED_DOWN))
    {
        decoded->recognized = true;
        decoded->mask       = CELLULAR_MONITOR_EVENT_MODEM_POWERED_DOWN;
    }
}

/**
 * @brief 清空Monitor运行期事件状态。
 *
 * @note 调用方必须持有g_cellular_monitor.lock。
 */
static void _cellular_monitor_reset_runtime_locked(void)
{
    g_cellular_monitor.pending_mask            = CELLULAR_MONITOR_EVENT_NONE;
    g_cellular_monitor.sim_presence            = CELLULAR_MONITOR_SIM_PRESENCE_UNKNOWN;
    g_cellular_monitor.generation              = 0U;
    g_cellular_monitor.updated_ms              = 0U;
    g_cellular_monitor.sim_presence_updated_ms = 0U;
    g_cellular_monitor.sim_presence_valid      = false;
}

/**
 * @brief 消费并清空Monitor eventfd中的全部通知计数。
 *
 * @note 调用方必须持有g_cellular_monitor.lock；eventfd为非阻塞描述符。
 */
static int _cellular_monitor_drain_event_fd_locked(void)
{
    eventfd_t value;
    int       ret;

    if (g_cellular_monitor.event_fd < 0)
    {
        return -ENODEV;
    }

    do
    {
        ret = eventfd_read(g_cellular_monitor.event_fd, &value);
    }
    while (ret != 0 && errno == EINTR);

    if (ret == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
    {
        return 0;
    }

    return -errno;
}

/**
 * @brief 将已解析URC合并到待处理事件并通知network-cell Owner。
 */
static void _cellular_monitor_publish_urc(at_channel_t *source, const cellular_monitor_decoded_urc_t *decoded, uint64_t now_ms)
{
    eventfd_t notify_value;
    int       notify_error;

    if (source == NULL || decoded == NULL || !decoded->recognized || decoded->mask == CELLULAR_MONITOR_EVENT_NONE)
    {
        return;
    }

    notify_error = 0;

    pthread_mutex_lock(&g_cellular_monitor.lock);

    if (!g_cellular_monitor.initialized ||
        !g_cellular_monitor.started ||
        g_cellular_monitor.channel != source ||
        g_cellular_monitor.event_fd < 0)
    {
        pthread_mutex_unlock(&g_cellular_monitor.lock);
        return;
    }

    g_cellular_monitor.pending_mask |= decoded->mask;

    if (decoded->sim_presence_update)
    {
        g_cellular_monitor.sim_presence            = decoded->sim_presence;
        g_cellular_monitor.sim_presence_valid      = decoded->parse_error == 0 &&
                                                     decoded->sim_presence != CELLULAR_MONITOR_SIM_PRESENCE_UNKNOWN;
        g_cellular_monitor.sim_presence_updated_ms = now_ms;
    }

    if (g_cellular_monitor.generation != UINT64_MAX)
    {
        g_cellular_monitor.generation++;
    }

    g_cellular_monitor.updated_ms = now_ms;

    notify_value = 1U;
    do
    {
        notify_error = eventfd_write(g_cellular_monitor.event_fd, notify_value);
    }
    while (notify_error != 0 && errno == EINTR);

    if (notify_error != 0)
    {
        notify_error = errno != 0 ? -errno : -EIO;
    }

    pthread_mutex_unlock(&g_cellular_monitor.lock);

    if (decoded->parse_error != 0)
    {
        LINKG_LOG_WARN("CELL-MONITOR: malformed supported URC, error=%d", decoded->parse_error);
    }

    if (notify_error != 0)
    {
        LINKG_LOG_ERROR("CELL-MONITOR: notify Owner failed, error=%d", notify_error);
    }
}

/**
 * @brief 接收AT RX线程递交的一条URC并快速转换为Monitor事件。
 *
 * @note 本函数运行于AT RX线程，禁止调用同步AT接口或执行阻塞操作。
 */
static void _cellular_monitor_urc_callback(const char *line, void *context)
{
    cellular_monitor_decoded_urc_t decoded;
    uint64_t                       now_ms;

    if (line == NULL || context == NULL)
    {
        return;
    }

    _cellular_monitor_decode_urc(line, &decoded);
    if (!decoded.recognized)
    {
        return;
    }

    now_ms = linkg_time_elapsed_ms();
    _cellular_monitor_publish_urc((at_channel_t *)context, &decoded, now_ms);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化蜂窝异步事件监控模块并创建Owner通知描述符。
 */
int cellular_monitor_init(void)
{
    int event_fd;
    int ret;

    pthread_mutex_lock(&g_cellular_monitor.lock);

    if (g_cellular_monitor.initialized)
    {
        ret = -EALREADY;
        goto unlock;
    }

    event_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0)
    {
        ret = -errno;
        goto unlock;
    }

    _cellular_monitor_reset_runtime_locked();

    g_cellular_monitor.channel     = NULL;
    g_cellular_monitor.event_fd    = event_fd;
    g_cellular_monitor.initialized = true;
    g_cellular_monitor.started     = false;
    ret                            = 0;

unlock:
    pthread_mutex_unlock(&g_cellular_monitor.lock);

    return ret;
}

/**
 * @brief 注册蜂窝模块唯一AT URC回调并开始接受异步事件。
 *
 * @note 本函数只建立URC接收通道，不发送任何AT配置命令；
 *       QSIMSTAT、CEREG、C5GREG、QCSQ和QNETDEVSTATUS等事件源由Owner配置。
 */
int cellular_monitor_start(at_channel_t *channel)
{
    int drain_ret;
    int ret;

    if (channel == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_cellular_monitor.lock);

    if (!g_cellular_monitor.initialized)
    {
        ret = -ENODEV;
        goto unlock;
    }

    if (g_cellular_monitor.started)
    {
        ret = -EALREADY;
        goto unlock;
    }

    drain_ret = _cellular_monitor_drain_event_fd_locked();
    if (drain_ret != 0)
    {
        ret = drain_ret;
        goto unlock;
    }

    _cellular_monitor_reset_runtime_locked();

    g_cellular_monitor.channel = channel;
    g_cellular_monitor.started = true;
    ret                        = 0;

unlock:
    pthread_mutex_unlock(&g_cellular_monitor.lock);

    if (ret != 0)
    {
        return ret;
    }

    ret = at_channel_register_urc(channel, _cellular_monitor_urc_callback, channel);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_cellular_monitor.lock);

        g_cellular_monitor.channel = NULL;
        g_cellular_monitor.started = false;
        _cellular_monitor_reset_runtime_locked();

        pthread_mutex_unlock(&g_cellular_monitor.lock);
    }

    return ret;
}

/**
 * @brief 停止接受蜂窝URC并清空尚未消费的异步事件。
 */
int cellular_monitor_stop(void)
{
    at_channel_t *channel;
    int           drain_ret;
    int           ret;

    pthread_mutex_lock(&g_cellular_monitor.lock);

    if (!g_cellular_monitor.initialized)
    {
        pthread_mutex_unlock(&g_cellular_monitor.lock);
        return 0;
    }

    if (!g_cellular_monitor.started)
    {
        pthread_mutex_unlock(&g_cellular_monitor.lock);
        return 0;
    }

    channel = g_cellular_monitor.channel;

    g_cellular_monitor.channel      = NULL;
    g_cellular_monitor.started      = false;
    g_cellular_monitor.pending_mask = CELLULAR_MONITOR_EVENT_NONE;

    pthread_mutex_unlock(&g_cellular_monitor.lock);

    ret = at_channel_register_urc(channel, NULL, NULL);

    pthread_mutex_lock(&g_cellular_monitor.lock);
    drain_ret = _cellular_monitor_drain_event_fd_locked();
    pthread_mutex_unlock(&g_cellular_monitor.lock);

    if (ret != 0)
    {
        return ret;
    }

    return drain_ret;
}

/**
 * @brief 反初始化蜂窝异步事件监控模块并释放Owner通知描述符。
 */
void cellular_monitor_deinit(void)
{
    int event_fd;
    int ret;

    ret = cellular_monitor_stop();
    if (ret != 0)
    {
        LINKG_LOG_WARN("CELL-MONITOR: stop during deinit failed, error=%d", ret);
    }

    pthread_mutex_lock(&g_cellular_monitor.lock);

    if (!g_cellular_monitor.initialized)
    {
        pthread_mutex_unlock(&g_cellular_monitor.lock);
        return;
    }

    event_fd = g_cellular_monitor.event_fd;

    g_cellular_monitor.channel     = NULL;
    g_cellular_monitor.event_fd    = -1;
    g_cellular_monitor.initialized = false;
    g_cellular_monitor.started     = false;
    _cellular_monitor_reset_runtime_locked();

    pthread_mutex_unlock(&g_cellular_monitor.lock);

    if (event_fd >= 0 && close(event_fd) != 0)
    {
        LINKG_LOG_WARN("CELL-MONITOR: close eventfd failed, error=%d", -errno);
    }
}

/****************************** 事件读取 ******************************/

/**
 * @brief 获取供network-cell Owner监听的Monitor事件描述符。
 */
int cellular_monitor_get_event_fd(void)
{
    int event_fd;

    pthread_mutex_lock(&g_cellular_monitor.lock);

    if (!g_cellular_monitor.initialized)
    {
        event_fd = -ENODEV;
    }
    else if (!g_cellular_monitor.started)
    {
        event_fd = -ENETDOWN;
    }
    else
    {
        event_fd = g_cellular_monitor.event_fd;
    }

    pthread_mutex_unlock(&g_cellular_monitor.lock);

    return event_fd;
}

/**
 * @brief 原子取得并清空当前已经合并的全部蜂窝异步事件。
 *
 * @note 多个同类URC在Owner消费前会合并为一个事件标志；
 *       SIM物理状态返回最近一次QSIMSTAT提供的最终状态。
 */
int cellular_monitor_take_events(cellular_monitor_events_t *events)
{
    int ret;

    if (events == NULL)
    {
        return -EINVAL;
    }

    memset(events, 0, sizeof(*events));
    events->sim_presence = CELLULAR_MONITOR_SIM_PRESENCE_UNKNOWN;

    pthread_mutex_lock(&g_cellular_monitor.lock);

    if (!g_cellular_monitor.initialized)
    {
        ret = -ENODEV;
        goto unlock;
    }

    if (!g_cellular_monitor.started)
    {
        ret = -ENETDOWN;
        goto unlock;
    }

    ret = _cellular_monitor_drain_event_fd_locked();
    if (ret != 0)
    {
        goto unlock;
    }

    events->mask                    = g_cellular_monitor.pending_mask;
    events->sim_presence            = g_cellular_monitor.sim_presence;
    events->generation              = g_cellular_monitor.generation;
    events->updated_ms              = g_cellular_monitor.updated_ms;
    events->sim_presence_updated_ms = g_cellular_monitor.sim_presence_updated_ms;
    events->sim_presence_valid      = g_cellular_monitor.sim_presence_valid;

    g_cellular_monitor.pending_mask = CELLULAR_MONITOR_EVENT_NONE;
    ret                             = 0;

unlock:
    pthread_mutex_unlock(&g_cellular_monitor.lock);

    return ret;
}

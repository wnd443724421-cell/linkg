/**
 * @file wifi_wpa_event.c
 * @brief LinkG wpa_supplicant连接事件实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-26
 */

#define _POSIX_C_SOURCE                      200809L                                                  // 启用POSIX.1-2008接口

#include "wifi_wpa_event.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "linkg_system_resources.h"

#include "wifi_service_internal.h"
#include "wpa_ctrl.h"

/****************************** 模块常量 ******************************/

#define WIFI_WPA_EVENT_CTRL_PATH             "/var/run/wpa_supplicant/" LINKG_RESOURCE_INTERFACE_WIFI // 控制套接字
#define WIFI_WPA_EVENT_STATUS_REPLY_SIZE_MAX 1024U                                                    // STATUS响应最大字节数
#define WIFI_WPA_EVENT_MESSAGE_SIZE_MAX      512U                                                     // 单条事件最大字节数

/****************************** 内部类型 ******************************/

struct wifi_wpa_event_listener
{
    struct wpa_ctrl *control;  // wpa_supplicant事件控制连接
    bool             attached; // 是否已成功发送ATTACH
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 将wpa_ctrl返回值转换为负errno。
 */
static int _wifi_wpa_event_map_control_error(int ret)
{
    if (ret == -2)
    {
        return -ETIMEDOUT;
    }

    if (ret < 0)
    {
        return -EIO;
    }

    return 0;
}

/**
 * @brief 释放监听器；正常路径先DETACH，异常路径直接关闭。
 */
static void _wifi_wpa_event_listener_release(wifi_wpa_event_listener_t *listener, bool detach)
{
    int ret;

    if (listener == NULL)
    {
        return;
    }

    if (listener->control != NULL)
    {
        if (detach && listener->attached)
        {
            /**
             * wpa_ctrl_request使用1000ms绝对截止时间。无论DETACH结果如何，
             * 本函数都继续关闭本地socket，正常退出不会重新引入长时间阻塞。
             */
            ret = wpa_ctrl_detach(listener->control);
            if (ret < 0)
            {
                ret = _wifi_wpa_event_map_control_error(ret);
                WIFI_SERVICE_WARN("detach wpa_supplicant event listener failed, error=%d", ret);
            }
            else
            {
                WIFI_SERVICE_DEBUG("wpa_supplicant event listener detached");
            }
        }

        listener->attached = false;
        wpa_ctrl_close(listener->control);
        listener->control = NULL;
    }

    free(listener);
}

/**
 * @brief 丢弃STATUS顺序屏障前已经排队的事件。
 */
static void _wifi_wpa_event_discard_message(char *message, size_t message_length)
{
    (void)message;
    (void)message_length;
}

/**
 * @brief 去除消息末尾的换行符。
 */
static void _wifi_wpa_event_trim_message(char *message, size_t *message_length)
{
    char tail;

    if (message == NULL)
    {
        return;
    }

    if (message_length == NULL)
    {
        return;
    }

    while (*message_length > 0U)
    {
        tail = message[*message_length - 1U];
        if (tail != '\n' && tail != '\r')
        {
            break;
        }

        (*message_length)--;
        message[*message_length] = '\0';
    }
}

/**
 * @brief 在指定控制连接上请求wpa_supplicant STATUS文本。
 */
static int _wifi_wpa_event_request_status(struct wpa_ctrl *control, char *reply, size_t reply_size)
{
    static const char command[] = "STATUS";

    size_t reply_length;
    int    ret;

    if (control == NULL)
    {
        return -EINVAL;
    }

    if (reply == NULL)
    {
        return -EINVAL;
    }

    if (reply_size < 2U)
    {
        return -EINVAL;
    }

    reply_length = reply_size - 1U;

    ret = wpa_ctrl_request(control,
                           command,
                           sizeof(command) - 1U,
                           reply,
                           &reply_length,
                           _wifi_wpa_event_discard_message);
    if (ret < 0)
    {
        return _wifi_wpa_event_map_control_error(ret);
    }

    if (reply_length >= reply_size)
    {
        return -EOVERFLOW;
    }

    reply[reply_length] = '\0';

    return 0;
}

/**
 * @brief 从STATUS文本解析STA连接状态。
 */
static int _wifi_wpa_event_parse_link_state(const char *status, wifi_wpa_link_state_t *state)
{
    static const char state_prefix[]    = "wpa_state=";
    static const char completed_value[] = "COMPLETED";

    const char *line;
    const char *value;
    size_t      line_length;
    size_t      prefix_length;
    size_t      value_length;

    if (status == NULL)
    {
        return -EINVAL;
    }

    if (state == NULL)
    {
        return -EINVAL;
    }

    *state = WIFI_WPA_LINK_STATE_DISCONNECTED;

    prefix_length = sizeof(state_prefix) - 1U;
    line = status;

    while (*line != '\0')
    {
        line_length = strcspn(line, "\r\n");

        if (line_length >= prefix_length &&
            strncmp(line, state_prefix, prefix_length) == 0)
        {
            value = line + prefix_length;
            value_length = line_length - prefix_length;

            if (value_length == sizeof(completed_value) - 1U &&
                strncmp(value, completed_value, value_length) == 0)
            {
                *state = WIFI_WPA_LINK_STATE_CONNECTED;
            }

            return 0;
        }

        line += line_length;

        while (*line == '\r' || *line == '\n')
        {
            line++;
        }
    }

    return -ENODATA;
}

/**
 * @brief 去除wpa_supplicant事件优先级前缀。
 */
static const char *_wifi_wpa_event_skip_priority(const char *message)
{
    const char *cursor;

    if (message == NULL)
    {
        return NULL;
    }

    if (message[0] != '<')
    {
        return message;
    }

    cursor = message + 1;
    if (!isdigit((unsigned char)*cursor))
    {
        return message;
    }

    while (isdigit((unsigned char)*cursor))
    {
        cursor++;
    }

    if (*cursor != '>')
    {
        return message;
    }

    return cursor + 1;
}

/**
 * @brief 判断事件文本是否匹配指定wpa事件前缀。
 */
static bool _wifi_wpa_event_matches(const char *message, const char *event_prefix)
{
    size_t prefix_length;
    char   next;

    if (message == NULL)
    {
        return false;
    }

    if (event_prefix == NULL)
    {
        return false;
    }

    prefix_length = strlen(event_prefix);

    while (prefix_length > 0U && event_prefix[prefix_length - 1U] == ' ')
    {
        prefix_length--;
    }

    if (prefix_length == 0U)
    {
        return false;
    }

    if (strncmp(message, event_prefix, prefix_length) != 0)
    {
        return false;
    }

    next = message[prefix_length];

    if (next == '\0')
    {
        return true;
    }

    return next == ' ' || next == '\r' || next == '\n';
}

/**
 * @brief 解析wpa_supplicant连接事件。
 */
static wifi_wpa_event_t _wifi_wpa_event_parse(const char *message)
{
    if (message == NULL)
    {
        return WIFI_WPA_EVENT_UNKNOWN;
    }

    if (_wifi_wpa_event_matches(message, WPA_EVENT_CONNECTED))
    {
        return WIFI_WPA_EVENT_CONNECTED;
    }

    if (_wifi_wpa_event_matches(message, WPA_EVENT_DISCONNECTED))
    {
        return WIFI_WPA_EVENT_DISCONNECTED;
    }

    if (_wifi_wpa_event_matches(message, WPA_EVENT_SCAN_STARTED))
    {
        return WIFI_WPA_EVENT_SCAN_STARTED;
    }

    if (_wifi_wpa_event_matches(message, WPA_EVENT_SCAN_RESULTS))
    {
        return WIFI_WPA_EVENT_SCAN_RESULTS;
    }

    if (_wifi_wpa_event_matches(message, WPA_EVENT_SCAN_FAILED))
    {
        return WIFI_WPA_EVENT_SCAN_FAILED;
    }

    if (_wifi_wpa_event_matches(message, WPA_EVENT_NETWORK_NOT_FOUND))
    {
        return WIFI_WPA_EVENT_NETWORK_NOT_FOUND;
    }

    if (_wifi_wpa_event_matches(message, WPA_EVENT_TERMINATING))
    {
        return WIFI_WPA_EVENT_TERMINATING;
    }

    return WIFI_WPA_EVENT_UNKNOWN;
}

/****************************** 监听器管理 ******************************/

/**
 * @brief 创建并附加wpa_supplicant事件监听器。
 */
int wifi_wpa_event_listener_open(wifi_wpa_event_listener_t **listener)
{
    wifi_wpa_event_listener_t *instance;
    int                        ret;

    if (listener == NULL)
    {
        return -EINVAL;
    }

    *listener = NULL;

    instance = calloc(1U, sizeof(*instance));
    if (instance == NULL)
    {
        return -ENOMEM;
    }

    instance->control = wpa_ctrl_open(WIFI_WPA_EVENT_CTRL_PATH);
    if (instance->control == NULL)
    {
        WIFI_SERVICE_DEBUG("open wpa_supplicant event control socket failed, path=%s",
                           WIFI_WPA_EVENT_CTRL_PATH);

        free(instance);
        return -ENOTCONN;
    }

    ret = wpa_ctrl_attach(instance->control);
    if (ret < 0)
    {
        ret = _wifi_wpa_event_map_control_error(ret);

        WIFI_SERVICE_DEBUG("attach wpa_supplicant event listener failed, error=%d", ret);

        wpa_ctrl_close(instance->control);
        instance->control = NULL;

        free(instance);
        return ret;
    }

    instance->attached = true;
    *listener = instance;

    WIFI_SERVICE_DEBUG("wpa_supplicant event listener attached, path=%s",
                       WIFI_WPA_EVENT_CTRL_PATH);

    return 0;
}

/**
 * @brief 关闭并释放wpa_supplicant事件监听器。
 */
void wifi_wpa_event_listener_close(wifi_wpa_event_listener_t *listener)
{
    _wifi_wpa_event_listener_release(listener, true);
}

/**
 * @brief 异常关闭并释放监听器，不向失效的wpa_supplicant发送DETACH。
 */
void wifi_wpa_event_listener_abort(wifi_wpa_event_listener_t *listener)
{
    _wifi_wpa_event_listener_release(listener, false);
}

/****************************** 状态查询 ******************************/

/**
 * @brief 在已附加监听器上查询当前连接状态并建立事件顺序屏障。
 */
int wifi_wpa_event_listener_get_link_state(wifi_wpa_event_listener_t *listener, wifi_wpa_link_state_t *state)
{
    char reply[WIFI_WPA_EVENT_STATUS_REPLY_SIZE_MAX];
    int  ret;

    if (listener == NULL)
    {
        return -EINVAL;
    }

    if (state == NULL)
    {
        return -EINVAL;
    }

    if (listener->control == NULL)
    {
        return -ENOTCONN;
    }

    *state = WIFI_WPA_LINK_STATE_DISCONNECTED;

    memset(reply, 0, sizeof(reply));

    ret = _wifi_wpa_event_request_status(listener->control, reply, sizeof(reply));
    if (ret != 0)
    {
        return ret;
    }

    return _wifi_wpa_event_parse_link_state(reply, state);
}

/**
 * @brief 获取事件监听器文件描述符。
 */
int wifi_wpa_event_listener_get_fd(const wifi_wpa_event_listener_t *listener)
{
    int event_fd;

    if (listener == NULL)
    {
        return -EINVAL;
    }

    if (listener->control == NULL)
    {
        return -ENOTCONN;
    }

    event_fd = wpa_ctrl_get_fd(listener->control);
    if (event_fd < 0)
    {
        return -EBADF;
    }

    return event_fd;
}

/****************************** 事件接收 ******************************/

/**
 * @brief 接收并解析一条wpa_supplicant事件。
 */
int wifi_wpa_event_listener_receive(wifi_wpa_event_listener_t *listener, wifi_wpa_event_t *event)
{
    char        message[WIFI_WPA_EVENT_MESSAGE_SIZE_MAX];
    const char *payload;
    size_t      message_length;
    int         ret;

    if (listener == NULL)
    {
        return -EINVAL;
    }

    if (event == NULL)
    {
        return -EINVAL;
    }

    if (listener->control == NULL)
    {
        return -ENOTCONN;
    }

    *event = WIFI_WPA_EVENT_UNKNOWN;

    memset(message, 0, sizeof(message));
    message_length = sizeof(message) - 1U;

    ret = wpa_ctrl_recv(listener->control, message, &message_length);
    if (ret < 0)
    {
        WIFI_SERVICE_DEBUG("receive wpa_supplicant event failed, error=%d", ret);
        return -EIO;
    }

    if (message_length >= sizeof(message))
    {
        return -EOVERFLOW;
    }

    message[message_length] = '\0';
    _wifi_wpa_event_trim_message(message, &message_length);

    payload = _wifi_wpa_event_skip_priority(message);
    if (payload == NULL)
    {
        return -EPROTO;
    }

    *event = _wifi_wpa_event_parse(payload);

    return 0;
}


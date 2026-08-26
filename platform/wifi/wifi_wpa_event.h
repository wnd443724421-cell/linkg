/**
 * @file wifi_wpa_event.h
 * @brief LinkG wpa_supplicant连接事件接口
 */

#ifndef WIFI_WPA_EVENT_H
#define WIFI_WPA_EVENT_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 连接状态 ******************************/

typedef enum
{
    WIFI_WPA_LINK_STATE_DISCONNECTED = 0, // STA尚未建立数据连接
    WIFI_WPA_LINK_STATE_CONNECTED         // STA已经建立数据连接
} wifi_wpa_link_state_t;

/****************************** 事件类型 ******************************/

typedef enum
{
    WIFI_WPA_EVENT_UNKNOWN = 0,       // 未识别或不关心的事件
    WIFI_WPA_EVENT_CONNECTED,         // STA连接已经建立
    WIFI_WPA_EVENT_DISCONNECTED,      // STA连接已经断开
    WIFI_WPA_EVENT_SCAN_STARTED,      // wpa_supplicant扫描已经开始
    WIFI_WPA_EVENT_SCAN_RESULTS,      // 扫描完成且结果可用
    WIFI_WPA_EVENT_SCAN_FAILED,       // 扫描执行失败
    WIFI_WPA_EVENT_NETWORK_NOT_FOUND, // 扫描完成但未找到已配置网络
    WIFI_WPA_EVENT_TERMINATING        // wpa_supplicant正在退出
} wifi_wpa_event_t;

/****************************** 监听器类型 ******************************/

typedef struct wifi_wpa_event_listener wifi_wpa_event_listener_t; // wpa_supplicant事件监听器

/****************************** 监听器管理 ******************************/

int  wifi_wpa_event_listener_open(wifi_wpa_event_listener_t **listener);
void wifi_wpa_event_listener_close(wifi_wpa_event_listener_t *listener);
void wifi_wpa_event_listener_abort(wifi_wpa_event_listener_t *listener);

/****************************** 状态查询 ******************************/

int wifi_wpa_event_listener_get_link_state(wifi_wpa_event_listener_t *listener, wifi_wpa_link_state_t *state);
int wifi_wpa_event_listener_get_fd(const wifi_wpa_event_listener_t *listener);

/****************************** 事件接收 ******************************/

int wifi_wpa_event_listener_receive(wifi_wpa_event_listener_t *listener, wifi_wpa_event_t *event);

#ifdef __cplusplus
}
#endif

#endif

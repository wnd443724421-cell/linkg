/**
 * @file wifi_flowctrl.h
 * @brief LinkG Wi-Fi发送流控控制器接口
 */

#ifndef WIFI_FLOWCTRL_H
#define WIFI_FLOWCTRL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 前置声明 ******************************/

typedef struct linkg_wifi_flowctrl linkg_wifi_flowctrl_t;

/****************************** 流控采样 ******************************/

typedef struct
{
    uint32_t be_queue_length;     // 驱动BE发送队列长度
    uint32_t vi_queue_length;     // 驱动VI发送队列长度
    uint32_t vo_queue_length;     // 驱动VO发送队列长度
    uint32_t flowctrl_off_count;  // FLOWCTRL_OFF累计触发次数
    uint32_t normal_batch_limit;  // 当前VIDEO/DATA单次最大准入数量
    uint32_t requested_count;     // 本次请求发送数量
    uint32_t allowed_count;       // 本次允许发送数量
    uint64_t read_us;             // 驱动流控状态读取耗时，单位微秒
    uint8_t  tx_allowed;          // 驱动当前发送允许状态
    bool     status_valid;        // 本次驱动状态是否有效
    bool     backed_off;          // 本次是否触发批次退避
    bool     waterline_blocked;   // 本次是否因队列水位阻塞
} linkg_wifi_flowctrl_sample_t;

/****************************** 生命周期 ******************************/

linkg_wifi_flowctrl_t *linkg_wifi_flowctrl_create(void);
void                   linkg_wifi_flowctrl_destroy(linkg_wifi_flowctrl_t *flowctrl);
int                    linkg_wifi_flowctrl_start(linkg_wifi_flowctrl_t *flowctrl);
void                   linkg_wifi_flowctrl_stop(linkg_wifi_flowctrl_t *flowctrl);

/****************************** 发送准入 ******************************/

int linkg_wifi_flowctrl_admit(linkg_wifi_flowctrl_t *flowctrl, uint32_t requested_count, uint32_t *allowed_count, linkg_wifi_flowctrl_sample_t *sample);

#ifdef __cplusplus
}
#endif

#endif

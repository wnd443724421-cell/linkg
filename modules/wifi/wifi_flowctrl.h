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
    uint32_t be_queue_length;     // 驱动BE发送队列长度，仅用于统计
    uint32_t vi_queue_length;     // 驱动VI发送队列长度，仅用于统计
    uint32_t vo_queue_length;     // 驱动VO发送队列长度，仅用于统计
    uint32_t flowctrl_off_count;  // FLOWCTRL_OFF累计触发次数
    uint64_t read_us;             // 驱动状态读取耗时，单位微秒
    bool     driver_tx_allowed;   // 驱动原始发送允许状态
    bool     new_off_detected;    // 本次是否观察到新的FLOWCTRL_OFF
    bool     submit_allowed;      // 本次是否最终允许提交
    bool     status_valid;        // 本次驱动状态是否有效
} linkg_wifi_flowctrl_sample_t;

/****************************** 生命周期 ******************************/

linkg_wifi_flowctrl_t *linkg_wifi_flowctrl_create(void);
void                   linkg_wifi_flowctrl_destroy(linkg_wifi_flowctrl_t *flowctrl);
int                    linkg_wifi_flowctrl_start(linkg_wifi_flowctrl_t *flowctrl);
void                   linkg_wifi_flowctrl_stop(linkg_wifi_flowctrl_t *flowctrl);

/****************************** 发送准入 ******************************/

/**
 * @brief 检查当前Wi-Fi是否允许提交一个新的发送批次。
 */
int linkg_wifi_flowctrl_check(linkg_wifi_flowctrl_t *flowctrl,
                              bool *submit_allowed,
                              linkg_wifi_flowctrl_sample_t *sample);

#ifdef __cplusplus
}
#endif

#endif

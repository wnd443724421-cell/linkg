/**
 * @file wifi_nb_report.h
 * @brief LinkG HI1105窄带附加状态接口
 */

#ifndef WIFI_NB_REPORT_H
#define WIFI_NB_REPORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 状态信息 ******************************/

typedef struct
{
    uint8_t  rate_level;         // 当前窄带速率档位，范围0～5
    uint16_t chip_temperature_c; // HI1105芯片温度，单位摄氏度
} wifi_nb_report_status_t;

/****************************** 生命周期 ******************************/

int  wifi_nb_report_init(void);
int  wifi_nb_report_start(void);
int  wifi_nb_report_stop(void);
void wifi_nb_report_deinit(void);

/****************************** 状态读取 ******************************/

int wifi_nb_report_get_status(wifi_nb_report_status_t *status);

#ifdef __cplusplus
}
#endif

#endif

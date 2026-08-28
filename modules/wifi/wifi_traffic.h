/**
 * @file wifi_traffic.h
 * @brief LinkG Wi-Fi内部业务类别定义
 */

#ifndef WIFI_TRAFFIC_H
#define WIFI_TRAFFIC_H

/****************************** 业务类别 ******************************/

typedef enum
{
    LINKG_WIFI_TRAFFIC_REALTIME = 0, // 实时低延时数据，映射WMM AC_VO
    LINKG_WIFI_TRAFFIC_VIDEO,        // 视频实时媒体数据，映射WMM AC_VI
    LINKG_WIFI_TRAFFIC_DATA,         // 普通数据，映射WMM AC_BE
    LINKG_WIFI_TRAFFIC_COUNT         // Wi-Fi业务类别数量
} linkg_wifi_traffic_class_t;

#endif

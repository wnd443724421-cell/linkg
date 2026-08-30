/**
 * @file discovery_wifi.h
 * @brief LinkG设备发现Wi-Fi Channel内部接口
 */

#ifndef DISCOVERY_WIFI_H
#define DISCOVERY_WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_discovery_wifi_init(void);
int linkg_discovery_wifi_start(void);
int linkg_discovery_wifi_stop(void);
int linkg_discovery_wifi_deinit(void);

#ifdef __cplusplus
}
#endif

#endif

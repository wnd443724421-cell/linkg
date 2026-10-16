/**
 * @file linkg_wifi_hardware.h
 * @brief GPIO HWID到Wi-Fi硬件变体的唯一映射接口
 */
#ifndef LINKG_WIFI_HARDWARE_H
#define LINKG_WIFI_HARDWARE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    LINKG_WIFI_HARDWARE_UNKNOWN = 0, // 未识别的GPIO编码
    LINKG_WIFI_HARDWARE_DOMESTIC,    // 国产PA
    LINKG_WIFI_HARDWARE_IMPORTED     // 进口PA
} linkg_wifi_hardware_type_t;

linkg_wifi_hardware_type_t linkg_wifi_hardware_from_id(uint8_t hardware_id);
const char *linkg_wifi_hardware_type_string(linkg_wifi_hardware_type_t type);

#ifdef __cplusplus
}
#endif

#endif // LINKG_WIFI_HARDWARE_H

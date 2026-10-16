/**
 * @file linkg_wifi_hardware.c
 * @brief LinkG Wi-Fi硬件变体统一识别实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-16
 */

#include "linkg_wifi_hardware.h"

#include "wifi_platform_internal.h"

/****************************** 硬件识别 ******************************/

/**
 * @brief 将GPIO硬件编码转换为统一Wi-Fi硬件类型。
 */
linkg_wifi_hardware_type_t linkg_wifi_hardware_from_id(uint8_t hardware_id)
{
    if (hardware_id == WIFI_DRIVER_HARDWARE_ID_DOMESTIC)
    {
        return LINKG_WIFI_HARDWARE_DOMESTIC;
    }

    if (hardware_id == WIFI_DRIVER_HARDWARE_ID_IMPORTED)
    {
        return LINKG_WIFI_HARDWARE_IMPORTED;
    }

#if WIFI_DRIVER_ENABLE_LQ10_COMPAT
    if (hardware_id == WIFI_DRIVER_HARDWARE_ID_LQ10)
    {
        return LINKG_WIFI_HARDWARE_IMPORTED;
    }
#endif

    return LINKG_WIFI_HARDWARE_UNKNOWN;
}

/**
 * @brief 获取Wi-Fi硬件类型字符串。
 */
const char *linkg_wifi_hardware_type_string(linkg_wifi_hardware_type_t type)
{
    switch (type)
    {
        case LINKG_WIFI_HARDWARE_DOMESTIC:
            return "domestic";

        case LINKG_WIFI_HARDWARE_IMPORTED:
            return "imported";

        default:
            return "unknown";
    }
}

/**
 * @file linkg_wifi_ops.c
 * @brief LinkG Wi-Fi通用校验接口实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-26
 */

#include "linkg_wifi_ops.h"

#include <stddef.h>
#include <string.h>

#include "wifi_platform_internal.h"

/****************************** 通用定义 ******************************/

/**
 * @brief SSID和密码允许的ASCII特殊字符集合。
 *
 * @note 完整允许字符包括英文字母、数字以及：
 *       !@#$%^&*()-_=+.,:;?
 */
#define LINKG_ARRAY_SIZE(array)          (sizeof(array) / sizeof((array)[0])) // 获取数组元素数量
#define LINKG_WIFI_ALLOWED_SPECIAL_CHARS "!@#$%^&*()-_=+.,:;?"                // Wi-Fi允许的特殊字符

/****************************** 信道列表 ******************************/

static const uint16_t g_wifi_narrow_channels[] =
{
#if LINKG_WIFI_ENABLE_EXTENDED_CHANNELS
     36U,  40U,  44U,  48U,
     52U,  56U,  60U,  64U,
    100U, 104U, 108U, 112U,
    116U, 120U, 124U, 128U,
    132U, 136U, 140U, 144U,
#endif
    149U, 153U, 157U, 161U,
    165U,
#if LINKG_WIFI_ENABLE_EXTENDED_CHANNELS
    184U, 188U, 192U, 196U
#endif
};

static const uint16_t g_wifi_wide_channels[] =
{
#if LINKG_WIFI_ENABLE_EXTENDED_CHANNELS
     36U,  40U,  44U,  48U,
     52U,  56U,  60U,  64U,
    100U, 104U, 108U, 112U,
    116U, 120U, 124U, 128U,
    132U, 136U, 140U, 144U,
#endif
    149U, 153U, 157U, 161U,
    165U,
#if LINKG_WIFI_ENABLE_EXTENDED_CHANNELS
    184U, 188U, 192U, 196U
#endif
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查字符串长度是否合法。
 */
static bool _wifi_string_length_valid(const char *string, size_t capacity, size_t min_length, size_t max_length)
{
    const char *end;
    size_t      length;

    if (string == NULL || capacity == 0U || min_length > max_length)
    {
        return false;
    }

    end = memchr(string, '\0', capacity);
    if (end == NULL)
    {
        return false;
    }

    length = (size_t)(end - string);

    return length >= min_length && length <= max_length;
}

/**
 * @brief 检查SSID或密码中的单个ASCII字符是否合法。
 */
static bool _wifi_ascii_character_valid(char character)
{
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') ||
           strchr(LINKG_WIFI_ALLOWED_SPECIAL_CHARS, character) != NULL;
}

/**
 * @brief 检查ASCII字符串长度和字符内容是否合法。
 */
static bool _wifi_ascii_content_valid(const char *string, size_t capacity, size_t min_length, size_t max_length)
{
    size_t index;

    if (!_wifi_string_length_valid(string, capacity, min_length, max_length))
    {
        return false;
    }

    for (index = 0U; string[index] != '\0'; index++)
    {
        if (!_wifi_ascii_character_valid(string[index]))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 检查信道是否位于指定信道列表中。
 */
static bool _wifi_channel_in_list(uint16_t channel, const uint16_t *channels, size_t count)
{
    size_t index;

    if (channels == NULL || count == 0U)
    {
        return false;
    }

    for (index = 0U; index < count; index++)
    {
        if (channels[index] == channel)
        {
            return true;
        }
    }

    return false;
}

/****************************** 基础校验 ******************************/

/**
 * @brief 检查Wi-Fi SSID是否合法。
 *
 * @note 长度必须为1～32个ASCII字符；允许英文字母、数字及特殊字符
 *       !@#$%^&*()-_=+.,:;?，不允许空格、中文、引号、斜杠、
 *       反斜杠及控制字符。
 */
bool linkg_wifi_ssid_valid(const char *ssid)
{
    return _wifi_ascii_content_valid(ssid,
                                     LINKG_WIFI_SSID_MAX + 1U,
                                     1U,
                                     LINKG_WIFI_SSID_MAX);
}

/**
 * @brief 检查Wi-Fi密码是否合法。
 *
 * @note OPEN模式密码必须为空字符串；WPA2-PSK密码长度必须为8～63个
 *       ASCII字符，不支持64个十六进制字符形式的原始PSK。
 */
bool linkg_wifi_password_valid(linkg_wifi_security_t security, const char *password)
{
    switch (security)
    {
        case LINKG_WIFI_SECURITY_OPEN:
            return _wifi_string_length_valid(password,
                                             LINKG_WIFI_PASSWORD_MAX + 1U,
                                             0U,
                                             0U);

        case LINKG_WIFI_SECURITY_WPA2_PSK:
            return _wifi_ascii_content_valid(password,
                                             LINKG_WIFI_PASSWORD_MAX + 1U,
                                             LINKG_WIFI_WPA2_PASSWORD_MIN,
                                             LINKG_WIFI_PASSWORD_MAX);

        case LINKG_WIFI_SECURITY_UNKNOWN:
        default:
            return false;
    }
}

/**
 * @brief 检查Wi-Fi信道是否属于当前工作模式允许的信道。
 */
bool linkg_wifi_channel_valid(linkg_wifi_work_mode_t work_mode, uint16_t channel)
{
    switch (work_mode)
    {
        case LINKG_WIFI_WORK_MODE_NARROW:
            return _wifi_channel_in_list(channel,
                                         g_wifi_narrow_channels,
                                         LINKG_ARRAY_SIZE(g_wifi_narrow_channels));

        case LINKG_WIFI_WORK_MODE_WIDE:
            return _wifi_channel_in_list(channel,
                                         g_wifi_wide_channels,
                                         LINKG_ARRAY_SIZE(g_wifi_wide_channels));

        case LINKG_WIFI_WORK_MODE_UNKNOWN:
        default:
            return false;
    }
}

/**
 * @brief 检查Wi-Fi安全模式是否有效。
 */
bool linkg_wifi_security_valid(linkg_wifi_security_t security)
{
    return security == LINKG_WIFI_SECURITY_OPEN ||
           security == LINKG_WIFI_SECURITY_WPA2_PSK;
}

/****************************** 窄带校验 ******************************/

/**
 * @brief 检查Wi-Fi窄带速率控制模式是否有效。
 */
bool linkg_wifi_narrow_mode_valid(linkg_wifi_narrow_mode_t mode)
{
    return mode == LINKG_WIFI_NARROW_MODE_FIXED ||
           mode == LINKG_WIFI_NARROW_MODE_ADAPTIVE;
}

/**
 * @brief 检查Wi-Fi窄带带宽是否有效。
 */
bool linkg_wifi_narrow_bandwidth_valid(uint16_t bandwidth)
{
    return bandwidth == LINKG_WIFI_NARROW_BANDWIDTH_MHZ;
}

/**
 * @brief 检查Wi-Fi窄带速率档位是否有效。
 */
bool linkg_wifi_narrow_rate_valid(uint16_t rate)
{
    return rate <= LINKG_WIFI_NARROW_RATE_MAX;
}

/****************************** 宽带校验 ******************************/

/**
 * @brief 检查Wi-Fi宽带带宽是否有效。
 */
bool linkg_wifi_wide_bandwidth_valid(linkg_wifi_wide_bandwidth_t bandwidth)
{
    return bandwidth == LINKG_WIFI_WIDE_BANDWIDTH_20_MHZ ||
           bandwidth == LINKG_WIFI_WIDE_BANDWIDTH_40_MHZ ||
           bandwidth == LINKG_WIFI_WIDE_BANDWIDTH_80_MHZ;
}

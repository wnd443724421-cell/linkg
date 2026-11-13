/**
 * @file wifi_scan_result.c
 * @brief LinkG扫描结果白名单解析与有效采样累计
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */

#include "wifi_scan.h"

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/****************************** 内部类型 ******************************/

typedef struct
{
    unsigned long long  active_ms; // 本轮驻留时间
    unsigned long long  busy_ms;   // 本轮繁忙时间
    int                 noise;     // 本轮噪声
    bool                active;    // 驻留字段有效
    bool                busy;      // 繁忙字段有效
    bool                has_noise; // 噪声字段有效
    bool                duplicate; // 同频率重复结果，不累计
    bool                seen;      // 本轮包含该频率
} wifi_scan_sample_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 查找用户开放频率，其他频率和缓存结果不得进入统计。
 */
static int _wifi_scan_frequency_index(uint32_t frequency)
{
    static const uint32_t frequencies[] = {5745U, 5765U, 5785U, 5805U, 5825U};
    uint32_t index;

    for (index = 0U; index < LINKG_WIFI_SCAN_CHANNEL_COUNT; index++)
    {
        if (frequency == frequencies[index])
        {
            return (int)index;
        }
    }
    return -1;
}

/****************************** 结果初始化 ******************************/

/**
 * @brief 初始化五个用户信道，不将未测得字段填成有效的零。
 */
void wifi_scan_result_init(linkg_wifi_scan_result_t *result)
{
    uint32_t index;

    if (result == NULL)
    {
        return;
    }
    memset(result, 0, sizeof(*result));
    for (index = 0U; index < LINKG_WIFI_SCAN_CHANNEL_COUNT; index++)
    {
        result->channels[index].channel = 149U + 4U * index;
        result->channels[index].frequency_mhz = 5745U + 20U * index;
    }
}

/****************************** Survey统计 ******************************/

/**
 * @brief 解析一轮survey；每频率最多累计一次，拒绝无效时长及重复结果。
 */
int wifi_scan_parse_survey(linkg_wifi_scan_result_t *result, const char *text)
{
    wifi_scan_sample_t         samples[LINKG_WIFI_SCAN_CHANNEL_COUNT];
    linkg_wifi_scan_channel_t *channel;
    wifi_scan_sample_t        *sample;
    char                      *copy;
    char                      *save;
    char                      *line;
    unsigned int               frequency;
    unsigned long long         value;
    double                     percent;
    uint32_t                   index;
    int                        current;
    int                        noise;
    int                        valid;

    if (result == NULL || text == NULL || strlen(text) > 32768U)
    {
        return -EINVAL;
    }
    copy = strdup(text);
    if (copy == NULL)
    {
        return -ENOMEM;
    }
    memset(samples, 0, sizeof(samples));
    current = -1;
    save = NULL;
    for (line = strtok_r(copy, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save))
    {
        while (*line == ' ' || *line == '\t')
        {
            line++;
        }
        if (sscanf(line, "frequency: %u MHz", &frequency) == 1)
        {
            current = _wifi_scan_frequency_index(frequency);
            if (current >= 0)
            {
                samples[current].duplicate = samples[current].seen;
                samples[current].seen = true;
            }
            continue;
        }
        if (current < 0)
        {
            continue;
        }
        sample = &samples[current];
        if (sscanf(line, "noise: %d dBm", &noise) == 1)
        {
            sample->has_noise = noise >= -127 && noise < 0;
            sample->noise = noise;
        }
        else if (sscanf(line, "channel active time: %llu ms", &value) == 1)
        {
            sample->active = value > 0U && value <= 60000U;
            sample->active_ms = value;
        }
        else if (sscanf(line, "channel busy time: %llu ms", &value) == 1)
        {
            sample->busy = value <= 60000U;
            sample->busy_ms = value;
        }
    }
    free(copy);

    valid = 0;
    for (index = 0U; index < LINKG_WIFI_SCAN_CHANNEL_COUNT; index++)
    {
        sample = &samples[index];
        channel = &result->channels[index];
        if (!sample->seen || sample->duplicate)
        {
            continue;
        }
        if (sample->has_noise && channel->noise_samples < LINKG_WIFI_SCAN_ROUND_COUNT)
        {
            channel->noise_dbm[channel->noise_samples++] = sample->noise;
        }
        if (!sample->active || !sample->busy || sample->busy_ms > sample->active_ms ||
            channel->samples >= LINKG_WIFI_SCAN_ROUND_COUNT)
        {
            continue;
        }
        channel->samples++;
        channel->active_ms += sample->active_ms;
        channel->busy_ms += sample->busy_ms;
        percent = 100.0 * (double)sample->busy_ms / (double)sample->active_ms;
        if (percent > channel->peak_busy_percent)
        {
            channel->peak_busy_percent = percent;
        }
        valid++;
    }
    return valid > 0 ? 0 : -ENODATA;
}

/****************************** AP辅助信息 ******************************/

/**
 * @brief 解析本次SCAN_RESULTS，只保留白名单频率并按BSSID去重。
 */
int wifi_scan_parse_bss(linkg_wifi_scan_result_t *result, const char *text)
{
    linkg_wifi_scan_bss_t  entry;
    char                  *copy;
    char                  *save;
    char                  *line;
    char                  *fields[5];
    char                  *cursor;
    uint32_t               index;
    uint32_t               field;
    unsigned int           octets[6];
    int                    frequency;
    int                    signal;

    if (result == NULL || text == NULL || strlen(text) > 32768U)
    {
        return -EINVAL;
    }
    copy = strdup(text);
    if (copy == NULL)
    {
        return -ENOMEM;
    }
    save = NULL;
    for (line = strtok_r(copy, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save))
    {
        cursor = line;
        for (field = 0U; field < 4U; field++)
        {
            fields[field] = cursor;
            cursor = strchr(cursor, '\t');
            if (cursor == NULL)
            {
                break;
            }
            *cursor++ = '\0';
        }
        if (field != 4U)
        {
            continue;
        }
        fields[4] = cursor;
        if (strlen(fields[0]) != 17U ||
            sscanf(fields[0], "%2x:%2x:%2x:%2x:%2x:%2x", &octets[0], &octets[1], &octets[2], &octets[3], &octets[4], &octets[5]) != 6 ||
            sscanf(fields[1], "%d", &frequency) != 1 ||
            sscanf(fields[2], "%d", &signal) != 1 || signal < -127 || signal >= 0 ||
            _wifi_scan_frequency_index((uint32_t)frequency) < 0)
        {
            continue;
        }
        for (field = 0U; field < 17U; field++)
        {
            if (field % 3U == 2U ? fields[0][field] != ':' : !isxdigit((unsigned char)fields[0][field]))
            {
                break;
            }
            fields[0][field] = (char)tolower((unsigned char)fields[0][field]);
        }
        if (field != 17U)
        {
            continue;
        }
        memset(&entry, 0, sizeof(entry));
        (void)snprintf(entry.bssid, sizeof(entry.bssid), "%s", fields[0]);
        (void)snprintf(entry.ssid, sizeof(entry.ssid), "%s", fields[4]);
        entry.frequency_mhz = (uint32_t)frequency;
        entry.signal_dbm = signal;
        for (index = 0U; index < result->bss_count; index++)
        {
            if (strcmp(result->bss[index].bssid, entry.bssid) == 0)
            {
                break;
            }
        }
        if (index < LINKG_WIFI_SCAN_BSS_MAX)
        {
            result->bss[index] = entry;
            if (index == result->bss_count)
            {
                result->bss_count++;
            }
        }
        else
        {
            result->bss_truncated = true;
        }
    }
    free(copy);
    return 0;
}

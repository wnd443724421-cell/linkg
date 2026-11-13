/**
 * @file test_wifi_scan.c
 * @brief 信道统计与附近网络解析回归
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */
#include "wifi_scan.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief 验证加权计数、零繁忙和缺失数据。
 */
int main(void)
{
    linkg_wifi_scan_result_t  result;
    char                      row[256];

    wifi_scan_result_init(&result);
    assert(result.channels[4].channel == 165U);
    assert(wifi_scan_parse_survey(&result, "frequency: 5745 MHz\nnoise: -96 dBm\nchannel active time: 35 ms\nchannel busy time: 7 ms\n") == 0);
    assert(wifi_scan_parse_survey(&result, "frequency: 5745 MHz\nnoise: -98 dBm\nchannel active time: 65 ms\nchannel busy time: 13 ms\nfrequency: 5765 MHz\nnoise: -99 dBm\nchannel active time: 35 ms\nchannel busy time: 0 ms\n") == 0);
    assert(result.channels[0].active_ms == 100U && result.channels[0].busy_ms == 20U);
    assert(result.channels[0].peak_busy_percent == 20.0);
    assert(result.channels[1].samples == 1U && result.channels[1].busy_ms == 0U);
    assert(result.channels[2].samples == 0U);
    assert(wifi_scan_parse_survey(&result, "frequency: 5785 MHz\nchannel active time: 0 ms\nchannel busy time: 0 ms\nfrequency: 5805 MHz\nchannel active time: 35 ms\nchannel busy time: 36 ms\n") == -ENODATA);
    assert(wifi_scan_parse_survey(&result, "frequency: 5825 MHz\nchannel active time: 35 ms\nchannel busy time: 0 ms\nfrequency: 5825 MHz\nchannel active time: 35 ms\nchannel busy time: 0 ms\n") == -ENODATA);
    assert(wifi_scan_parse_survey(&result, "frequency: 4920 MHz\nchannel active time: 35 ms\nchannel busy time: 0 ms\n") == -ENODATA);
    assert(wifi_scan_parse_survey(&result, "") == -ENODATA);
    for (unsigned i = 0; i < 30U; i++)
    {
        (void)wifi_scan_parse_survey(&result, "frequency: 5745 MHz\nnoise: -96 dBm\nchannel active time: 35 ms\nchannel busy time: 7 ms\n");
    }
    assert(result.channels[0].samples == LINKG_WIFI_SCAN_ROUND_COUNT);
    assert(result.channels[0].noise_samples == LINKG_WIFI_SCAN_ROUND_COUNT);
    wifi_scan_result_init(&result);
    assert(wifi_scan_parse_bss(&result, "AA:BB:CC:DD:EE:01\t5745\t-51\t[ESS]\tLinkG\naa:bb:cc:dd:ee:02\t5765\t-69\t[ESS]\tLinkG\naa:bb:cc:dd:ee:03\t5805\t-67\t[ESS]\t\n") == 0);
    assert(result.bss_count == 3U && result.bss[2].ssid[0] == '\0');
    assert(wifi_scan_parse_bss(&result, "aa:bb:cc:dd:ee:01\t5745\t-71\t[WPA2]\tLinkG\n") == 0);
    assert(result.bss_count == 3U && result.bss[0].signal_dbm == -71);
    assert(wifi_scan_parse_bss(&result, "aa:bb:cc:dd:ee:04\t2412\t-41\t[ESS]\tother\naa:bb:cc:dd:ee:05\t5745\t0\t[ESS]\tinvalid\naa:bb:cc:dd:ee:0z\t5745\t-60\t[ESS]\tinvalid\n") == 0);
    assert(result.bss_count == 3U);
    for (unsigned i = 0; i < LINKG_WIFI_SCAN_BSS_MAX + 3U; i++)
    {
        (void)snprintf(row, sizeof(row), "02:00:00:00:%02x:%02x\t5745\t-60\t[ESS]\tcap\n", i / 256U, i % 256U);
        assert(wifi_scan_parse_bss(&result, row) == 0);
    }
    assert(result.bss_count == LINKG_WIFI_SCAN_BSS_MAX && result.bss_truncated);
    puts("scan parser regression passed");
    return 0;
}

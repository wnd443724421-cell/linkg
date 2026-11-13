/**
 * @file test_scan_bss_iteration.c
 * @brief 原始BSS遍历与有效网络容量独立限制回归
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */
#include "../../platform/wifi/wifi_scan.c"

#include <assert.h>

static unsigned raw_count;
static unsigned foreign_count;

/**
 * @brief 模拟WPA混合频率缓存，目标网络位于原始索引128之后。
 */
int wpa_ctrl_request(struct wpa_ctrl *control, const char *command, size_t command_length, char *reply, size_t *reply_length, void (*callback)(char *, size_t))
{
    unsigned index;
    int length;

    (void)control;
    (void)command_length;
    (void)callback;
    assert(sscanf(command, "BSS %u", &index) == 1);
    if (index >= raw_count)
    {
        memcpy(reply, "FAIL", 4U);
        *reply_length = 4U;
        return 0;
    }
    length = snprintf(reply, *reply_length, "bssid=02:00:00:00:%02x:%02x\nfreq=%u\nlevel=-60\nssid=network-%u\n", index / 256U, index % 256U, index < foreign_count ? 2412U : 5745U, index);
    assert(length > 0 && (size_t)length < *reply_length);
    *reply_length = (size_t)length;
    return 0;
}

/**
 * @brief 提供最小任务继续条件，未使用的硬件路径由链接器剔除。
 */
bool linkg_thread_is_running(const linkg_thread_t *thread)
{
    return atomic_load(&thread->running);
}

/**
 * @brief 验证非目标记录不消耗有效容量，且原始遍历仍然有界。
 */
int main(void)
{
    linkg_wifi_scan_result_t result;
    linkg_thread_t thread;
    char text[WIFI_SCAN_TEXT_MAX];

    memset(&thread, 0, sizeof(thread));
    atomic_init(&thread.running, true);
    wifi_scan_result_init(&result);
    foreign_count = 140U;
    raw_count = 142U;
    assert(_wifi_scan_read_bss(&thread, (struct wpa_ctrl *)&thread, &result, text) == 0);
    assert(result.bss_count == 2U && !result.bss_truncated);
    wifi_scan_result_init(&result);
    raw_count = 270U;
    assert(_wifi_scan_read_bss(&thread, (struct wpa_ctrl *)&thread, &result, text) == 0);
    assert(result.bss_count == 128U && result.bss_truncated);
    wifi_scan_result_init(&result);
    raw_count = WIFI_SCAN_BSS_ITERATION_MAX + 2U;
    foreign_count = raw_count;
    assert(_wifi_scan_read_bss(&thread, (struct wpa_ctrl *)&thread, &result, text) == 0);
    assert(result.bss_count == 0U && result.bss_truncated);
    atomic_store(&thread.running, false);
    assert(_wifi_scan_read_bss(&thread, (struct wpa_ctrl *)&thread, &result, text) == -ECANCELED);
    return 0;
}

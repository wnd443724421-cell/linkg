/**
 * @file hostapd_test.c
 * @brief 验证实际窄带运行配置生成只限制Wi-Fi窄带STA数量。
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-17
 */

#include <assert.h>
#include <stdarg.h>

#include "../../platform/wifi/wifi_hostapd.c"
#include "linkg_log.h"

/****************************** 外部适配 ******************************/

/**
 * @brief 配置文本测试不访问实际日志设备。
 */
void linkg_log_write_internal(linkg_log_level_t level, const char *file, int line, const char *format, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

/****************************** 模板回归 ******************************/

/**
 * @brief 缺项、重复键和旧值均生成唯一的窄带限制，宽带文本保持不变。
 */
static void _test_station_limit(const char *source)
{
    linkg_wifi_config_t config = {0};
    char  *content;
    size_t length;

    content = strdup(source);
    assert(content != NULL);
    length                    = strlen(content);
    config.wideband.work_mode = LINKG_WIFI_WORK_MODE_WIDE;
    assert(_wifi_hostapd_update_station_limit(&content, &length, &config) == 0);
    assert(strcmp(content, source) == 0);
    config.wideband.work_mode = LINKG_WIFI_WORK_MODE_NARROW;
    assert(_wifi_hostapd_update_station_limit(&content, &length, &config) == 0);
    const char *limit = strstr(content, "\nmax_num_sta=1\n");
    if (limit == NULL && strncmp(content, "max_num_sta=1\n", strlen("max_num_sta=1\n")) == 0)
    {
        limit = content;
    }
    assert(limit != NULL);
    const char *first_key = strstr(content, "max_num_sta=");
    assert(first_key != NULL && strstr(first_key + 1, "max_num_sta=") == NULL);
    assert(length == strlen(content));
    free(content);
}

/**
 * @brief 验证模板回归，也支持只读校验实际固件模板生成的内存文本。
 */
int main(int argc, char **argv)
{
    _test_station_limit("interface=wlan0\nmax_num_sta=16\nssid=LinkG\n");
    _test_station_limit("interface=wlan0\nssid=LinkG");
    _test_station_limit("max_num_sta=16\ninterface=wlan0\nmax_num_sta=10\n");
    for (int index = 1; index < argc; index++)
    {
        FILE *file = fopen(argv[index], "rb");
        assert(file != NULL);
        assert(fseek(file, 0L, SEEK_END) == 0);
        long size = ftell(file);
        assert(size >= 0L && size <= 65536L);
        assert(fseek(file, 0L, SEEK_SET) == 0);
        char *source = malloc((size_t)size + 1U);
        assert(source != NULL && fread(source, 1U, (size_t)size, file) == (size_t)size);
        source[size] = '\0';
        assert(fclose(file) == 0);
        _test_station_limit(source);
        free(source);
    }
    puts("PASS hostapd runtime station limit / narrow one STA / wide unchanged / missing and duplicate keys / actual templates when supplied");
    return 0;
}

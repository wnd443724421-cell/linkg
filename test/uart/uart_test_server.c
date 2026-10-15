/**
 * @file uart_test_server.c
 * @brief UART真实线程、Socket及Web控制服务器集成测试宿主
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-15
 */

#define _GNU_SOURCE

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "linkg_config.h"
#include "linkg_file.h"
#include "linkg_log.h"
#include "linkg_thread.h"
#include "linkg_time.h"
#include "linkg_uart.h"
#include "linkg_uart_manager.h"
#include "web_internal.h"

/****************************** 测试上下文 ******************************/

static char        g_test_slave[3][128];  // 三个PTY从设备节点
static int         g_test_master[3];      // 测试端持有的PTY主设备
static const char *g_test_storage;        // 临时配置文件路径
static atomic_int  g_test_fail_init;      // 线程初始化失败注入次数
static atomic_int  g_test_fail_start;     // 线程启动失败注入次数
static atomic_int  g_test_fail_stop;      // 线程停止失败注入次数
static atomic_int  g_test_fail_open;      // UART打开失败注入次数
static atomic_int  g_test_fail_before;    // 原子保存写入前失败次数
static atomic_int  g_test_fail_after;     // 原子保存完成后失败次数
static atomic_int  g_test_fail_gpio_rx;   // UART接收门控失败注入次数
static atomic_int  g_test_fail_gpio_sbus; // SBUS模式控制失败注入次数
static atomic_int  g_test_delay_ms;       // 一次持久化延时
static atomic_int  g_test_worker_starts;  // Worker累计启动次数
static atomic_int  g_test_workers;        // 尚未join的Worker数量
static atomic_int  g_test_peak_workers;   // 同时存在的Worker峰值
static atomic_bool g_test_rx_enabled;     // 模拟PD21接收门控
static atomic_bool g_test_sbus_enabled;   // 模拟PD16模式选择

int __real_linux_uart_open(const char *dev, const uart_config_t *config);
int __real_linkg_thread_init(linkg_thread_t *thread, const char *name, linkg_thread_func_t function, void *data);
int __real_linkg_thread_start(linkg_thread_t *thread);
int __real_linkg_thread_stop(linkg_thread_t *thread);
int __real_linkg_file_read_all(const char *path, size_t max_length, char **data, size_t *length);
int __real_linkg_file_write_all_atomic(const char *path, const void *data, size_t length, mode_t mode);

/****************************** 故障注入 ******************************/

/**
 * @brief 原子消费一次故障，测试指令可与后台线程并发。
 */
static bool _uart_test_fail(atomic_int *counter)
{
    int expected = atomic_load(counter);

    while (expected > 0)
    {
        if (atomic_compare_exchange_weak(counter, &expected, expected - 1))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 将板级UART映射至PTY，物理电平和SBUS奇偶校验另做实板验证。
 */
int __wrap_linux_uart_open(const char *dev, const uart_config_t *config)
{
    uart_config_t physical = *config;
    int           index;

    if (_uart_test_fail(&g_test_fail_open))
    {
        return -ENODEV;
    }
    for (index = 0; index < 3; index++)
    {
        if (strcmp(dev, linkg_uart_manager_device_path((uint8_t)index)) == 0)
        {
            physical.baudrate  = 115200;
            physical.data_bits = 8;
            physical.parity    = 'n';
            physical.stop_bits = 1;
            physical.exclusive = false;
            return __real_linux_uart_open(g_test_slave[index], &physical);
        }
    }

    return -EINVAL;
}

/**
 * @brief 注入UART对象初始化失败，其他线程保持真实实现。
 */
int __wrap_linkg_thread_init(linkg_thread_t *thread, const char *name, linkg_thread_func_t function, void *data)
{
    if (strcmp(name, "linkg-uart") == 0 && _uart_test_fail(&g_test_fail_init))
    {
        return -ENOMEM;
    }

    return __real_linkg_thread_init(thread, name, function, data);
}

/**
 * @brief 统计实际Worker创建数量并注入线程启动错误。
 */
int __wrap_linkg_thread_start(linkg_thread_t *thread)
{
    int ret;
    int count;

    if (strcmp(thread->name, "linkg-uart") == 0 && _uart_test_fail(&g_test_fail_start))
    {
        return -EAGAIN;
    }
    ret = __real_linkg_thread_start(thread);
    if (ret == 0 && strcmp(thread->name, "linkg-uart") == 0)
    {
        atomic_fetch_add(&g_test_worker_starts, 1);
        count = atomic_fetch_add(&g_test_workers, 1) + 1;
        if (count > atomic_load(&g_test_peak_workers))
        {
            atomic_store(&g_test_peak_workers, count);
        }
    }

    return ret;
}

/**
 * @brief 注入无法确认线程退出的错误，成功join后更新Worker计数。
 */
int __wrap_linkg_thread_stop(linkg_thread_t *thread)
{
    bool started = linkg_thread_is_started(thread);
    int  ret;

    if (strcmp(thread->name, "linkg-uart") == 0 && _uart_test_fail(&g_test_fail_stop))
    {
        return -EIO;
    }
    ret = __real_linkg_thread_stop(thread);
    if (ret == 0 && started && strcmp(thread->name, "linkg-uart") == 0)
    {
        atomic_fetch_sub(&g_test_workers, 1);
    }

    return ret;
}

/**
 * @brief 把正式配置路径重定向到测试目录，不接触设备配置。
 */
int __wrap_linkg_file_read_all(const char *path, size_t max_length, char **data, size_t *length)
{
    const char *target = path;

    if (strcmp(path, LINKG_CONFIG_USER_PATH) == 0)
    {
        target = access(g_test_storage, F_OK) == 0 ? g_test_storage : "config/default_config_sta.json";
    }
    else if (strcmp(path, LINKG_CONFIG_FACTORY_PATH) == 0)
    {
        target = "config/default_config_sta.json";
    }

    return __real_linkg_file_read_all(target, max_length, data, length);
}

/**
 * @brief 使用真实原子写入并注入rename前后错误，覆盖磁盘已经改变的失败路径。
 */
int __wrap_linkg_file_write_all_atomic(const char *path, const void *data, size_t length, mode_t mode)
{
    int delay;
    int ret;

    assert(strcmp(path, LINKG_CONFIG_USER_PATH) == 0);
    delay = atomic_exchange(&g_test_delay_ms, 0);
    if (delay > 0)
    {
        linkg_time_sleep_ms((uint32_t)delay);
    }
    if (_uart_test_fail(&g_test_fail_before))
    {
        return -EIO;
    }
    ret = __real_linkg_file_write_all_atomic(g_test_storage, data, length, mode);
    if (ret == 0 && _uart_test_fail(&g_test_fail_after))
    {
        return -EIO;
    }

    return ret;
}

/****************************** 硬件隔离 ******************************/

/**
 * @brief 模拟串口接收门控，测试不操作VM或板卡GPIO。
 */
int linkg_gpio_manager_set_uart_rx_enabled(bool enabled)
{
    if (_uart_test_fail(&g_test_fail_gpio_rx))
    {
        return -EIO;
    }

    atomic_store(&g_test_rx_enabled, enabled);
    return 0;
}

/**
 * @brief 切换MCU模式时接收门控必须关闭。
 */
int linkg_gpio_manager_set_sbus_enabled(bool enabled)
{
    assert(!atomic_load(&g_test_rx_enabled));
    if (_uart_test_fail(&g_test_fail_gpio_sbus))
    {
        return -EIO;
    }

    atomic_store(&g_test_sbus_enabled, enabled);
    return 0;
}

/**
 * @brief 测试日志不写入设备日志目录。
 */
void linkg_log_write_internal(linkg_log_level_t level, const char *file, int line, const char *format, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

/****************************** 非UART请求替身 ******************************/

/**
 * @brief 非UART模块不参与测试进程，只证明分发和Web线程继续响应。
 */
static int _uart_test_other(int cmd, const cJSON *param, char **response)
{
    (void)param;
    return _linkg_web_response_success(cmd, NULL, "test stub", response);
}

// 这些替身不表示真实Wi-Fi、5G、Switch已在VM运行。

/**
 * @brief 返回固定头部测试替身响应。
 */
int _linkg_web_handler_header_get(const cJSON *param, char **response)
{
    return _uart_test_other(1001, param, response);
}

/**
 * @brief 返回概览页面测试替身响应。
 */
int _linkg_web_handler_overview_get(const cJSON *param, char **response)
{
    return _uart_test_other(1002, param, response);
}

/**
 * @brief 返回Switch状态测试替身响应。
 */
int _linkg_web_handler_switch_status_get(const cJSON *param, char **response)
{
    return _uart_test_other(1003, param, response);
}

/**
 * @brief 返回Wi-Fi配置读取测试替身响应。
 */
int _linkg_web_handler_wifi_config_get(const cJSON *param, char **response)
{
    return _uart_test_other(1004, param, response);
}

/**
 * @brief 返回Wi-Fi配置写入测试替身响应。
 */
int _linkg_web_handler_wifi_config_set(const cJSON *param, char **response)
{
    return _uart_test_other(1005, param, response);
}

/**
 * @brief 返回Wi-Fi状态测试替身响应。
 */
int _linkg_web_handler_wifi_status_get(const cJSON *param, char **response)
{
    return _uart_test_other(1006, param, response);
}

/**
 * @brief 返回蜂窝网络配置读取测试替身响应。
 */
int _linkg_web_handler_cellular_config_get(const cJSON *param, char **response)
{
    return _uart_test_other(1007, param, response);
}

/**
 * @brief 返回蜂窝网络配置写入测试替身响应。
 */
int _linkg_web_handler_cellular_config_set(const cJSON *param, char **response)
{
    return _uart_test_other(1008, param, response);
}

/**
 * @brief 返回蜂窝网络状态测试替身响应。
 */
int _linkg_web_handler_cellular_status_get(const cJSON *param, char **response)
{
    return _uart_test_other(1009, param, response);
}

/**
 * @brief 返回流量配置读取测试替身响应。
 */
int _linkg_web_handler_traffic_config_get(const cJSON *param, char **response)
{
    return _uart_test_other(1010, param, response);
}

/**
 * @brief 返回流量配置写入测试替身响应。
 */
int _linkg_web_handler_traffic_config_set(const cJSON *param, char **response)
{
    return _uart_test_other(1011, param, response);
}

/****************************** 测试控制 ******************************/

/**
 * @brief 统计本进程描述符，重复重载后应回到稳定数量。
 */
static int _uart_test_fd_count(void)
{
    DIR           *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    int            count = 0;

    assert(directory != NULL);
    while ((entry = readdir(directory)) != NULL)
    {
        if (entry->d_name[0] != '.')
        {
            count++;
        }
    }
    closedir(directory);

    return count;
}

/**
 * @brief 通过独立stdin控制故障与PTY，不给正式Web增加测试命令。
 */
int main(int argc, char **argv)
{
    linkg_config_t snapshot;
    char           line[256];
    char           command[64];
    int            value;
    int            slave;
    int            index;

    assert(argc == 2);
    g_test_storage = argv[1];
    for (index = 0; index < 3; index++)
    {
        assert(openpty(&g_test_master[index], &slave, g_test_slave[index], NULL, NULL) == 0);
        close(slave);
        assert(fcntl(g_test_master[index], F_SETFL, O_NONBLOCK) == 0);
    }
    assert(linkg_time_init() == 0);
    assert(linkg_config_load() == 0);
    assert(linkg_config_create_snapshot(&snapshot) == 0);
    memset(&snapshot.uart, 0, sizeof(snapshot.uart));
    assert(linkg_config_replace(&snapshot) == 0);
    assert(linkg_config_save(&snapshot) == 0);
    assert(linkg_uart_manager_init() == 0);
    assert(linkg_uart_manager_start() == 0);
    assert(_linkg_web_server_init() == 0);
    assert(_linkg_web_server_start() == 0);
    puts("READY");
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        value = 0;
        sscanf(line, "%63s %d", command, &value);
        if (strcmp(command, "quit") == 0)
        {
            break;
        }
        if (strcmp(command, "init") == 0)
        {
            atomic_store(&g_test_fail_init, value);
        }
        if (strcmp(command, "start") == 0)
        {
            atomic_store(&g_test_fail_start, value);
        }
        if (strcmp(command, "stop") == 0)
        {
            atomic_store(&g_test_fail_stop, value);
        }
        if (strcmp(command, "open") == 0)
        {
            atomic_store(&g_test_fail_open, value);
        }
        if (strcmp(command, "before") == 0)
        {
            atomic_store(&g_test_fail_before, value);
        }
        if (strcmp(command, "after") == 0)
        {
            atomic_store(&g_test_fail_after, value);
        }
        if (strcmp(command, "gpio-rx") == 0)
        {
            atomic_store(&g_test_fail_gpio_rx, value);
        }
        if (strcmp(command, "gpio-sbus") == 0)
        {
            atomic_store(&g_test_fail_gpio_sbus, value);
        }
        if (strcmp(command, "delay") == 0)
        {
            atomic_store(&g_test_delay_ms, value);
        }
        if (strcmp(command, "service-stop") == 0)
        {
            assert(linkg_uart_manager_stop() == 0);
            puts("OK");
        }
        else if (strcmp(command, "service-start") == 0)
        {
            assert(linkg_uart_manager_start() == 0);
            puts("OK");
        }
        else if (strcmp(command, "counts") == 0)
        {
            printf("%d %d %d %d\n", _uart_test_fd_count(), atomic_load(&g_test_worker_starts),
                   atomic_load(&g_test_workers), atomic_load(&g_test_peak_workers));
        }
        else if (strcmp(command, "write") == 0)
        {
            assert(value >= 0 && value < 3);
            printf("%zd\n", write(g_test_master[value], "linkg-test", 10U));
        }
        else if (strcmp(command, "read") == 0)
        {
            char    data[128];
            ssize_t count;

            assert(value >= 0 && value < 3);
            count = read(g_test_master[value], data, sizeof(data) - 1U);
            if (count > 0)
            {
                data[count] = '\0';
                printf("%s\n", data);
            }
            else
            {
                puts("EMPTY");
            }
        }
        else if (strcmp(command, "recover") == 0)
        {
            assert(linkg_uart_manager_stop() == 0);
            assert(linkg_uart_manager_deinit() == 0);
            assert(linkg_uart_manager_init() == 0);
            assert(linkg_uart_manager_start() == 0);
            puts("OK");
        }
        else
        {
            puts("OK");
        }
        fflush(stdout);
    }

    assert(_linkg_web_server_stop() == 0);
    assert(_linkg_web_server_deinit() == 0);
    assert(linkg_uart_manager_stop() == 0);
    assert(linkg_uart_manager_deinit() == 0);
    linkg_config_deinit();
    for (index = 0; index < 3; index++)
    {
        close(g_test_master[index]);
    }

    return 0;
}

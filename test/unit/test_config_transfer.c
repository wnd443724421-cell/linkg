/**
 * @file test_config_transfer.c
 * @brief 配置文件往返、导入故障和应答后重载测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */

#include "test_assert.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config_internal.h"
#include "linkg_config.h"
#include "linkg_file.h"
#include "linkg_json.h"
#include "linkg_lifecycle.h"
#include "web_internal.h"
#include "uart_internal.h"
#include <pthread.h>

/****************************** 测试状态 ******************************/

static char g_test_file[256];        // 所有配置文件IO重定向到临时目录
static int  g_test_write_fail = 0;   // 单次写失败：1写前，2原子替换后
static char g_test_instance[64] = "transfer-test-instance"; // 测试服务代次
static int  g_test_restart_failures;
static uint64_t g_test_now = 1U;
static int  g_test_restarts = 0;     // 替身生命周期接收到的重载次数

int __real_linkg_file_write_all_atomic(const char *path, const void *data, size_t length, mode_t mode);
int __real_linkg_file_read_all(const char *path, size_t max_length, char **data, size_t *length);

/**
 * @brief 保持真实原子写实现，仅重定向目标路径并注入持久化错误。
 */
int __wrap_linkg_file_write_all_atomic(const char *path, const void *data, size_t length, mode_t mode)
{
    int fail = g_test_write_fail;
    int ret;

    (void)path;
    g_test_write_fail = 0;
    if (fail == 1)
    {
        return -EIO;
    }

    ret = __real_linkg_file_write_all_atomic(g_test_file, data, length, mode);
    return fail == 2 && ret == 0 ? -EIO : ret;
}

/**
 * @brief 配置加载读取真实测试文件，不访问主机/app。
 */
int __wrap_linkg_file_read_all(const char *path, size_t max_length, char **data, size_t *length)
{
    (void)path;
    return __real_linkg_file_read_all(g_test_file, max_length, data, length);
}

/**
 * @brief 提供固定服务实例，验证过期页面保护。
 */
const char *_linkg_web_network_basic_instance(void)
{
    return g_test_instance;
}

/**
 * @brief 记录重载请求，不停止测试进程。
 */
int linkg_lifecycle_request_exit(linkg_exit_action_t action)
{
    if (action != LINKG_EXIT_ACTION_RESTART_SERVICES)
    {
        return -EINVAL;
    }
    g_test_restarts++;
    if (g_test_restart_failures > 0)
    {
        g_test_restart_failures--;
        return -EIO;
    }
    return 0;
}

/****************************** 确定性异步调度替身 ******************************/

linkg_uart_manager_context_t g_uart_manager;
static void *(*g_delayed_function)(void *);
static void *g_delayed_argument;
static bool  g_delay_create;
static bool  g_fail_create;

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument);

/**
 * @brief 延迟UART后台执行，精确复现已接受但尚未取得事务锁的窗口。
 */
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument)
{
    if (g_fail_create)
    {
        return EAGAIN;
    }
    if (g_delay_create)
    {
        g_delayed_function = function;
        g_delayed_argument = argument;
        return 0;
    }
    return __real_pthread_create(thread, attributes, function, argument);
}

/**
 * @brief 为重载重试提供可控单调时间，不引入秒级测试等待。
 */
uint64_t __wrap_linkg_time_elapsed_ms(void)
{
    return g_test_now;
}

/**
 * @brief 提供UART时间及硬件生命周期替身，仍运行真实配置应用代码。
 */
uint64_t uart_now_ms(void)
{
    return g_test_now;
}

int uart_manager_init(void)
{
    g_uart_manager.state = LINKG_UART_LIFE_STOPPED;
    return 0;
}

int uart_manager_start(bool strict)
{
    (void)strict;
    g_uart_manager.state = LINKG_UART_LIFE_RUNNING;
    return 0;
}

int uart_manager_stop(void)
{
    g_uart_manager.state = LINKG_UART_LIFE_STOPPED;
    return 0;
}

int uart_manager_deinit(void)
{
    g_uart_manager.state = LINKG_UART_LIFE_UNINITIALIZED;
    return 0;
}

/**
 * @brief 创建三路串口各十六条转发规则的完整配置。
 */
static void fixture(linkg_config_t *config)
{
    uint32_t i;
    uint32_t j;

    config_set_default(config);
    config->network.node_id = 20;
    (void)inet_pton(AF_INET, "172.28.0.0", &config->network.virtual_network);
    (void)inet_pton(AF_INET, "192.168.1.0", &config->network.ethernet_network);
    config->links.wifi.enabled = true;
    config->links.cellular.enabled = true;
    config->uart.count = 3;
    for (i = 0; i < 3; i++)
    {
        linkg_uart_item_config_t *uart = &config->uart.items[i];
        uart->uart_num = i;
        uart->port_config.type = LINKG_UART_PORT_TYPE_NORMAL;
        uart->port_config.baudrate = 115200;
        uart->port_config.data_bits = 8;
        uart->port_config.stop_bits = 1;
        uart->port_config.parity = 'n';
        uart->route_count = 16;
        for (j = 0; j < 16; j++)
        {
            linkg_uart_route_config_t *route = &uart->routes[j];
            route->proto = LINKG_UART_ROUTE_PROTO_UDP;
            route->endpoint = LINKG_UART_ROUTE_ENDPOINT_REMOTE;
            route->dir = LINKG_UART_ROUTE_DIR_BOTH;
            route->port = 40000 + i * 100 + j;
            (void)inet_pton(AF_INET, "172.28.1.1", &route->ip);
        }
    }
    config->uart.items[0].port_config.type = LINKG_UART_PORT_TYPE_SBUS;
    config->uart.items[0].port_config.baudrate = 100000;
    config->uart.items[0].port_config.stop_bits = 2;
    config->uart.items[0].port_config.parity = 'e';
}

/**
 * @brief 导出后重新解析，验证全部串口及SBUS参数保留。
 */
static void test_roundtrip(void)
{
    linkg_config_t config;
    linkg_config_t decoded;
    cJSON         *root;
    cJSON         *response;
    char          *text;

    fixture(&config);
    TEST_CHECK_EQ(config_validate(&config), 0);
    TEST_CHECK_EQ(linkg_config_replace(&config), 0);
    TEST_CHECK_EQ(_linkg_web_handler_config_export(NULL, &text), 0);
    response = cJSON_Parse(text);
    TEST_CHECK(response != NULL);
    root = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(response, "data"), "config");
    TEST_CHECK(root != NULL);
    TEST_CHECK_EQ(linkg_config_parse_json(root, &decoded), 0);
    TEST_CHECK(linkg_uart_config_equal(&config.uart, &decoded.uart));
    TEST_CHECK_EQ(decoded.network.node_id, 20);
    cJSON_Delete(response);
    linkg_json_string_free(text);
}

/**
 * @brief 构造导入请求，保留原始文件文本给后端校验。
 */
static cJSON *request(const char *content)
{
    cJSON *param = cJSON_CreateObject();

    cJSON_AddStringToObject(param, "content", content);
    cJSON_AddStringToObject(param, "instance", "transfer-test-instance");
    cJSON_AddBoolToObject(param, "confirm", true);
    return param;
}

/**
 * @brief 非法文件、重复键、未确认与过期实例均不得保存或重启。
 */
static void test_invalid_import(void)
{
    const char *invalid[] = {"{}", "{", "[]", "{\"device\":{},\"device\":{}}", "{} trailing"};
    cJSON      *param;
    char       *response;
    size_t      i;

    _linkg_web_config_init();
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    {
        param = request(invalid[i]);
        TEST_CHECK_EQ(_linkg_web_handler_config_import(param, &response), 0);
        TEST_CHECK(strstr(response, "\"error\"") != NULL);
        linkg_json_string_free(response);
        cJSON_Delete(param);
        _linkg_web_config_response_complete(true);
        TEST_CHECK_EQ(g_test_restarts, 0);
        TEST_CHECK(!linkg_config_import_pending());
    }
    param = request("{}");
    cJSON_ReplaceItemInObject(param, "confirm", cJSON_CreateFalse());
    TEST_CHECK_EQ(_linkg_web_handler_config_import(param, &response), 0);
    TEST_CHECK(strstr(response, "\"error\"") != NULL);
    linkg_json_string_free(response);
    cJSON_ReplaceItemInObject(param, "confirm", cJSON_CreateTrue());
    cJSON_ReplaceItemInObject(param, "instance", cJSON_CreateString("stale-instance"));
    TEST_CHECK_EQ(_linkg_web_handler_config_import(param, &response), 0);
    TEST_CHECK(strstr(response, "\"error\"") != NULL);
    linkg_json_string_free(response);
    cJSON_Delete(param);
}

/**
 * @brief 验证原子写失败后的恢复，以及成功后旧配置不可覆盖。
 */
static void test_commit_and_reload(void)
{
    linkg_config_t old;
    linkg_config_t imported;
    linkg_config_t current;
    struct stat    info;
    cJSON         *root;
    cJSON         *param;
    char          *text;
    char          *response;
    int            fail;

    fixture(&old);
    TEST_CHECK_EQ(linkg_config_replace(&old), 0);
    TEST_CHECK_EQ(linkg_config_save(&old), 0);
    imported = old;
    imported.network.node_id = 21;
    TEST_CHECK_EQ(linkg_config_to_json(&imported, &root), 0);
    TEST_CHECK_EQ(linkg_json_print_formatted(root, &text), 0);
    cJSON_Delete(root);
    param = request(text);
    linkg_json_string_free(text);
    for (fail = 1; fail <= 2; fail++)
    {
        g_test_write_fail = fail;
        TEST_CHECK_EQ(_linkg_web_handler_config_import(param, &response), 0);
        TEST_CHECK(strstr(response, "\"error\"") != NULL);
        linkg_json_string_free(response);
        TEST_CHECK(!linkg_config_import_pending());
        TEST_CHECK_EQ(linkg_config_load(), 0);
        TEST_CHECK_EQ(linkg_config_create_snapshot(&current), 0);
        TEST_CHECK_EQ(current.network.node_id, 20);
    }
    TEST_CHECK_EQ(_linkg_web_handler_config_import(param, &response), 0);
    TEST_CHECK(strstr(response, "\"restarting\"") != NULL);
    linkg_json_string_free(response);
    cJSON_Delete(param);
    TEST_CHECK_EQ(stat(g_test_file, &info), 0);
    TEST_CHECK_EQ(info.st_mode & 0777, 0600);
    TEST_CHECK_EQ(linkg_config_create_snapshot(&current), 0);
    TEST_CHECK_EQ(current.network.node_id, 20);
    TEST_CHECK(linkg_config_import_pending());
    TEST_CHECK(linkg_config_save(&old) != 0);
    TEST_CHECK(linkg_config_restore_factory() != 0);
    TEST_CHECK_EQ(g_test_restarts, 0);
    _linkg_web_config_response_complete(false);
    TEST_CHECK_EQ(g_test_restarts, 1);
    _linkg_web_config_response_complete(true);
    TEST_CHECK_EQ(g_test_restarts, 1);
    linkg_config_deinit();
    TEST_CHECK_EQ(linkg_config_load(), 0);
    TEST_CHECK_EQ(linkg_config_create_snapshot(&current), 0);
    TEST_CHECK_EQ(current.network.node_id, 21);
    TEST_CHECK(linkg_uart_config_equal(&imported.uart, &current.uart));
    TEST_CHECK(!linkg_config_import_pending());
}

/**
 * @brief 导入落盘后重载失败保持文件保护，周期重试成功后才撤销待提交状态。
 */
static void test_reload_retry(void)
{
    linkg_config_t old;
    linkg_config_t imported;
    cJSON *root;
    cJSON *param;
    char *text;
    char *response;
    int attempts;

    linkg_config_deinit();
    _linkg_web_config_init();
    fixture(&old);
    TEST_CHECK_EQ(linkg_config_replace(&old), 0);
    TEST_CHECK_EQ(linkg_config_save(&old), 0);
    imported = old;
    imported.network.node_id = 22;
    TEST_CHECK_EQ(linkg_config_to_json(&imported, &root), 0);
    TEST_CHECK_EQ(linkg_json_print_formatted(root, &text), 0);
    cJSON_Delete(root);
    param = request(text);
    linkg_json_string_free(text);
    TEST_CHECK_EQ(_linkg_web_handler_config_import(param, &response), 0);
    linkg_json_string_free(response);
    cJSON_Delete(param);
    attempts = g_test_restarts;
    g_test_restart_failures = 2;
    _linkg_web_config_response_complete(false);
    TEST_CHECK(_linkg_web_config_reload_pending());
    TEST_CHECK(linkg_config_import_pending());
    TEST_CHECK(linkg_config_save(&old) != 0);
    _linkg_web_config_retry_reload();
    TEST_CHECK_EQ(g_test_restarts, attempts + 1);
    g_test_now += 1000U;
    _linkg_web_config_retry_reload();
    TEST_CHECK(_linkg_web_config_reload_pending());
    g_test_now += 1000U;
    _linkg_web_config_retry_reload();
    TEST_CHECK(!_linkg_web_config_reload_pending());
    TEST_CHECK_EQ(g_test_restarts, attempts + 3);
    TEST_CHECK(linkg_config_import_pending());
    TEST_CHECK(linkg_config_save(&old) != 0);
    linkg_config_deinit();
}

/**
 * @brief 真实UART接受任务立即阻止其他事务，创建失败撤销预留，跨线程领取后正常应用。
 */
static void test_uart_transaction_reservation(void)
{
    linkg_config_t config;
    linkg_uart_apply_status_t status;
    pthread_t worker;
    uint32_t job_id;
    uint64_t reservation;

    fixture(&config);
    TEST_CHECK_EQ(linkg_config_replace(&config), 0);
    TEST_CHECK_EQ(linkg_config_save(&config), 0);
    g_uart_manager.state = LINKG_UART_LIFE_RUNNING;
    config.uart.items[1].port_config.baudrate = 57600U;
    g_delay_create = true;
    TEST_CHECK_EQ(linkg_uart_manager_apply(&config.uart, &job_id), 0);
    TEST_CHECK(job_id != 0U && g_delayed_function != NULL);
    TEST_CHECK_EQ(linkg_config_transaction_begin(), -EBUSY);
    TEST_CHECK_EQ(linkg_config_transaction_reserve(&reservation), -EBUSY);
    TEST_CHECK_EQ(linkg_uart_manager_get_apply_status(&status), 0);
    TEST_CHECK_EQ(status.state, LINKG_UART_APPLY_RUNNING);
    g_delay_create = false;
    TEST_CHECK_EQ(__real_pthread_create(&worker, NULL, g_delayed_function, g_delayed_argument), 0);
    TEST_CHECK_EQ(pthread_join(worker, NULL), 0);
    TEST_CHECK_EQ(linkg_uart_manager_get_apply_status(&status), 0);
    TEST_CHECK_EQ(status.state, LINKG_UART_APPLY_APPLIED);
    TEST_CHECK_EQ(linkg_config_transaction_begin(), 0);
    linkg_config_transaction_end();
    config.uart.items[1].port_config.baudrate = 38400U;
    g_fail_create = true;
    TEST_CHECK_EQ(linkg_uart_manager_apply(&config.uart, &job_id), -EAGAIN);
    g_fail_create = false;
    TEST_CHECK_EQ(linkg_config_transaction_begin(), 0);
    linkg_config_transaction_end();
    TEST_CHECK_EQ(linkg_config_transaction_reserve(&reservation), 0);
    TEST_CHECK_EQ(linkg_config_transaction_begin_reserved(reservation + 1U), -ESTALE);
    TEST_CHECK_EQ(linkg_config_transaction_cancel_reserved(reservation + 1U), -ESTALE);
    TEST_CHECK_EQ(linkg_config_transaction_begin(), -EBUSY);
    TEST_CHECK_EQ(linkg_config_transaction_cancel_reserved(reservation), 0);
    TEST_CHECK_EQ(linkg_config_transaction_begin(), 0);
    linkg_config_transaction_end();
    linkg_config_deinit();
}

/**
 * @brief 浏览器集成测试的JSON行桥，读写真实配置模块但不操作硬件。
 */
static int gateway(void)
{
    linkg_config_t config;
    char          *line = NULL;
    size_t         capacity = 0;
    cJSON         *request_root;
    cJSON         *data;
    const cJSON   *param;
    char          *response;
    int            cmd;
    int            ret;

    fixture(&config);
    if (linkg_config_replace(&config) != 0 || linkg_config_save(&config) != 0)
    {
        return 1;
    }
    _linkg_web_config_init();
    while (getline(&line, &capacity, stdin) > 0)
    {
        request_root = NULL;
        response = NULL;
        cmd = 0;
        ret = linkg_json_parse_buffer(line, strlen(line), &request_root);
        if (ret != 0 || linkg_json_get_int(request_root, "cmd", &cmd) != 0)
        {
            ret = _linkg_web_response_error(0, "invalid request", &response);
        }
        else
        {
            param = cJSON_GetObjectItemCaseSensitive(request_root, "param");
            if (cmd == 1031)
            {
                ret = _linkg_web_handler_config_export(param, &response);
            }
            else if (cmd == 1032)
            {
                ret = linkg_config_transaction_begin();
                if (ret == 0)
                {
                    ret = _linkg_web_handler_config_import(param, &response);
                    linkg_config_transaction_end();
                }
            }
            else if (cmd == 1018 || cmd == 1001)
            {
                data = cJSON_CreateObject();
                cJSON_AddStringToObject(data, "instance", g_test_instance);
                cJSON_AddStringToObject(data, "role", "sta");
                cJSON_AddNumberToObject(data, "node_id", 20);
                ret = _linkg_web_response_success(cmd, data, NULL, &response);
            }
            else
            {
                ret = _linkg_web_response_error(cmd, "unsupported command", &response);
            }
        }
        cJSON_Delete(request_root);
        if (ret != 0 || response == NULL)
        {
            linkg_json_string_free(response);
            break;
        }
        puts(response);
        fflush(stdout);
        linkg_json_string_free(response);
        _linkg_web_config_response_complete(true);
        if (linkg_config_import_pending())
        {
            linkg_config_deinit();
            if (linkg_config_load() != 0)
            {
                break;
            }
            snprintf(g_test_instance, sizeof(g_test_instance), "transfer-test-instance-%d", g_test_restarts);
            _linkg_web_config_init();
        }
    }
    free(line);
    return 0;
}

/**
 * @brief 在独立临时目录执行全部配置导入导出用例。
 */
int main(int argc, char **argv)
{
    char directory[] = "/tmp/linkg-config-transfer-XXXXXX";
    int  ret;

    if (mkdtemp(directory) == NULL)
    {
        return 1;
    }
    snprintf(g_test_file, sizeof(g_test_file), "%s/user.json", directory);
    if (argc == 2 && strcmp(argv[1], "--gateway") == 0)
    {
        ret = gateway();
        linkg_config_deinit();
        unlink(g_test_file);
        rmdir(directory);
        return ret;
    }
    TEST_RUN(test_roundtrip);
    TEST_RUN(test_invalid_import);
    TEST_RUN(test_commit_and_reload);
    TEST_RUN(test_reload_retry);
    TEST_RUN(test_uart_transaction_reservation);
    linkg_config_deinit();
    unlink(g_test_file);
    rmdir(directory);
    ret = linkg_test_finish("config_transfer");
    return ret;
}

/**
 * @file test_network_rf.c
 * @brief RG255 FTM命令及独占诊断清理逻辑测试
 */

#define _POSIX_C_SOURCE 200809L // POSIX接口

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "linkg_network_task.h"
#include "linkg_rf_test.h"
#include "linkg_time.h"

#include "at_channel.h"
#include "rg255_cmd.h"

typedef struct
{
    const char *command;  // 预期AT指令
    int         result;   // 通道返回值
    const char *response; // 模拟AT回包
} rf_test_reply_t;

struct at_channel
{
    bool started; // 模拟串口是否已启动
};

static at_channel_t g_test_channel;
static const rf_test_reply_t *g_test_replies;
static size_t g_test_reply_count;
static size_t g_test_reply_index;
static char g_test_dir[] = "/tmp/linkg-rf-test-XXXXXX";
static char g_test_marker[128];
static char g_test_legacy_marker[128];
static uint64_t g_test_now_ms;
static bool g_test_running;
static bool g_test_cancel_on_wait;
static unsigned int g_test_close_failures;
static unsigned int g_test_restarts;
static linkg_network_cellular_task_run_t g_test_run;
static linkg_network_cellular_task_finish_t g_test_finish;
static void *g_test_context;

int __real_open(const char *path, int flags, ...);
int __real_access(const char *path, int mode);
int __real_unlink(const char *path);

/**
 * @brief 把生产标记路径映射到隔离测试目录。
 */
static const char *_test_path(const char *path)
{
    if (strcmp(path, "/app") == 0)
    {
        return g_test_dir;
    }
    if (strcmp(path, "/app/.linkg-rf-pending") == 0)
    {
        return g_test_marker;
    }
    assert(strcmp(path, "/tmp/linkg-rf-pending") == 0);
    return g_test_legacy_marker;
}

/**
 * @brief 在临时目录执行标记打开。
 */
int __wrap_open(const char *path, int flags, ...)
{
    if (flags & O_CREAT)
    {
        va_list args;
        int     mode;

        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
        return __real_open(_test_path(path), flags, mode);
    }
    return __real_open(_test_path(path), flags);
}

/**
 * @brief 在临时目录执行标记查询。
 */
int __wrap_access(const char *path, int mode)
{
    return __real_access(_test_path(path), mode);
}

/**
 * @brief 在临时目录执行标记删除。
 */
int __wrap_unlink(const char *path)
{
    return __real_unlink(_test_path(path));
}

/**
 * @brief 验证诊断使用独占串口。
 */
at_channel_t *at_channel_create(const char *device, const uart_config_t *config)
{
    assert(strcmp(device, "/dev/ttyUSB1") == 0 && config->exclusive);
    assert(!g_test_channel.started);
    return &g_test_channel;
}

/**
 * @brief 模拟AT接收线程启动。
 */
int at_channel_start(at_channel_t *channel)
{
    channel->started = true;
    return 0;
}

/**
 * @brief 模拟AT线程停止或暂时无法停止。
 */
int at_channel_stop(at_channel_t *channel)
{
    if (g_test_close_failures != 0U)
    {
        g_test_close_failures--;
        return -EBUSY;
    }
    channel->started = false;
    return 0;
}

/**
 * @brief 确认通道只在停机后销毁。
 */
void at_channel_destroy(at_channel_t *channel)
{
    assert(!channel->started);
}

/**
 * @brief 模拟AT返回并校验完整命令序列。
 */
int at_channel_exec(at_channel_t *channel, const char *command, const at_command_config_t *config, char *response, int size)
{
    const rf_test_reply_t *reply;

    assert(channel->started && config->timeout_ms > 0);
    assert(g_test_reply_index < g_test_reply_count);
    reply = &g_test_replies[g_test_reply_index++];
    assert(strcmp(command, reply->command) == 0);
    if (strstr(command, "\"tx\"") != NULL || strcmp(command, "AT+QRFTEST=\"mode\",1") == 0)
    {
        assert(__real_access(g_test_marker, F_OK) == 0);
    }
    assert(response != NULL && size > 0);
    snprintf(response, (size_t)size, "%s", reply->response);
    return reply->result;
}

/**
 * @brief 模拟普通AT可用。
 */
int rg255_cmd_test(at_channel_t *channel)
{
    assert(channel->started);
    return 0;
}

/**
 * @brief 模拟模组重启被拒绝。
 */
int rg255_cmd_restart(at_channel_t *channel)
{
    assert(channel->started);
    g_test_restarts++;
    return -EREMOTEIO;
}

/**
 * @brief 以虚拟时钟执行10秒窗口测试。
 */
uint64_t linkg_time_elapsed_ms(void)
{
    return g_test_now_ms;
}

/**
 * @brief 推进虚拟时钟并按需注入强发期间取消。
 */
int linkg_time_sleep_ms(uint32_t milliseconds)
{
    g_test_now_ms += milliseconds;
    if (g_test_cancel_on_wait && milliseconds == 100U)
    {
        g_test_running = false;
    }
    return 0;
}

/**
 * @brief 返回模拟任务取消状态。
 */
bool linkg_thread_is_running(const linkg_thread_t *thread)
{
    assert(thread != NULL);
    return g_test_running;
}

/**
 * @brief 模拟Manager接收独占任务。
 */
int linkg_network_cellular_task_start(linkg_network_cellular_task_run_t run, linkg_network_cellular_task_finish_t finish, void *context)
{
    g_test_run = run;
    g_test_finish = finish;
    g_test_context = context;
    return 0;
}

/**
 * @brief 提供未使用的取消入口替身。
 */
int linkg_network_cellular_task_cancel(void)
{
    return -ENOSYS;
}

/**
 * @brief 模拟独占任务及原Owner恢复后的完成通知。
 */
static void _test_complete_job(void)
{
    linkg_thread_t thread = {0};
    bool           released = false;
    int            ret;

    ret = g_test_run(&thread, g_test_context, &released);
    g_test_finish(g_test_context, ret, 0, released);
}

/**
 * @brief 设置一组模拟模组响应。
 */
static void _test_replies(const rf_test_reply_t *replies, size_t count)
{
    g_test_replies = replies;
    g_test_reply_count = count;
    g_test_reply_index = 0U;
}

/**
 * @brief 获取模拟任务并校验测试结果与清理结果各自保留。
 */
static void _test_run(int test_error, int cleanup_error, bool released, bool tx_confirmed, uint64_t elapsed_ms)
{
    const linkg_rf_params_t params = {2, 41, 3, 2593005, 10};
    linkg_rf_status_t       status;
    uint32_t               job_id;

    assert(linkg_rf_test_start(&params, &job_id) == 0);
    assert(job_id != 0U);
    _test_complete_job();
    linkg_rf_test_get(&status);
    assert(status.channel == 518601U && status.actual_khz == 2593005U);
    assert(status.test_error == test_error && status.cleanup_error == cleanup_error);
    assert(status.recovery_blocked == !released && status.tx_confirmed == tx_confirmed);
    assert(status.elapsed_ms == elapsed_ms && !status.active);
    assert(g_test_reply_index == g_test_reply_count);
}

/**
 * @brief 验证正常到期、超时收尾、错误分离和遗留标记恢复。
 */
int main(void)
{
    static const rf_test_reply_t success[] =
    {
        {"AT+QRFTEST=\"mode\"", 0, "+QRFTEST: 0"},
        {"AT+QRFTEST=\"mode\",1", 0, ""},
        {"AT+QRFTEST=\"mode\"", 0, "+QRFTEST: 0"},
        {"AT+QRFTEST=\"mode\"", 0, "+QRFTEST: 1"},
        {"AT+QRFTEST=\"tx\",2,41,3,518601,1,23", 0, ""},
        {"AT+QRFTEST=\"tx\",2,41,3,518601,0,23", 0, ""},
        {"AT+QRFTEST=\"mode\",0", 0, ""},
        {"AT+QRFTEST=\"mode\"", 0, "+QRFTEST: 0"}
    };
    static const rf_test_reply_t timeout[] =
    {
        {"AT+QRFTEST=\"mode\"", 0, "+QRFTEST: 0"},
        {"AT+QRFTEST=\"mode\",1", 0, ""},
        {"AT+QRFTEST=\"mode\"", 0, "+QRFTEST: 1"},
        {"AT+QRFTEST=\"tx\",2,41,3,518601,1,23", -ETIMEDOUT, ""},
        {"AT+QRFTEST=\"tx\",2,41,3,518601,0,23", -EAGAIN, ""},
        {"AT+QRFTEST=\"tx\",2,41,3,518601,0,23", -EREMOTEIO, "+CME ERROR: 3"},
        {"AT+QRFTEST=\"mode\",0", 0, ""},
        {"AT+QRFTEST=\"mode\"", 0, "+QRFTEST: 0"}
    };
    static const rf_test_reply_t failed_mode[] =
    {
        {"AT+QRFTEST=\"mode\"", -EREMOTEIO, "ERROR"}
    };
    static const rf_test_reply_t failed_exit[] =
    {
        {"AT+QRFTEST=\"mode\"", 0, "+QRFTEST: 0"},
        {"AT+QRFTEST=\"mode\",1", -EREMOTEIO, "ERROR"},
        {"AT+QRFTEST=\"mode\",0", -EREMOTEIO, "ERROR"},
        {"AT+QRFTEST=\"mode\"", -EPROTO, ""}
    };
    static const rf_test_reply_t recover[] =
    {
        {"AT+QRFTEST=\"mode\",0", 0, ""},
        {"AT+QRFTEST=\"mode\"", 0, "+QRFTEST: 0"}
    };
    linkg_rf_params_t invalid = {2, 41, 10, 2496000, 10};
    uint32_t channel;
    uint32_t khz;

    assert(mkdtemp(g_test_dir) != NULL);
    snprintf(g_test_marker, sizeof(g_test_marker), "%s/pending", g_test_dir);
    snprintf(g_test_legacy_marker, sizeof(g_test_legacy_marker), "%s/legacy", g_test_dir);
    assert(linkg_rf_test_resolve(&invalid, &channel, &khz) == -EINVAL);

    g_test_running = true;
    _test_replies(success, sizeof(success) / sizeof(success[0]));
    _test_run(0, 0, true, true, 10000U);

    _test_replies(timeout, sizeof(timeout) / sizeof(timeout[0]));
    _test_run(-ETIMEDOUT, -EREMOTEIO, true, false, 0U);

    _test_replies(failed_mode, sizeof(failed_mode) / sizeof(failed_mode[0]));
    _test_run(-EREMOTEIO, 0, true, false, 0U);
    assert(g_test_restarts == 0U && __real_access(g_test_marker, F_OK) != 0);

    _test_replies(failed_exit, sizeof(failed_exit) / sizeof(failed_exit[0]));
    _test_run(-EREMOTEIO, -EREMOTEIO, false, false, 0U);
    assert(g_test_restarts == 1U && __real_access(g_test_marker, F_OK) == 0);

    _test_replies(recover, sizeof(recover) / sizeof(recover[0]));
    assert(linkg_rf_test_recover_pending() == 0);
    assert(g_test_reply_index == g_test_reply_count && __real_access(g_test_marker, F_OK) != 0);

    g_test_close_failures = 1U;
    _test_replies(success, sizeof(success) / sizeof(success[0]));
    _test_run(0, -EBUSY, false, true, 10000U);
    assert(g_test_channel.started && __real_access(g_test_marker, F_OK) == 0);
    _test_replies(recover, sizeof(recover) / sizeof(recover[0]));
    assert(linkg_rf_test_recover_pending() == 0);
    assert(!g_test_channel.started && g_test_reply_index == g_test_reply_count);

    g_test_cancel_on_wait = true;
    _test_replies(success, sizeof(success) / sizeof(success[0]));
    _test_run(-ECANCELED, 0, true, true, 100U);
    assert(rmdir(g_test_dir) == 0);
    return 0;
}


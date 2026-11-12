/**
 * @file test_web_speedtest.c
 * @brief Web测速参数、结果解析和真实Worker子进程生命周期测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-09
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

static pid_t test_waitpid(pid_t pid, int *status, int options);
static int   test_kill(pid_t pid, int signal_number);

static char g_test_rmem_path[256];
static char g_test_wmem_path[256];
#define LINKG_WEB_SPEEDTEST_RMEM_MAX_PATH g_test_rmem_path
#define LINKG_WEB_SPEEDTEST_WMEM_MAX_PATH g_test_wmem_path

#define waitpid test_waitpid
#define kill    test_kill
#include "../../service/web/speedtest/web_speedtest.c"
#undef waitpid
#undef kill

static linkg_cellular_status_snapshot_t g_cellular;
static atomic_bool                     g_cancel_after_reap;
static atomic_int                      g_reaped_pid;
static atomic_int                      g_signals_after_reap;

/**
 * @brief 使用临时文件验证配额提高、已有大值保留和格式错误；不修改测试主机内核。
 */
static void test_buffer_limits(void)
{
    unsigned long value;
    FILE         *stream;

    stream = fopen(g_test_rmem_path, "w");
    assert(stream != NULL && fputs("212992\n", stream) >= 0 && fclose(stream) == 0);
    stream = fopen(g_test_wmem_path, "w");
    assert(stream != NULL && fputs("8388608\n", stream) >= 0 && fclose(stream) == 0);
    assert(_linkg_web_speedtest_prepare_buffer_limits() == 0);
    assert(_linkg_web_speedtest_read_buffer_limit(g_test_rmem_path, &value) == 0 && value == 4194304UL);
    assert(_linkg_web_speedtest_read_buffer_limit(g_test_wmem_path, &value) == 0 && value == 8388608UL);
    stream = fopen(g_test_rmem_path, "w");
    assert(stream != NULL && fputs("invalid\n", stream) >= 0 && fclose(stream) == 0);
    assert(_linkg_web_speedtest_prepare_buffer_limits() == -EINVAL);
    stream = fopen(g_test_rmem_path, "w");
    assert(stream != NULL && fputs("212992\n", stream) >= 0 && fclose(stream) == 0);
}

/**
 * @brief 提供可控的蜂窝状态，不访问真实蜂窝网络。
 */
int linkg_cellular_get_status(linkg_cellular_status_snapshot_t *snapshot)
{
    *snapshot = g_cellular;
    return 0;
}

/**
 * @brief 在回收子进程之后制造用户取消，以验证PID不会被再次用于发信号。
 */
static pid_t test_waitpid(pid_t pid, int *status, int options)
{
    pid_t result = waitpid(pid, status, options);

    if (result > 0)
    {
        atomic_store(&g_reaped_pid, (int)result);
        if (atomic_load(&g_cancel_after_reap))
        {
            pthread_mutex_lock(&g_web_speedtest.lock);
            g_web_speedtest.status.cancel_requested = true;
            pthread_mutex_unlock(&g_web_speedtest.lock);
        }
    }
    return result;
}

/**
 * @brief 拦截对已回收PID的信号，避免测试本身影响无关进程。
 */
static int test_kill(pid_t pid, int signal_number)
{
    if ((int)pid == atomic_load(&g_reaped_pid))
    {
        atomic_fetch_add(&g_signals_after_reap, 1);
        return 0;
    }
    return kill(pid, signal_number);
}

/**
 * @brief 构造合法组网UDP客户端参数。
 */
static linkg_web_speedtest_params_t client_params(void)
{
    linkg_web_speedtest_params_t params = {0};

    strcpy(params.server, "172.28.1.1");
    params.port           = 5600;
    params.duration_sec   = 5;
    params.packet_length  = 1300;
    params.bandwidth_mbps = 2.0;
    params.protocol       = LINKG_WEB_SPEEDTEST_UDP;
    return params;
}

/**
 * @brief 清空解析状态，但不破坏永久互斥锁。
 */
static void reset_parser(bool server)
{
    memset(&g_web_speedtest.status, 0, sizeof(g_web_speedtest.status));
    g_web_speedtest.status.params.mode = server ? LINKG_WEB_SPEEDTEST_MODE_SERVER : LINKG_WEB_SPEEDTEST_MODE_CLIENT;
    g_web_speedtest.last_sender_sum = false;
    g_web_speedtest.last_summary_sum = false;
}

/**
 * @brief 验证服务端多流总计优先于单流，接收端优先于发送端。
 */
static void test_summary_priority(void)
{
    reset_parser(true);
    _linkg_web_speedtest_parse_line("Accepted connection from 172.28.2.1, port 1234");
    _linkg_web_speedtest_parse_line("[  5] 0.00-5.00 sec 1 MBytes 100 Mbits/sec sender");
    _linkg_web_speedtest_parse_line("[SUM] 0.00-5.00 sec 4 MBytes 400 Mbits/sec sender");
    assert(g_web_speedtest.status.average_valid);
    assert(g_web_speedtest.status.average_mbps == 400.0);
    assert(g_web_speedtest.status.average_is_sender);
    assert(g_web_speedtest.status.sender_valid && g_web_speedtest.status.sender_mbps == 400.0);
    _linkg_web_speedtest_parse_line("[  7] 0.00-5.00 sec 1 MBytes 100 Mbits/sec sender");
    assert(g_web_speedtest.status.sender_mbps == 400.0);
    _linkg_web_speedtest_parse_line("[SUM] 0.00-5.00 sec 8 MBytes 800 Mbits/sec 1.0 ms 20/100 (20%) receiver");
    _linkg_web_speedtest_parse_line("[  5] 0.00-5.00 sec 1 MBytes 100 Mbits/sec 1.0 ms 10/100 (10%) receiver");
    assert(g_web_speedtest.status.average_mbps == 800.0);
    assert(g_web_speedtest.status.packet_loss_percent == 20.0);
    assert(!g_web_speedtest.status.average_is_sender);
    assert(g_web_speedtest.status.state == LINKG_WEB_SPEEDTEST_LISTENING);
    _linkg_web_speedtest_parse_line("Accepted connection from 172.28.2.1, port 1235");
    assert(g_web_speedtest.status.session_id == 2);
    assert(!g_web_speedtest.status.average_valid && !g_web_speedtest.status.packet_loss_valid);
    assert(!g_web_speedtest.status.sender_valid);
    assert(g_web_speedtest.status.sample_count == 0);
}

/**
 * @brief 验证超长日志整行被丢弃，下一行仍可正常解析。
 */
static void test_long_line(void)
{
    char   line[LINKG_WEB_SPEEDTEST_LINE_SIZE];
    char   noise[LINKG_WEB_SPEEDTEST_LINE_SIZE];
    size_t used = 0;
    const char *sample = "[  5] 0.00-1.00 sec 1 MBytes 8.00 Mbits/sec\r\n";

    reset_parser(false);
    memset(noise, 'x', sizeof(noise));
    _linkg_web_speedtest_consume_bytes(noise, sizeof(noise), line, &used);
    _linkg_web_speedtest_consume_bytes(sample, strlen(sample), line, &used);
    assert(g_web_speedtest.status.sample_count == 0);
    _linkg_web_speedtest_consume_bytes(sample, 11, line, &used);
    _linkg_web_speedtest_consume_bytes(sample + 11, strlen(sample) - 11, line, &used);
    assert(g_web_speedtest.status.sample_count == 1);
    assert(g_web_speedtest.status.current_mbps == 8.0);
}

/**
 * @brief 验证样本环形保留、SUM区间覆盖及客户端最终接收统计。
 */
static void test_samples(void)
{
    char text[192];

    reset_parser(false);
    for (unsigned second = 1; second <= 70; second++)
    {
        snprintf(text, sizeof(text), "[  5] %u.00-%u.00 sec 1 MBytes 8.00 Mbits/sec", second - 1, second);
        _linkg_web_speedtest_parse_line(text);
    }
    assert(g_web_speedtest.status.sample_count == 64);
    assert(g_web_speedtest.status.interval_count == 70);
    assert(g_web_speedtest.status.samples[0].second == 7.0);
    _linkg_web_speedtest_parse_line("[SUM] 69.00-70.00 sec 2 MBytes 16.00 Mbits/sec");
    _linkg_web_speedtest_parse_line("[  7] 69.00-70.00 sec 1 MBytes 1.00 Mbits/sec");
    assert(g_web_speedtest.status.current_mbps == 16.0);
    _linkg_web_speedtest_parse_line("[  5] 0.00-70.00 sec 1 GBytes 100 Mbits/sec sender");
    assert(!g_web_speedtest.status.average_valid);
    _linkg_web_speedtest_parse_line("[  5] 0.00-70.00 sec 1 GBytes 98 Mbits/sec receiver");
    assert(g_web_speedtest.status.average_valid && g_web_speedtest.status.average_mbps == 98.0);
}

/**
 * @brief 验证无蜂窝时组网目标仍可用，公网地址按IPv4/IPv6状态选择源。
 */
static void test_binding_and_validation(void)
{
    linkg_web_speedtest_params_t params = client_params();
    char                        address[48];
    const char                 *family;

    memset(&g_cellular, 0, sizeof(g_cellular));
    assert(_linkg_web_speedtest_params_valid(&params));
    assert(_linkg_web_speedtest_bind_address(&params, address, sizeof(address), &family) == 0);
    assert(address[0] == '\0' && strcmp(family, "-4") == 0);
    strcpy(params.server, "203.0.113.1");
    assert(_linkg_web_speedtest_bind_address(&params, address, sizeof(address), &family) == -ENETDOWN);
    g_cellular.data.pdp_active = true;
    g_cellular.data.ipv4_valid = true;
    inet_pton(AF_INET, "192.0.2.1", &g_cellular.data.ipv4);
    assert(_linkg_web_speedtest_bind_address(&params, address, sizeof(address), &family) == 0);
    assert(strcmp(address, "192.0.2.1") == 0);
    strcpy(params.server, "2001:db8::2");
    assert(_linkg_web_speedtest_bind_address(&params, address, sizeof(address), &family) == -EADDRNOTAVAIL);
    g_cellular.data.global_ipv6_valid = true;
    inet_pton(AF_INET6, "2001:db8::1", &g_cellular.data.global_ipv6);
    assert(_linkg_web_speedtest_bind_address(&params, address, sizeof(address), &family) == 0);
    assert(strcmp(address, "2001:db8::1") == 0 && strcmp(family, "-6") == 0);
    strcpy(params.server, "example.test");
    assert(_linkg_web_speedtest_bind_address(&params, address, sizeof(address), &family) == 0);
    assert(strcmp(family, "-4") == 0);
    params.bandwidth_mbps = NAN;
    assert(!_linkg_web_speedtest_params_valid(&params));
    params.bandwidth_mbps = 2;
    params.auto_port = true;
    assert(!_linkg_web_speedtest_params_valid(&params));
    params.auto_port = false;
    strcpy(params.server, "-invalid");
    assert(!_linkg_web_speedtest_params_valid(&params));
}

/**
 * @brief 等待真实Worker发布终态。
 */
static linkg_web_speedtest_status_t wait_terminal(uint32_t job)
{
    linkg_web_speedtest_status_t status;
    uint64_t                    until = linkg_time_elapsed_ms() + 7000;

    do
    {
        assert(_linkg_web_speedtest_get(job, &status) == 0);
        if (!_linkg_web_speedtest_state_active(status.state))
        {
            return status;
        }
        poll(NULL, 0, 10);
    } while (linkg_time_elapsed_ms() < until);
    assert(!"Worker did not reach a terminal state");
    return status;
}

/**
 * @brief 验证完成、失败、强制取消之后可再次提交任务，且无子进程遗留。
 */
static void test_worker_reuse(void)
{
    static const struct
    {
        double       bandwidth_mbps;
        uint32_t     packet_length;
        unsigned int burst;
    } cases[] =
    {
        {0.1, 1300U, 1U},
        {6.0, 1400U, 2U},
        {20.0, 512U, 16U},
        {50.0, 1300U, 16U},
        {95.0, 1400U, 32U},
        {180.0, 1400U, 32U},
        {180.0, 1472U, 32U},
        {200.0, 200U, 32U}
    };
    linkg_web_speedtest_params_t params = client_params();
    linkg_web_speedtest_status_t status;
    char                        burst_text[8];
    uint32_t                    job;
    uint32_t                    rejected;

    assert(_linkg_web_speedtest_init() == 0 && _linkg_web_speedtest_start() == 0);
    for (size_t pass = 0U; pass < sizeof(cases) / sizeof(cases[0]); pass++)
    {
        params.bandwidth_mbps = cases[pass].bandwidth_mbps;
        params.packet_length  = cases[pass].packet_length;
        params.direction = pass % 2U != 0U ? LINKG_WEB_SPEEDTEST_DOWNLOAD : LINKG_WEB_SPEEDTEST_UPLOAD;
        snprintf(burst_text, sizeof(burst_text), "%u", cases[pass].burst);
        assert(setenv("LINKG_TEST_EXPECT_BURST", burst_text, 1) == 0);
        setenv("LINKG_TEST_IPERF_MODE", "normal", 1);
        assert(_linkg_web_speedtest_submit(&params, &job) == 0);
        status = wait_terminal(job);
        assert(status.state == LINKG_WEB_SPEEDTEST_COMPLETED);
        assert(status.average_valid && status.average_mbps == 7.5);
    }
    assert(unsetenv("LINKG_TEST_EXPECT_BURST") == 0);
    params.protocol = LINKG_WEB_SPEEDTEST_TCP;
    assert(_linkg_web_speedtest_submit(&params, &job) == 0);
    status = wait_terminal(job);
    assert(status.state == LINKG_WEB_SPEEDTEST_COMPLETED);
    params.protocol = LINKG_WEB_SPEEDTEST_UDP;
    setenv("LINKG_TEST_IPERF_MODE", "missing_summary", 1);
    assert(_linkg_web_speedtest_submit(&params, &job) == 0);
    status = wait_terminal(job);
    assert(status.state == LINKG_WEB_SPEEDTEST_FAILED);
    setenv("LINKG_TEST_IPERF_MODE", "stubborn", 1);
    assert(_linkg_web_speedtest_submit(&params, &job) == 0);
    assert(_linkg_web_speedtest_submit(&params, &rejected) == -EBUSY);
    pthread_mutex_lock(&g_web_speedtest.lock);
    g_web_speedtest.status.server_connected = true;
    pthread_mutex_unlock(&g_web_speedtest.lock);
    poll(NULL, 0, 250);
    assert(_linkg_web_speedtest_cancel(job) == 0);
    status = wait_terminal(job);
    assert(status.state == LINKG_WEB_SPEEDTEST_CANCELLED);
    assert(!status.server_connected);
    assert(_linkg_web_speedtest_cancel(job) == -EALREADY);
    setenv("LINKG_TEST_IPERF_MODE", "long_eof", 1);
    assert(_linkg_web_speedtest_submit(&params, &job) == 0);
    status = wait_terminal(job);
    assert(status.state == LINKG_WEB_SPEEDTEST_FAILED);
    assert(_linkg_web_speedtest_stop() == 0);
    _linkg_web_speedtest_deinit();
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    assert(_linkg_web_speedtest_init() == 0);
    assert(_linkg_web_speedtest_get(0, &status) == 0);
    assert(status.job_id == 0 && status.state == LINKG_WEB_SPEEDTEST_IDLE);
    assert(!status.average_valid && status.sample_count == 0);
    _linkg_web_speedtest_deinit();
}

/**
 * @brief 使用本地夹具验证公网繁忙端口轮换，不访问外部服务器。
 */
static void test_public_port_retry(void)
{
    linkg_web_speedtest_params_t params = client_params();
    linkg_web_speedtest_status_t status;
    uint32_t                    job;

    memset(&g_cellular, 0, sizeof(g_cellular));
    g_cellular.data.pdp_active = true;
    g_cellular.data.ipv4_valid = true;
    inet_pton(AF_INET, "192.0.2.1", &g_cellular.data.ipv4);
    strcpy(params.server, LINKG_WEB_SPEEDTEST_PUBLIC_SERVER);
    params.port      = 5201;
    params.auto_port = true;
    setenv("LINKG_TEST_IPERF_MODE", "busy", 1);
    assert(_linkg_web_speedtest_init() == 0 && _linkg_web_speedtest_start() == 0);
    assert(_linkg_web_speedtest_submit(&params, &job) == 0);
    status = wait_terminal(job);
    assert(status.state == LINKG_WEB_SPEEDTEST_COMPLETED);
    assert(status.params.port == 5203);
    setenv("LINKG_TEST_IPERF_MODE", "exit_failure", 1);
    assert(_linkg_web_speedtest_submit(&params, &job) == 0);
    status = wait_terminal(job);
    assert(status.state == LINKG_WEB_SPEEDTEST_FAILED);
    assert(strstr(status.message, "=2") != NULL);
    _linkg_web_speedtest_deinit();
}

/**
 * @brief 验证已waitpid回收的PID不会在取消时收到终止信号。
 */
static void test_reaped_process_signal(void)
{
    linkg_web_speedtest_params_t params = client_params();
    uint32_t                    job;

    atomic_store(&g_signals_after_reap, 0);
    atomic_store(&g_cancel_after_reap, true);
    setenv("LINKG_TEST_IPERF_MODE", "hold_pipe", 1);
    assert(_linkg_web_speedtest_init() == 0 && _linkg_web_speedtest_start() == 0);
    assert(_linkg_web_speedtest_submit(&params, &job) == 0);
    (void)wait_terminal(job);
    assert(atomic_load(&g_signals_after_reap) == 0);
    atomic_store(&g_cancel_after_reap, false);
    _linkg_web_speedtest_deinit();
}

/**
 * @brief 检查合法带宽与包长范围内的突发上限、预算和单调性。
 */
static void test_udp_burst_budget(void)
{
    linkg_web_speedtest_params_t params = client_params();
    uint32_t                    previous_rate[1473] = {0};
    uint32_t                    previous_length;
    uint32_t                    burst;
    double                      budget;
    double                      packet_bits;

    for (uint32_t rate = 0U; rate <= 200U; rate++)
    {
        params.bandwidth_mbps = rate == 0U ? 0.1 : (double)rate;
        budget = params.bandwidth_mbps * (double)LINKG_WEB_SPEEDTEST_UDP_BURST_WINDOW_US;
        previous_length = LINKG_WEB_SPEEDTEST_UDP_BURST_MAX;

        for (uint32_t length = 200U; length <= 1472U; length++)
        {
            params.packet_length = length;
            packet_bits = 8.0 * (double)length;
            burst = _linkg_web_speedtest_udp_burst(&params);
            assert(burst >= 1U && burst <= LINKG_WEB_SPEEDTEST_UDP_BURST_MAX);
            assert((burst & (burst - 1U)) == 0U);
            assert(burst >= previous_rate[length] && burst <= previous_length);
            assert(burst == 1U || (double)burst * packet_bits <= budget);
            assert(burst == LINKG_WEB_SPEEDTEST_UDP_BURST_MAX || (double)(burst * 2U) * packet_bits > budget);
            previous_rate[length] = burst;
            previous_length = burst;
        }
    }
}

/**
 * @brief 运行指定复现用例或完整回归。
 */
int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "all";
    char        directory[] = "/tmp/linkg-speedtest-limits-XXXXXX";

    assert(mkdtemp(directory) != NULL);
    snprintf(g_test_rmem_path, sizeof(g_test_rmem_path), "%s/rmem_max", directory);
    snprintf(g_test_wmem_path, sizeof(g_test_wmem_path), "%s/wmem_max", directory);
    test_buffer_limits();
    assert(linkg_time_init() == 0);
    if (strcmp(name, "burst") == 0 || strcmp(name, "all") == 0)
    {
        test_udp_burst_budget();
    }
    if (strcmp(name, "summary") == 0 || strcmp(name, "all") == 0)
    {
        test_summary_priority();
    }
    if (strcmp(name, "line") == 0 || strcmp(name, "all") == 0)
    {
        test_long_line();
    }
    if (strcmp(name, "samples") == 0 || strcmp(name, "all") == 0)
    {
        test_samples();
    }
    if (strcmp(name, "binding") == 0 || strcmp(name, "all") == 0)
    {
        test_binding_and_validation();
    }
    if (strcmp(name, "worker") == 0 || strcmp(name, "all") == 0)
    {
        test_worker_reuse();
    }
    if (strcmp(name, "reap") == 0 || strcmp(name, "all") == 0)
    {
        test_reaped_process_signal();
    }
    if (strcmp(name, "retry") == 0 || strcmp(name, "all") == 0)
    {
        test_public_port_retry();
    }
    printf("PASS web_speedtest %s\n", name);
    assert(unlink(g_test_rmem_path) == 0);
    assert(unlink(g_test_wmem_path) == 0);
    assert(rmdir(directory) == 0);
    return 0;
}

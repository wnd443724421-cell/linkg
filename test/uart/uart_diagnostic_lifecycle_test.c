#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "uart_internal.h"
#include "uart_diagnostic_internal.h"
#include "linkg_config.h"
#include "linkg_gpio_manager.h"
#include "linkg_log.h"

linkg_uart_manager_context_t g_uart_manager = {.life_lock = PTHREAD_MUTEX_INITIALIZER};
static atomic_bool held, releasing, test_wait, test_entered, begin_wait, begin_entered;
static atomic_bool acquired;
static int begin_error, test_error, restore_error;
static atomic_int creates, joins, idle_polls;
static atomic_bool fail_init, fail_create, fail_join, fail_wake, fail_poll;
static atomic_bool poll_entered, hold_poll, poll_held;
static linkg_thread_t *worker;
static pthread_t task_thread;

int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *tid, const pthread_attr_t *attr, void *(*entry)(void *), void *data)
{
    if (atomic_exchange(&fail_create, false)) { return EAGAIN; }
    int ret = __real_pthread_create(tid, attr, entry, data);
    if (ret == 0) {
        atomic_fetch_add(&creates, 1);
        if (data != NULL) { worker = data; }
    }
    return ret;
}
int __real_pthread_join(pthread_t, void **);
int __wrap_pthread_join(pthread_t tid, void **result)
{
    if (atomic_exchange(&fail_join, false)) { return EBUSY; }
    int ret = __real_pthread_join(tid, result);
    if (ret == 0) { atomic_fetch_add(&joins, 1); }
    return ret;
}
int __real_eventfd(unsigned int, int);
int __wrap_eventfd(unsigned int value, int flags)
{
    if (atomic_exchange(&fail_init, false)) { errno = EMFILE; return -1; }
    return __real_eventfd(value, flags);
}
int __real_linkg_thread_wakeup(linkg_thread_t *thread);
int __wrap_linkg_thread_wakeup(linkg_thread_t *thread)
{
    if (atomic_exchange(&fail_wake, false)) { return -EIO; }
    return __real_linkg_thread_wakeup(thread);
}
int __real_poll(struct pollfd *, nfds_t, int);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && timeout == -1); // idle worker must use an indefinite event wait
    atomic_fetch_add(&idle_polls, 1);
    atomic_store(&poll_entered, true);
    while (atomic_load(&hold_poll)) { atomic_store(&poll_held, true); usleep(1000); }
    if (atomic_exchange(&fail_poll, false)) { errno = EIO; return -1; }
    return __real_poll(fds, count, timeout);
}
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes)
{
    assert(bytes >= count * sizeof(*fds));
    return __wrap_poll(fds, count, timeout);
}
void linkg_log_write_internal(linkg_log_level_t level, const char *file, int line, const char *format, ...)
{
    (void)level; (void)file; (void)line; (void)format;
}

uint64_t linkg_time_elapsed_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}
int linkg_time_sleep_ms(uint32_t ms) { (void)ms; return 0; }
int linkg_config_transaction_begin(void)
{
    atomic_store(&begin_entered, true);
    while (atomic_load(&begin_wait)) { usleep(1000); }
    return begin_error;
}
void linkg_config_transaction_end(void) {}
int linkg_gpio_manager_set_sbus_enabled(bool enabled) { (void)enabled; return 0; }
int uart_manager_diagnostic_acquire(void)
{
    return atomic_exchange(&acquired, true) ? -EBUSY : 0;
}
void uart_manager_diagnostic_release(void)
{
    atomic_store(&releasing, true);
    while (atomic_load(&held)) { usleep(1000); }
    atomic_store(&acquired, false);
}
static void manager_state(linkg_uart_life_t state)
{
    pthread_mutex_lock(&g_uart_manager.life_lock);
    g_uart_manager.state = state;
    pthread_mutex_unlock(&g_uart_manager.life_lock);
}
int uart_manager_stop(void) { manager_state(LINKG_UART_LIFE_STOPPED); return 0; }
int uart_manager_start(bool strict)
{
    (void)strict;
    if (restore_error) { return restore_error; }
    manager_state(LINKG_UART_LIFE_RUNNING);
    return 0;
}
int uart_diagnostic_test_port(uint8_t uart, linkg_uart_diagnostic_path_t path,
                              linkg_uart_diagnostic_test_result_t *result)
{
    (void)uart; (void)path;
    task_thread = pthread_self();
    atomic_store(&test_entered, true);
    while (atomic_load(&test_wait) && !uart_diagnostic_cancel_requested()) { usleep(1000); }
    int error = uart_diagnostic_cancel_requested() ? -ECANCELED : test_error;
    result->error = error;
    result->state = error == -ECANCELED ? LINKG_UART_DIAGNOSTIC_TEST_CANCELLED :
                    error ? LINKG_UART_DIAGNOSTIC_TEST_FAILED : LINKG_UART_DIAGNOSTIC_TEST_PASSED;
    return error;
}
static bool active(linkg_uart_diagnostic_state_t state)
{
    return state == LINKG_UART_DIAGNOSTIC_STATE_STARTING ||
           state == LINKG_UART_DIAGNOSTIC_STATE_RUNNING || state == LINKG_UART_DIAGNOSTIC_STATE_RESTORING;
}
static void wait_flag(atomic_bool *flag)
{
    uint64_t deadline = linkg_time_elapsed_ms() + 3000;
    while (!atomic_load(flag) && linkg_time_elapsed_ms() < deadline) { usleep(1000); }
    assert(atomic_load(flag));
}
static uint32_t start_job(bool all)
{
    uint32_t job = 0;
    linkg_uart_diagnostic_params_t params = {
        .mode = all ? LINKG_UART_DIAGNOSTIC_MODE_ALL : LINKG_UART_DIAGNOSTIC_MODE_SINGLE,
        .uart_num = all ? 0 : 1
    };
    assert(linkg_uart_diagnostic_submit(&params, &job) == 0);
    assert(job != 0);
    return job;
}
static linkg_uart_diagnostic_status_t finish(uint32_t job)
{
    linkg_uart_diagnostic_status_t status;
    uint64_t deadline = linkg_time_elapsed_ms() + 3000;
    do {
        assert(linkg_uart_diagnostic_get_status(job, &status) == 0);
        if (!active(status.state)) { assert(!atomic_load(&acquired)); return status; }
        usleep(1000);
    } while (linkg_time_elapsed_ms() < deadline);
    assert(!"diagnostic did not finish");
    return status;
}
static void reset(void)
{
    assert(linkg_uart_diagnostic_deinit() == 0);
    atomic_store(&held, false); atomic_store(&releasing, false);
    atomic_store(&test_wait, false); atomic_store(&test_entered, false);
    atomic_store(&begin_wait, false); atomic_store(&begin_entered, false);
    atomic_store(&poll_entered, false); atomic_store(&poll_held, false);
    begin_error = test_error = restore_error = 0;
    manager_state(LINKG_UART_LIFE_RUNNING);
    assert(linkg_uart_diagnostic_init() == 0);
    assert(linkg_uart_diagnostic_start() == 0);
}
static void *deinit_worker(void *unused)
{
    (void)unused;
    assert(linkg_uart_diagnostic_deinit() == 0);
    return NULL;
}
static void *stop_worker(void *unused)
{
    (void)unused;
    assert(linkg_uart_diagnostic_stop() == 0);
    return NULL;
}
int main(void)
{
    reset();
    atomic_store(&held, true);
    uint32_t job = start_job(false);
    wait_flag(&releasing);
    linkg_uart_diagnostic_status_t status;
    assert(linkg_uart_diagnostic_get_status(job, &status) == 0);
    assert(active(status.state)); // releasing resources must never be visible as terminal
    atomic_store(&held, false);
    status = finish(job);
    assert(status.tests_passed && status.uart_restored);
    pthread_t original_thread = task_thread;
    int original_creates = atomic_load(&creates), original_joins = atomic_load(&joins);
    for (int i = 0; i < 20; i++) {
        status = finish(start_job(false)); assert(status.tests_passed);
        assert(pthread_equal(original_thread, task_thread));
    }
    assert(atomic_load(&creates) == original_creates && atomic_load(&joins) == original_joins);
    wait_flag(&poll_entered);
    usleep(10000); // allow the final task's eventfd to be consumed
    int polls = atomic_load(&idle_polls);
    usleep(30000);
    assert(atomic_load(&idle_polls) == polls);
    assert(linkg_uart_diagnostic_init() == -EALREADY);
    assert(linkg_uart_diagnostic_start() == -EALREADY);

    reset(); begin_error = -EBUSY;
    status = finish(start_job(true));
    assert(status.state == LINKG_UART_DIAGNOSTIC_STATE_FAILED && status.uart_restored);
    assert(status.uart_was_initialized && status.uart_was_running);
    for (uint32_t i = 0; i < status.test_count; i++) { assert(status.tests[i].state == LINKG_UART_DIAGNOSTIC_TEST_SKIPPED); }
    begin_error = 0; assert(finish(start_job(false)).tests_passed);

    reset(); atomic_store(&begin_wait, true);
    job = start_job(true); wait_flag(&begin_entered);
    assert(linkg_uart_diagnostic_cancel(job) == 0);
    atomic_store(&begin_wait, false);
    status = finish(job);
    assert(status.state == LINKG_UART_DIAGNOSTIC_STATE_CANCELLED && status.uart_restored);
    for (uint32_t i = 0; i < status.test_count; i++) { assert(status.tests[i].state == LINKG_UART_DIAGNOSTIC_TEST_SKIPPED); }
    assert(finish(start_job(false)).tests_passed);

    reset(); atomic_store(&test_wait, true);
    job = start_job(true); wait_flag(&test_entered);
    linkg_uart_diagnostic_params_t params = {.mode = LINKG_UART_DIAGNOSTIC_MODE_SINGLE, .uart_num = 1};
    uint32_t rejected;
    assert(linkg_uart_diagnostic_submit(&params, &rejected) == -EBUSY);
    assert(linkg_uart_diagnostic_cancel(job + 1) == -ENOENT);
    assert(linkg_uart_diagnostic_cancel(job) == 0);
    status = finish(job);
    assert(status.state == LINKG_UART_DIAGNOSTIC_STATE_CANCELLED && status.uart_restored);
    assert(status.tests[0].state == LINKG_UART_DIAGNOSTIC_TEST_CANCELLED);
    atomic_store(&test_wait, false);
    assert(finish(start_job(false)).tests_passed);

    reset(); test_error = -ETIMEDOUT;
    status = finish(start_job(false));
    assert(status.state == LINKG_UART_DIAGNOSTIC_STATE_COMPLETED && !status.tests_passed && status.uart_restored);
    test_error = 0; assert(finish(start_job(false)).tests_passed);

    reset(); restore_error = -EIO;
    status = finish(start_job(false));
    assert(status.state == LINKG_UART_DIAGNOSTIC_STATE_FAILED && !status.uart_restored && status.restore_error == -EIO);
    restore_error = 0; assert(finish(start_job(false)).tests_passed);

    for (int state = LINKG_UART_LIFE_UNINITIALIZED; state <= LINKG_UART_LIFE_STOPPED; state++) {
        reset(); manager_state((linkg_uart_life_t)state);
        status = finish(start_job(false));
        assert(status.uart_restored && g_uart_manager.state == (linkg_uart_life_t)state);
    }
    assert(linkg_uart_diagnostic_deinit() == 0);
    assert(linkg_uart_diagnostic_submit(&params, &rejected) == -ENODEV);

    reset(); atomic_store(&held, true);
    (void)start_job(false); wait_flag(&releasing);
    pthread_t shutdown;
    assert(pthread_create(&shutdown, NULL, deinit_worker, NULL) == 0);
    uint64_t deadline = linkg_time_elapsed_ms() + 3000;
    int ret;
    do {
        ret = linkg_uart_diagnostic_submit(&params, &rejected);
        assert(ret == -EBUSY || ret == -ESHUTDOWN);
        if (ret == -ESHUTDOWN) { break; }
        usleep(1000);
    } while (linkg_time_elapsed_ms() < deadline);
    assert(ret == -ESHUTDOWN); // teardown closes submission before waiting for the worker
    atomic_store(&held, false);
    assert(pthread_join(shutdown, NULL) == 0);

    atomic_store(&fail_init, true);
    assert(linkg_uart_diagnostic_init() == -EMFILE);
    assert(linkg_uart_diagnostic_init() == 0);
    assert(linkg_uart_diagnostic_submit(&params, &rejected) == -ESHUTDOWN);
    atomic_store(&fail_create, true);
    assert(linkg_uart_diagnostic_start() == -EAGAIN);
    assert(linkg_uart_diagnostic_start() == 0);
    status = finish(start_job(false));
    uint32_t previous_job = status.job_id;
    atomic_store(&fail_wake, true);
    rejected = 999;
    assert(linkg_uart_diagnostic_submit(&params, &rejected) == -EIO);
    assert(rejected == 0 && !atomic_load(&acquired));
    assert(linkg_uart_diagnostic_get_status(0, &status) == 0);
    assert(status.job_id == previous_job && status.tests_passed);
    assert(finish(start_job(false)).tests_passed);

    atomic_store(&test_wait, true); atomic_store(&test_entered, false);
    job = start_job(false); wait_flag(&test_entered);
    assert(linkg_uart_diagnostic_stop() == 0);
    status = finish(job);
    assert(status.state == LINKG_UART_DIAGNOSTIC_STATE_CANCELLED && status.uart_restored);
    assert(linkg_uart_diagnostic_submit(&params, &rejected) == -ESHUTDOWN);
    atomic_store(&test_wait, false);
    assert(linkg_uart_diagnostic_start() == 0);
    assert(finish(start_job(false)).tests_passed);

    // An accepted task must still release its gate if shutdown precedes execution.
    atomic_store(&hold_poll, true); atomic_store(&poll_held, false);
    assert(linkg_thread_wakeup(worker) == 0);
    wait_flag(&poll_held);
    job = start_job(true);
    assert(pthread_create(&shutdown, NULL, stop_worker, NULL) == 0);
    deadline = linkg_time_elapsed_ms() + 3000;
    do {
        ret = linkg_uart_diagnostic_submit(&params, &rejected);
        assert(ret == -EBUSY || ret == -ESHUTDOWN);
        if (ret == -ESHUTDOWN) { break; }
        usleep(1000);
    } while (linkg_time_elapsed_ms() < deadline);
    assert(ret == -ESHUTDOWN);
    atomic_store(&hold_poll, false);
    assert(pthread_join(shutdown, NULL) == 0);
    status = finish(job);
    assert(status.state == LINKG_UART_DIAGNOSTIC_STATE_CANCELLED && status.uart_restored);
    for (uint32_t i = 0; i < status.test_count; i++) { assert(status.tests[i].state == LINKG_UART_DIAGNOSTIC_TEST_SKIPPED); }
    assert(linkg_uart_diagnostic_start() == 0);
    assert(finish(start_job(false)).tests_passed);

    atomic_store(&fail_join, true);
    assert(linkg_uart_diagnostic_stop() == -EBUSY);
    assert(worker->initialized && linkg_thread_is_started(worker));
    assert(linkg_uart_diagnostic_submit(&params, &rejected) == -ESHUTDOWN);
    assert(linkg_uart_diagnostic_start() == -EBUSY);
    assert(linkg_uart_diagnostic_stop() == 0);
    assert(linkg_uart_diagnostic_start() == 0);
    assert(finish(start_job(false)).tests_passed);

    // Fail the event wait after a task was accepted but before it could run.
    atomic_store(&hold_poll, true); atomic_store(&poll_held, false);
    assert(linkg_thread_wakeup(worker) == 0);
    wait_flag(&poll_held);
    job = start_job(false);
    atomic_store(&fail_poll, true); atomic_store(&hold_poll, false);
    status = finish(job);
    assert(status.state == LINKG_UART_DIAGNOSTIC_STATE_FAILED && status.error == -EIO);
    assert(status.uart_restored && status.tests[0].state == LINKG_UART_DIAGNOSTIC_TEST_SKIPPED);
    assert(linkg_uart_diagnostic_submit(&params, &rejected) == -ESHUTDOWN);
    assert(linkg_uart_diagnostic_stop() == 0);
    assert(linkg_uart_diagnostic_start() == 0);
    assert(finish(start_job(false)).tests_passed);
    assert(linkg_uart_diagnostic_deinit() == 0);
    puts("PASS diagnostic lifecycle: terminal after release, repeat, failure, cancel, skipped items, restoration");
    puts("PASS resident worker: one TID, no per-job create/join, indefinite idle, init/start/wakeup/join/wait failures, stop/restart");
    return 0;
}

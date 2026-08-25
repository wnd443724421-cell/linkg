/**
 * @file linkg_time.c
 * @brief LinkG时间操作实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-28
 */

#define _POSIX_C_SOURCE 200809L

#include "linkg_time.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

/****************************** 模块常量 ******************************/

#define LINKG_US_PER_SECOND 1000000ULL    // 每秒包含的微秒数
#define LINKG_MS_PER_SECOND 1000ULL       // 每秒包含的毫秒数
#define LINKG_NS_PER_SECOND 1000000000L   // 每秒包含的纳秒数
#define LINKG_NS_PER_US     1000L         // 每微秒包含的纳秒数

/****************************** 全局上下文 ******************************/

static struct timespec g_start_time;
static bool            g_initialized = false;

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取当前单调时钟。
 */
static int _get_monotonic_time(struct timespec *time_value)
{
    if (time_value == NULL)
    {
        return -EINVAL;
    }

    if (clock_gettime(CLOCK_MONOTONIC, time_value) != 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 将 timespec 转换为微秒。
 */
static uint64_t _timespec_to_us(const struct timespec *time_value)
{
    return ((uint64_t)time_value->tv_sec * LINKG_US_PER_SECOND) +
           ((uint64_t)time_value->tv_nsec / LINKG_NS_PER_US);
}

/**
 * @brief 计算两个单调时钟时间点之间的微秒差值。
 */
static uint64_t _timespec_diff_us(const struct timespec *start_time, const struct timespec *end_time)
{
    time_t seconds;
    long   nanoseconds;

    if (start_time == NULL || end_time == NULL)
    {
        return 0;
    }

    if (end_time->tv_sec < start_time->tv_sec ||
        (end_time->tv_sec == start_time->tv_sec && end_time->tv_nsec < start_time->tv_nsec))
    {
        return 0;
    }

    seconds     = end_time->tv_sec - start_time->tv_sec;
    nanoseconds = end_time->tv_nsec - start_time->tv_nsec;

    if (nanoseconds < 0)
    {
        seconds--;
        nanoseconds += LINKG_NS_PER_SECOND;
    }

    return ((uint64_t)seconds * LINKG_US_PER_SECOND) +
           ((uint64_t)nanoseconds / LINKG_NS_PER_US);
}

/**
 * @brief 将微秒数转换为 timespec。
 */
static void _us_to_timespec(uint64_t microseconds, struct timespec *time_value)
{
    time_value->tv_sec  = (time_t)(microseconds / LINKG_US_PER_SECOND);
    time_value->tv_nsec = (long)((microseconds % LINKG_US_PER_SECOND) * LINKG_NS_PER_US);
}

/**
 * @brief 使用单调时钟进行相对睡眠。
 */
static int _sleep_relative_us(uint64_t microseconds)
{
    struct timespec request;
    struct timespec remaining;
    int             result;

    _us_to_timespec(microseconds, &request);

    for (;;)
    {
        result = clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &remaining);

        if (result == 0)
        {
            return 0;
        }

        if (result != EINTR)
        {
            return -result;
        }

        request = remaining;
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化时间。
 */
int  linkg_time_init(void)
{
    struct timespec start_time;
    int             result;

    if (g_initialized)
    {
        return 0;
    }

    result = _get_monotonic_time(&start_time);
    if (result != 0)
    {
        return result;
    }

    g_start_time  = start_time;
    g_initialized = true;

    return 0;
}

/****************************** 时间查询 ******************************/

/**
 * @brief 获取单调时钟微秒数。
 */
uint64_t linkg_time_monotonic_us(void)
{
    struct timespec current_time;

    if (_get_monotonic_time(&current_time) != 0)
    {
        return 0;
    }

    return _timespec_to_us(&current_time);
}

/**
 * @brief 获取程序已运行的微秒数。
 */
uint64_t linkg_time_elapsed_us(void)
{
    struct timespec current_time;

    if (!g_initialized)
    {
        return 0;
    }

    if (_get_monotonic_time(&current_time) != 0)
    {
        return 0;
    }

    return _timespec_diff_us(&g_start_time, &current_time);
}

/**
 * @brief 获取程序已运行的毫秒数。
 */
uint64_t linkg_time_elapsed_ms(void)
{
    return linkg_time_elapsed_us() / 1000ULL;
}

/****************************** 时间格式化 ******************************/

/**
 * @brief 将毫秒数格式化为可读时间字符串。
 */
int  linkg_time_format_ms(uint64_t total_ms, char *buffer, size_t buffer_size)
{
    uint64_t total_seconds;
    uint64_t hours;
    uint32_t minutes;
    uint32_t seconds;
    uint32_t milliseconds;
    int      length;

    if (buffer == NULL || buffer_size == 0U)
    {
        return -EINVAL;
    }

    total_seconds = total_ms / LINKG_MS_PER_SECOND;
    hours         = total_seconds / 3600ULL;
    minutes       = (uint32_t)((total_seconds % 3600ULL) / 60ULL);
    seconds       = (uint32_t)(total_seconds % 60ULL);
    milliseconds  = (uint32_t)(total_ms % LINKG_MS_PER_SECOND);

    length = snprintf(buffer,
                      buffer_size,
                      "%02" PRIu64 ":%02u:%02u.%03u",
                      hours,
                      minutes,
                      seconds,
                      milliseconds);

    if (length < 0)
    {
        buffer[0] = '\0';
        return -EIO;
    }

    if ((size_t)length >= buffer_size)
    {
        buffer[0] = '\0';
        return -ENOSPC;
    }

    return 0;
}

/**
 * @brief 将微秒数格式化为可读时间字符串。
 */
int  linkg_time_format_us(uint64_t total_us, char *buffer, size_t buffer_size)
{
    uint64_t total_seconds;
    uint64_t hours;
    uint32_t minutes;
    uint32_t seconds;
    uint32_t microseconds;
    int      length;

    if (buffer == NULL || buffer_size == 0U)
    {
        return -EINVAL;
    }

    total_seconds = total_us / LINKG_US_PER_SECOND;
    hours         = total_seconds / 3600ULL;
    minutes       = (uint32_t)((total_seconds % 3600ULL) / 60ULL);
    seconds       = (uint32_t)(total_seconds % 60ULL);
    microseconds  = (uint32_t)(total_us % LINKG_US_PER_SECOND);

    length = snprintf(buffer,
                      buffer_size,
                      "%02" PRIu64 ":%02u:%02u.%06u",
                      hours,
                      minutes,
                      seconds,
                      microseconds);

    if (length < 0)
    {
        buffer[0] = '\0';
        return -EIO;
    }

    if ((size_t)length >= buffer_size)
    {
        buffer[0] = '\0';
        return -ENOSPC;
    }

    return 0;
}

/****************************** 时间等待 ******************************/

/**
 * @brief 按毫秒执行相对等待。
 */
int  linkg_time_sleep_ms(uint32_t milliseconds)
{
    return _sleep_relative_us((uint64_t)milliseconds * 1000ULL);
}

/**
 * @brief 按微秒执行相对等待。
 */
int  linkg_time_sleep_us(uint32_t microseconds)
{
    return _sleep_relative_us((uint64_t)microseconds);
}

/**
 * @brief 等待到指定的单调时钟时间点。
 */
int  linkg_time_sleep_until_us(uint64_t deadline_us)
{
    struct timespec deadline;
    int             result;

    _us_to_timespec(deadline_us, &deadline);

    do
    {
        result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    } while (result == EINTR);

    return result == 0 ? 0 : -result;
}

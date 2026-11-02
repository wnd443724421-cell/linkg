/**
 * @file test_common.c
 * @brief LinkG V1独立测试公共实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-02
 */

#include "test_assert.h"

#include <stdio.h>

#include "linkg_log.h"

uint32_t g_linkg_test_assertions = 0U;
uint32_t g_linkg_test_failures   = 0U;
uint32_t g_linkg_test_cases      = 0U;

void linkg_test_fail(const char *file, int line, const char *expression, long long actual, long long expected)
{
    g_linkg_test_failures++;
    fprintf(stderr,
            "FAIL %s:%d: %s actual=%lld expected=%lld\n",
            file,
            line,
            expression,
            actual,
            expected);
}

void linkg_test_run(const char *name, linkg_test_case_func_t function)
{
    uint32_t failures_before;

    failures_before = g_linkg_test_failures;
    g_linkg_test_cases++;
    function();

    printf("%s %s\n",
           failures_before == g_linkg_test_failures ? "PASS" : "FAIL",
           name);
}

int linkg_test_finish(const char *suite)
{
    printf("RESULT suite=%s cases=%u assertions=%u failures=%u\n",
           suite,
           g_linkg_test_cases,
           g_linkg_test_assertions,
           g_linkg_test_failures);

    return g_linkg_test_failures == 0U ? 0 : 1;
}

void linkg_log_write_internal(linkg_log_level_t level, const char *file, int line, const char *format, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

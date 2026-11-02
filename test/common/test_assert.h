/**
 * @file test_assert.h
 * @brief LinkG V1独立测试断言接口
 */

#ifndef LINKG_TEST_ASSERT_H
#define LINKG_TEST_ASSERT_H

#include <stddef.h>
#include <stdint.h>

typedef void (*linkg_test_case_func_t)(void);

extern uint32_t g_linkg_test_assertions;
extern uint32_t g_linkg_test_failures;
extern uint32_t g_linkg_test_cases;

void linkg_test_fail(const char *file, int line, const char *expression, long long actual, long long expected);
void linkg_test_run(const char *name, linkg_test_case_func_t function);
int  linkg_test_finish(const char *suite);

#define TEST_CHECK(expression)                                                                 \
    do                                                                                         \
    {                                                                                          \
        g_linkg_test_assertions++;                                                              \
        if (!(expression))                                                                     \
        {                                                                                      \
            linkg_test_fail(__FILE__, __LINE__, #expression, 0, 1);                            \
            return;                                                                            \
        }                                                                                      \
    } while (0)

#define TEST_CHECK_EQ(actual_value, expected_value)                                             \
    do                                                                                         \
    {                                                                                          \
        long long test_actual   = (long long)(actual_value);                                   \
        long long test_expected = (long long)(expected_value);                                 \
        g_linkg_test_assertions++;                                                              \
        if (test_actual != test_expected)                                                      \
        {                                                                                      \
            linkg_test_fail(__FILE__, __LINE__, #actual_value, test_actual, test_expected);     \
            return;                                                                            \
        }                                                                                      \
    } while (0)

#define TEST_RUN(function) linkg_test_run(#function, function)

#endif

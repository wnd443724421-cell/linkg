/**
 * @file linkg_time.h
 * @brief LinkG通用时间接口
 */

#ifndef LINKG_TIME_H
#define LINKG_TIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 宏定义 ******************************/

#define LINKG_TIME_MS_STR_SIZE 24U // 毫秒时间字符串缓冲区大小
#define LINKG_TIME_US_STR_SIZE 24U // 微秒时间字符串缓冲区大小

/****************************** 生命周期 ******************************/

int linkg_time_init(void);

/****************************** 时间获取 ******************************/

uint64_t linkg_time_monotonic_us(void);
uint64_t linkg_time_elapsed_ms(void);
uint64_t linkg_time_elapsed_us(void);

/****************************** 时间格式化 ******************************/

int linkg_time_format_ms(uint64_t total_ms, char *buffer, size_t buffer_size);
int linkg_time_format_us(uint64_t total_us, char *buffer, size_t buffer_size);

/****************************** 线程睡眠 ******************************/

int linkg_time_sleep_ms(uint32_t milliseconds);
int linkg_time_sleep_us(uint32_t microseconds);
int linkg_time_sleep_until_us(uint64_t deadline_us);

#ifdef __cplusplus
}
#endif

#endif // LINKG_TIME_H

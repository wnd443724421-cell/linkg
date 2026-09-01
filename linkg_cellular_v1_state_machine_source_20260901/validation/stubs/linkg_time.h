#ifndef LINKG_TIME_H
#define LINKG_TIME_H
#include <stdint.h>
uint64_t linkg_time_monotonic_us(void);
uint64_t linkg_time_elapsed_ms(void);
uint64_t linkg_time_elapsed_us(void);
int      linkg_time_sleep_ms(uint32_t milliseconds);
int      linkg_time_sleep_us(uint32_t microseconds);
int      linkg_time_sleep_until_us(uint64_t deadline_us);
#endif

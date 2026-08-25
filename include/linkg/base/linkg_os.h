/**
 * @file linkg_os.h
 * @brief LinkG通用进程与系统命令接口
 */

#ifndef LINKG_OS_H
#define LINKG_OS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 编译器属性 ******************************/

#if defined(__GNUC__) || defined(__clang__)
#define LINKG_OS_SENTINEL    __attribute__((sentinel))              // 检查可变参数以NULL结束
#define LINKG_OS_PRINTF(a, b) __attribute__((format(printf, a, b))) // printf格式检查
#else
#define LINKG_OS_SENTINEL                                              // 空属性
#define LINKG_OS_PRINTF(a, b)                                          // 空属性
#endif

/****************************** 程序执行 ******************************/

int  linkg_os_run(const char *file, ...) LINKG_OS_SENTINEL;
void linkg_os_run_ignore(const char *file, ...) LINKG_OS_SENTINEL;
int  linkg_os_shellf(const char *format, ...) LINKG_OS_PRINTF(1, 2);

/****************************** 进程管理 ******************************/

int linkg_os_spawn(pid_t *process_id, const char *file, ...) LINKG_OS_SENTINEL;
int linkg_os_process_running(pid_t *process_id, bool *running);
int linkg_os_process_stop(pid_t *process_id, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // LINKG_OS_H

/**
 * @file linkg_thread.h
 * @brief LinkG通用线程接口
 */

#ifndef LINKG_THREAD_H
#define LINKG_THREAD_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 宏定义 ******************************/

#define LINKG_THREAD_NAME_MAX 16U // Linux线程名称最大长度，包含结束符

/****************************** 类型定义 ******************************/

typedef struct linkg_thread linkg_thread_t;                                      // 通用线程对象
typedef void (*linkg_thread_func_t)(linkg_thread_t *thread, void *user_data);    // 用户线程函数

typedef struct
{
    size_t stack_size;          // 线程栈大小，0表示使用系统默认值
    int    cpu_core;            // CPU核心编号
    int    sched_policy;        // POSIX调度策略
    int    sched_priority;      // 实时调度优先级
    bool   affinity_enabled;    // 是否启用CPU亲和性
    bool   scheduling_enabled;  // 是否启用显式调度配置
} linkg_thread_config_t;        // 线程启动配置

struct linkg_thread
{
    char                name[LINKG_THREAD_NAME_MAX]; // 线程名称
    pthread_t           tid;                         // POSIX线程句柄
    linkg_thread_func_t function;                    // 用户线程函数
    void               *user_data;                  // 用户私有数据
    int                 wakeup_fd;                  // 线程唤醒描述符
    size_t              stack_size;                 // 线程栈大小
    int                 cpu_core;                   // CPU核心编号
    int                 sched_policy;               // POSIX调度策略
    int                 sched_priority;             // 实时调度优先级
    _Atomic bool        started;                    // 是否已经启动
    _Atomic bool        running;                    // 是否继续运行
    bool                affinity_enabled;           // 是否启用CPU亲和性
    bool                scheduling_enabled;         // 是否启用显式调度配置
    bool                initialized;                // 是否已经初始化
};

/****************************** 生命周期 ******************************/

int  linkg_thread_init(linkg_thread_t *thread, const char *name, linkg_thread_func_t function, void *user_data);
int  linkg_thread_init_with_config(linkg_thread_t *thread, const char *name, linkg_thread_func_t function, void *user_data, const linkg_thread_config_t *config);
int  linkg_thread_start(linkg_thread_t *thread);

/**
 * @brief 请求线程停止并等待线程资源回收。
 *
 * @return 0表示返回时不存在尚未join的活动线程；
 *         负值表示无法确认线程已经完成停止。
 *
 * @note pthread_join成功后的wakeup_fd收尾异常只记录日志，
 *       不改变线程已经完成停止的返回语义。
 */
int  linkg_thread_stop(linkg_thread_t *thread);
void linkg_thread_deinit(linkg_thread_t *thread);

/****************************** 状态查询 ******************************/

bool linkg_thread_is_started(const linkg_thread_t *thread);
bool linkg_thread_is_running(const linkg_thread_t *thread);

/****************************** 线程唤醒 ******************************/

int linkg_thread_get_wakeup_fd(const linkg_thread_t *thread);
int linkg_thread_wakeup(linkg_thread_t *thread);
int linkg_thread_clear_wakeup(linkg_thread_t *thread);

#ifdef __cplusplus
}
#endif

#endif // LINKG_THREAD_H

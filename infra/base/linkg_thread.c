/**
 * @file linkg_thread.c
 * @brief LinkG线程管理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-28
 */

#define _GNU_SOURCE

#include "linkg_thread.h"

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "linkg_log.h"

/****************************** 日志定义 ******************************/

#define LINKG_THREAD_LOG_TAG "THREAD" // 线程模块日志标签

#define LINKG_THREAD_WARN(fmt, ...)  LINKG_LOG_WARN("%s: " fmt, LINKG_THREAD_LOG_TAG, ##__VA_ARGS__) // 线程模块警告日志
#define LINKG_THREAD_ERROR(fmt, ...) LINKG_LOG_ERROR("%s: " fmt, LINKG_THREAD_LOG_TAG, ##__VA_ARGS__) // 线程模块错误日志

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取线程日志名称。
 */
static const char *_thread_name(const linkg_thread_t *thread)
{
    if (thread == NULL || thread->name[0] == '\0')
    {
        return "unnamed";
    }

    return thread->name;
}

/**
 * @brief 向 eventfd 写入一次唤醒信号。
 *
 * @note 正常只写入一次，仅在系统调用被信号中断时重试。
 */
static int _eventfd_write(int wakeup_fd)
{
    uint64_t value;
    ssize_t  length;
    int      saved_errno;

    if (wakeup_fd < 0)
    {
        return -EBADF;
    }

    value = 1U;

    do
    {
        length = write(wakeup_fd, &value, sizeof(value));
    } while (length < 0 && errno == EINTR);

    if (length == (ssize_t)sizeof(value))
    {
        return 0;
    }

    /**
     * eventfd计数器已满时说明已经存在待处理唤醒，
     * 被等待线程最终仍然会被唤醒，因此视为成功。
     */
    if (length < 0 && errno == EAGAIN)
    {
        return 0;
    }

    saved_errno = errno;
    return saved_errno != 0 ? -saved_errno : -EIO;
}

/**
 * @brief 消费 eventfd 当前累计的唤醒信号。
 *
 * @note 普通 eventfd 一次读取会返回累计值，并将计数器清零。
 */
static int _eventfd_clear(int wakeup_fd)
{
    uint64_t value;
    ssize_t length;

    if (wakeup_fd < 0)
    {
        return -EBADF;
    }

    do
    {
        length = read(wakeup_fd, &value, sizeof(value));
    }
    while (length < 0 && errno == EINTR);

    if (length == (ssize_t)sizeof(value))
    {
        return 0;
    }

    if (length < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 0;
        }

        return -errno;
    }

    return -EIO;
}

/**
 * @brief 用户线程入口包装函数。
 */
static void *_thread_entry(void *argument)
{
    linkg_thread_t *thread;
    int             result;

    thread = (linkg_thread_t *)argument;
    if (thread == NULL)
    {
        LINKG_THREAD_ERROR("%s", "thread entry received null argument");
        return NULL;
    }

    if (thread->name[0] != '\0')
    {
        result = pthread_setname_np(pthread_self(), thread->name);
        if (result != 0)
        {
            LINKG_THREAD_WARN("set thread name failed, name=%s, error=%d",
                              thread->name,
                              result);
        }
    }

    thread->function(thread, thread->user_data);

    /**
     * 用户线程函数已经退出。
     * started不能在此处清除，必须等待stop()完成pthread_join()。
     */
    atomic_store(&thread->running, false);
    return NULL;
}

/**
 * @brief 初始化线程对象内部实现。
 */
static int _thread_init(linkg_thread_t *thread, const char *name, linkg_thread_func_t function, void *user_data, const linkg_thread_config_t *config)
{
    long   cpu_count;
    size_t name_length;
    int    priority_max;
    int    priority_min;
    int    result;

    if (thread == NULL || function == NULL)
    {
        LINKG_THREAD_WARN("invalid initialization arguments, thread=%p, function=%s",
                          (void *)thread,
                          function == NULL ? "null" : "valid");

        return -EINVAL;
    }

    /**
     * 线程对象首次使用前必须零初始化。
     * 已经初始化的线程对象不能重复初始化。
     */
    if (thread->initialized)
    {
        LINKG_THREAD_WARN("thread already initialized, name=%s", _thread_name(thread));
        return -EALREADY;
    }

    name_length = 0U;

    if (name != NULL)
    {
        name_length = strnlen(name, LINKG_THREAD_NAME_MAX);
        if (name_length >= LINKG_THREAD_NAME_MAX)
        {
            LINKG_THREAD_WARN("thread name too long, name=%s, max=%u",
                              name,
                              LINKG_THREAD_NAME_MAX - 1U);

            return -ENAMETOOLONG;
        }
    }

    memset(thread, 0, sizeof(*thread));

    thread->function           = function;
    thread->user_data          = user_data;
    thread->wakeup_fd          = -1;
    thread->stack_size         = 0U;
    thread->cpu_core           = -1;
    thread->sched_policy       = SCHED_OTHER;
    thread->sched_priority     = 0;
    thread->affinity_enabled   = false;
    thread->scheduling_enabled = false;

    if (name_length > 0U)
    {
        memcpy(thread->name, name, name_length);
        thread->name[name_length] = '\0';
    }

    if (config != NULL)
    {
        thread->stack_size         = config->stack_size;
        thread->cpu_core           = config->cpu_core;
        thread->sched_policy       = config->sched_policy;
        thread->sched_priority     = config->sched_priority;
        thread->affinity_enabled   = config->affinity_enabled;
        thread->scheduling_enabled = config->scheduling_enabled;
    }

    if (thread->affinity_enabled)
    {
        if (thread->cpu_core < 0 || thread->cpu_core >= CPU_SETSIZE)
        {
            LINKG_THREAD_WARN("invalid CPU core, name=%s, core=%d, max=%d",
                              _thread_name(thread),
                              thread->cpu_core,
                              CPU_SETSIZE - 1);

            return -EINVAL;
        }

        cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpu_count > 0 && thread->cpu_core >= cpu_count)
        {
            LINKG_THREAD_WARN("CPU core unavailable, name=%s, core=%d, online=%ld",
                              _thread_name(thread),
                              thread->cpu_core,
                              cpu_count);

            return -EINVAL;
        }
    }

    if (thread->scheduling_enabled)
    {
        if (thread->sched_policy != SCHED_OTHER &&
            thread->sched_policy != SCHED_FIFO &&
            thread->sched_policy != SCHED_RR)
        {
            LINKG_THREAD_WARN("invalid scheduling policy, name=%s, policy=%d",
                            _thread_name(thread),
                            thread->sched_policy);

            return -EINVAL;
        }

        priority_min = sched_get_priority_min(thread->sched_policy);
        priority_max = sched_get_priority_max(thread->sched_policy);

        if (priority_min < 0 || priority_max < 0)
        {
            LINKG_THREAD_WARN("query scheduling priority range failed, name=%s, policy=%d",
                            _thread_name(thread),
                            thread->sched_policy);

            return -EINVAL;
        }

        if (thread->sched_priority < priority_min || thread->sched_priority > priority_max)
        {
            LINKG_THREAD_WARN("invalid scheduling priority, name=%s, policy=%d, priority=%d, range=%d-%d",
                            _thread_name(thread),
                            thread->sched_policy,
                            thread->sched_priority,
                            priority_min,
                            priority_max);

            return -EINVAL;
        }
    }

    atomic_init(&thread->started, false);
    atomic_init(&thread->running, false);

    thread->wakeup_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    if (thread->wakeup_fd < 0)
    {
        result = errno != 0 ? -errno : -EIO;

        LINKG_THREAD_ERROR("create eventfd failed, name=%s, error=%d",
                           _thread_name(thread),
                           result);

        return result;
    }

    thread->initialized = true;
    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 使用默认配置初始化线程对象。
 */
int  linkg_thread_init(linkg_thread_t *thread, const char *name, linkg_thread_func_t function, void *user_data)
{
    return _thread_init(thread, name, function, user_data, NULL);
}

/**
 * @brief 使用指定配置初始化线程对象。
 */
int  linkg_thread_init_with_config(linkg_thread_t *thread, const char *name, linkg_thread_func_t function, void *user_data, const linkg_thread_config_t *config)
{
    if (config == NULL)
    {
        LINKG_THREAD_WARN("thread configuration is null, name=%s",
                          name != NULL ? name : "unnamed");

        return -EINVAL;
    }

    return _thread_init(thread, name, function, user_data, config);
}

/**
 * @brief 启动线程。
 */
int  linkg_thread_start(linkg_thread_t *thread)
{
    pthread_attr_t     attributes;
    struct sched_param sched_param;
    cpu_set_t          cpu_set;
    int                result;

    if (thread == NULL || !thread->initialized || thread->function == NULL)
    {
        LINKG_THREAD_WARN("invalid thread start request, thread=%p", (void *)thread);
        return -EINVAL;
    }

    if (atomic_load(&thread->started))
    {
        LINKG_THREAD_WARN("thread already started, name=%s", _thread_name(thread));
        return -EALREADY;
    }

    // 清除上一次运行周期中可能残留的唤醒信号。
    result = _eventfd_clear(thread->wakeup_fd);
    if (result != 0)
    {
        LINKG_THREAD_ERROR("clear wakeup before start failed, name=%s, fd=%d, error=%d",
                           _thread_name(thread),
                           thread->wakeup_fd,
                           result);

        return result;
    }

    result = pthread_attr_init(&attributes);
    if (result != 0)
    {
        LINKG_THREAD_ERROR("initialize thread attributes failed, name=%s, error=%d",
                           _thread_name(thread),
                           result);

        return -result;
    }

    if (thread->stack_size > 0U)
    {
        result = pthread_attr_setstacksize(&attributes, thread->stack_size);
        if (result != 0)
        {
            LINKG_THREAD_ERROR("set thread stack failed, name=%s, size=%zu, error=%d",
                               _thread_name(thread),
                               thread->stack_size,
                               result);

            (void)pthread_attr_destroy(&attributes);
            return -result;
        }
    }

    if (thread->scheduling_enabled)
    {
        /**
         * 必须使用显式调度属性。
         * 否则新线程可能继续继承创建线程的SCHED_OTHER策略。
         */
        result = pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
        if (result != 0)
        {
            LINKG_THREAD_ERROR("set explicit scheduling failed, name=%s, error=%d",
                            _thread_name(thread),
                            result);

            (void)pthread_attr_destroy(&attributes);
            return -result;
        }

        result = pthread_attr_setschedpolicy(&attributes, thread->sched_policy);
        if (result != 0)
        {
            LINKG_THREAD_ERROR("set scheduling policy failed, name=%s, policy=%d, error=%d",
                            _thread_name(thread),
                            thread->sched_policy,
                            result);

            (void)pthread_attr_destroy(&attributes);
            return -result;
        }

        memset(&sched_param, 0, sizeof(sched_param));
        sched_param.sched_priority = thread->sched_priority;

        result = pthread_attr_setschedparam(&attributes, &sched_param);
        if (result != 0)
        {
            LINKG_THREAD_ERROR("set scheduling priority failed, name=%s, priority=%d, error=%d",
                            _thread_name(thread),
                            thread->sched_priority,
                            result);

            (void)pthread_attr_destroy(&attributes);
            return -result;
        }
    }

    if (thread->affinity_enabled)
    {
        CPU_ZERO(&cpu_set);
        CPU_SET(thread->cpu_core, &cpu_set);

        result = pthread_attr_setaffinity_np(&attributes, sizeof(cpu_set), &cpu_set);
        if (result != 0)
        {
            LINKG_THREAD_ERROR("set thread affinity failed, name=%s, core=%d, error=%d",
                               _thread_name(thread),
                               thread->cpu_core,
                               result);

            (void)pthread_attr_destroy(&attributes);
            return -result;
        }
    }

    atomic_store(&thread->running, true);

    result = pthread_create(&thread->tid, &attributes, _thread_entry, thread);
    (void)pthread_attr_destroy(&attributes);

    if (result != 0)
    {
        memset(&thread->tid, 0, sizeof(thread->tid));
        atomic_store(&thread->running, false);

        LINKG_THREAD_ERROR("create thread failed, name=%s, error=%d",
                           _thread_name(thread),
                           result);

        return -result;
    }

    atomic_store(&thread->started, true);
    return 0;
}

/**
 * @brief 请求线程停止并等待线程资源回收。
 *
 * @return 0表示返回时不存在尚未join的活动线程；
 *         负值表示无法确认线程已经完成停止。
 *
 * @note pthread_join成功后的wakeup_fd收尾异常仅记录日志，
 *       不改变线程已经完成停止的返回语义。
 * @note 不允许在线程自身调用，否则返回-EDEADLK；业务线程需要主动结束时应直接退出线程函数。
 */
int  linkg_thread_stop(linkg_thread_t *thread)
{
    int result;

    if (thread == NULL || !thread->initialized)
    {
        LINKG_THREAD_WARN("invalid thread stop request, thread=%p", (void *)thread);
        return -EINVAL;
    }

    if (!atomic_load(&thread->started))
    {
        atomic_store(&thread->running, false);

        result = _eventfd_clear(thread->wakeup_fd);
        if (result != 0)
        {
            LINKG_THREAD_ERROR("clear inactive thread wakeup failed, name=%s, fd=%d, error=%d",
                               _thread_name(thread),
                               thread->wakeup_fd,
                               result);
        }

        return 0;
    }

    /**
     * 不能在线程内部join自身。
     * 用户线程需要主动结束时，直接退出自己的线程函数即可。
     */
    if (pthread_equal(pthread_self(), thread->tid))
    {
        LINKG_THREAD_ERROR("thread attempted to join itself, name=%s",
                           _thread_name(thread));

        return -EDEADLK;
    }

    /**
     * 必须先清除运行标志，再发送唤醒。
     * 线程从poll/epoll中醒来后即可识别停止请求。
     */
    atomic_store(&thread->running, false);

    result = linkg_thread_wakeup(thread);
    if (result != 0)
    {
        LINKG_THREAD_ERROR("wake thread for stop failed, name=%s, fd=%d, error=%d",
                           _thread_name(thread),
                           thread->wakeup_fd,
                           result);

        return result;
    }

    result = pthread_join(thread->tid, NULL);
    if (result != 0)
    {
        LINKG_THREAD_ERROR("join thread failed, name=%s, error=%d",
                           _thread_name(thread),
                           result);

        return -result;
    }

    memset(&thread->tid, 0, sizeof(thread->tid));
    atomic_store(&thread->started, false);

    /**
     * 业务线程正常情况下会自行消费停止唤醒，
     * 这里再次清除可保证下一次start()没有历史信号。
     * 清理失败只影响下一次start()前的唤醒状态，不影响线程已join的事实。
     */
    result = _eventfd_clear(thread->wakeup_fd);
    if (result != 0)
    {
        LINKG_THREAD_ERROR("clear wakeup after stop failed, name=%s, fd=%d, error=%d",
                           _thread_name(thread),
                           thread->wakeup_fd,
                           result);
    }

    return 0;
}

/**
 * @brief 销毁线程对象。
 */
void linkg_thread_deinit(linkg_thread_t *thread)
{
    int result;

    if (thread == NULL || !thread->initialized)
    {
        return;
    }

    if (atomic_load(&thread->started))
    {
        result = linkg_thread_stop(thread);
        if (result != 0)
        {
            LINKG_THREAD_ERROR("stop thread during deinit failed, name=%s, error=%d",
                               _thread_name(thread),
                               result);

            return;
        }
    }

    if (thread->wakeup_fd >= 0)
    {
        if (close(thread->wakeup_fd) != 0)
        {
            LINKG_THREAD_ERROR("close thread eventfd failed, name=%s, fd=%d, error=%d",
                               _thread_name(thread),
                               thread->wakeup_fd,
                               errno != 0 ? -errno : -EIO);
        }
    }

    memset(thread, 0, sizeof(*thread));

    /**
     * 保持无效描述符和无效CPU编号语义明确，
     * 便于调试时观察线程对象状态。
     */
    thread->wakeup_fd = -1;
    thread->cpu_core  = -1;
}

/****************************** 状态查询 ******************************/

/**
 * @brief 查询线程是否已经创建且尚未完成 join。
 */
bool linkg_thread_is_started(const linkg_thread_t *thread)
{
    return thread != NULL &&
           thread->initialized &&
           atomic_load(&thread->started);
}

/**
 * @brief 查询用户业务循环是否应继续运行。
 */
bool linkg_thread_is_running(const linkg_thread_t *thread)
{
    return thread != NULL &&
           thread->initialized &&
           atomic_load(&thread->running);
}

/**
 * @brief 获取线程通用唤醒描述符。
 */
int  linkg_thread_get_wakeup_fd(const linkg_thread_t *thread)
{
    if (thread == NULL || !thread->initialized)
    {
        return -1;
    }

    return thread->wakeup_fd;
}

/****************************** 唤醒控制 ******************************/

/**
 * @brief 唤醒正在阻塞等待的线程。
 */
int  linkg_thread_wakeup(linkg_thread_t *thread)
{
    int result;

    if (thread == NULL || !thread->initialized)
    {
        LINKG_THREAD_WARN("invalid thread wakeup request, thread=%p", (void *)thread);
        return -EINVAL;
    }

    if (!atomic_load(&thread->started))
    {
        LINKG_THREAD_WARN("cannot wake inactive thread, name=%s", _thread_name(thread));
        return -EINVAL;
    }

    result = _eventfd_write(thread->wakeup_fd);
    if (result != 0)
    {
        LINKG_THREAD_ERROR("write thread wakeup failed, name=%s, fd=%d, error=%d",
                           _thread_name(thread),
                           thread->wakeup_fd,
                           result);
    }

    return result;
}

/**
 * @brief 消费当前待处理的唤醒信号。
 */
int  linkg_thread_clear_wakeup(linkg_thread_t *thread)
{
    int result;

    if (thread == NULL || !thread->initialized)
    {
        LINKG_THREAD_WARN("invalid clear wakeup request, thread=%p", (void *)thread);
        return -EINVAL;
    }

    result = _eventfd_clear(thread->wakeup_fd);
    if (result != 0)
    {
        LINKG_THREAD_ERROR("clear thread wakeup failed, name=%s, fd=%d, error=%d",
                           _thread_name(thread),
                           thread->wakeup_fd,
                           result);
    }

    return result;
}

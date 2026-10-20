/**
 * @file linkg_upgrade.c
 * @brief LinkG升级安装任务调度与状态管理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#define _GNU_SOURCE

#include "linkg_upgrade.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linkg_thread.h"

#include "upgrade_archive.h"
#include "upgrade_manifest.h"

/****************************** 模块常量 ******************************/

#define LINKG_UPGRADE_THREAD_NAME        "linkg_upgrade"                      // 升级安装线程名称
#define LINKG_UPGRADE_UPLOAD_PATH        "/tmp/upgrade.tar.gz"                // CGI上传完成的升级包
#define LINKG_UPGRADE_UPLOAD_LOCK_PATH   "/tmp/.linkg-upload.lock"            // 与CGI共用的上传互斥锁
#define LINKG_UPGRADE_TASK_TEMPLATE      "/tmp/.linkg-upgrade-task.XXXXXX"    // 单次升级包私有路径
#define LINKG_UPGRADE_MAX_PACKAGE_BYTES  (64U * 1024U * 1024U)                // 升级包压缩大小上限，单位字节

/****************************** 内部类型 ******************************/

typedef struct
{
    pthread_mutex_t        lifecycle_lock;         // 串行化初始化、启动、停止和反初始化
    pthread_mutex_t        lock;                   // 保护任务状态和待处理升级包路径
    linkg_thread_t         worker;                 // 升级专用线程，空闲时阻塞在eventfd
    linkg_upgrade_status_t status;                 // 最近一次升级任务状态快照
    char                   package_path[PATH_MAX]; // 已接管升级包的私有路径
    bool                   initialized;            // 模块已经初始化
    bool                   started;                // 模块允许接受新的升级请求
    bool                   stopping;               // 模块正在停止，拒绝新的升级请求
    bool                   task_active;            // 已接受的升级任务尚未完成
} linkg_upgrade_context_t;

/****************************** 全局上下文 ******************************/

static linkg_upgrade_context_t g_upgrade =
{
    .lifecycle_lock = PTHREAD_MUTEX_INITIALIZER, // 生命周期独立互斥锁
    .lock           = PTHREAD_MUTEX_INITIALIZER, // 运行状态互斥锁
    .initialized    = false,                     // 尚未初始化
    .started        = false,                     // 尚未启动
    .stopping       = false,                     // 未请求停止
    .task_active    = false                      // 尚无待执行任务
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 在状态锁保护下记录本次升级任务的失败原因。
 */
static void _linkg_upgrade_set_failed_locked(linkg_upgrade_stage_t stage, int error_code, const char *message)
{
    g_upgrade.status.state      = LINKG_UPGRADE_STATE_FAILED;
    g_upgrade.status.stage      = stage;
    g_upgrade.status.error_code = error_code;
    g_upgrade.status.progress   = 0U;

    (void)snprintf(g_upgrade.status.message, sizeof(g_upgrade.status.message), "%s", message);
}

/**
 * @brief 在CGI上传锁下将完整上传包原子接管为本次任务私有文件。
 *
 * 成功返回时调用方持有upload_lock_fd，须在发布或撤销任务后关闭。
 * 调用方必须持有g_upgrade.lock，避免停止流程与接受任务并发。
 */
static int _linkg_upgrade_claim_upload(char *task_path, size_t task_path_size, int *upload_lock_fd)
{
    struct stat info;
    int         lock_fd;
    int         source_fd;
    int         temporary_fd;
    int         length;
    int         ret;
    bool        temporary_created;

    if (task_path == NULL || task_path_size == 0U || upload_lock_fd == NULL)
    {
        return -EINVAL;
    }

    *upload_lock_fd    = -1;
    task_path[0]      = '\0';
    source_fd         = -1;
    temporary_fd      = -1;
    temporary_created = false;

    lock_fd = open(LINKG_UPGRADE_UPLOAD_LOCK_PATH, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0)
    {
        return -errno;
    }

    if (fstat(lock_fd, &info) != 0)
    {
        ret = -errno;
        goto failed;
    }

    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1)
    {
        ret = -EPERM;
        goto failed;
    }

    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
    {
        ret = errno == EWOULDBLOCK || errno == EAGAIN ? -EBUSY : -errno;
        goto failed;
    }

    source_fd = open(LINKG_UPGRADE_UPLOAD_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source_fd < 0)
    {
        ret = -errno;
        goto failed;
    }

    if (fstat(source_fd, &info) != 0)
    {
        ret = -errno;
        goto failed;
    }

    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size <= 0)
    {
        ret = -EINVAL;
        goto failed;
    }

    if ((uint64_t)info.st_size > LINKG_UPGRADE_MAX_PACKAGE_BYTES)
    {
        ret = -EFBIG;
        goto failed;
    }

    length = snprintf(task_path, task_path_size, "%s", LINKG_UPGRADE_TASK_TEMPLATE);
    if (length < 0 || (size_t)length >= task_path_size)
    {
        ret = -ENAMETOOLONG;
        goto failed;
    }

    temporary_fd = mkstemp(task_path);
    if (temporary_fd < 0)
    {
        ret = -errno;
        goto failed;
    }

    temporary_created = true;
    if (close(temporary_fd) != 0)
    {
        ret = -errno;
        temporary_fd = -1;
        goto failed;
    }

    temporary_fd = -1;
    if (rename(LINKG_UPGRADE_UPLOAD_PATH, task_path) != 0)
    {
        ret = -errno;
        goto failed;
    }

    temporary_created = false;
    close(source_fd);
    *upload_lock_fd = lock_fd;
    return 0;

failed:
    if (temporary_fd >= 0)
    {
        close(temporary_fd);
    }

    if (temporary_created)
    {
        (void)unlink(task_path);
    }

    if (source_fd >= 0)
    {
        close(source_fd);
    }

    close(lock_fd);
    task_path[0] = '\0';
    return ret;
}

/**
 * @brief 处理已接管的升级任务。
 *
 * 当前仅执行升级包结构检查和解压，不触碰App、Web、Kernel或Supervisor状态。
 * 已接入FILES.txt校验；Release安装留给后续子模块实现。
 */
static void _linkg_upgrade_process_package(const char *task_path)
{
    int ret;
    int cleanup_error;
    linkg_upgrade_stage_t failure_stage;

    // 校验归档根目录并提取到/tmp/upgrade。
    ret           = upgrade_archive_extract(task_path, "/tmp");
    failure_stage = LINKG_UPGRADE_STAGE_ARCHIVE_VERIFY;

    if (ret == 0)
    {
        pthread_mutex_lock(&g_upgrade.lock);
        g_upgrade.status.stage    = LINKG_UPGRADE_STAGE_MANIFEST_VERIFY;
        g_upgrade.status.progress = 15U;
        (void)snprintf(g_upgrade.status.message, sizeof(g_upgrade.status.message), "%s", "正在校验FILES.txt和文件SHA-256");
        pthread_mutex_unlock(&g_upgrade.lock);

        ret           = upgrade_manifest_verify("/tmp/upgrade");
        failure_stage = LINKG_UPGRADE_STAGE_MANIFEST_VERIFY;
    }

    cleanup_error = unlink(task_path) == 0 ? 0 : -errno;

    pthread_mutex_lock(&g_upgrade.lock);
    if (ret != 0)
    {
        _linkg_upgrade_set_failed_locked(failure_stage, ret, "升级包解压或完整性校验失败");
    }
    else if (cleanup_error != 0)
    {
        _linkg_upgrade_set_failed_locked(LINKG_UPGRADE_STAGE_MANIFEST_VERIFY, cleanup_error, "升级包临时文件清理失败");
    }
    else
    {
        _linkg_upgrade_set_failed_locked(LINKG_UPGRADE_STAGE_TARGET_PREPARE, -ENOSYS, "完整性校验通过，Release安装尚未实现");
    }

    g_upgrade.package_path[0] = '\0';
    g_upgrade.task_active     = false;
    pthread_mutex_unlock(&g_upgrade.lock);
}

/**
 * @brief 运行升级专用线程，空闲时无限期阻塞，收到Web请求后执行一次任务。
 *
 * 停止时优先完成已经接受的任务，再响应线程基类的退出请求。
 */
static void _linkg_upgrade_worker(linkg_thread_t *thread, void *user_data)
{
    struct pollfd descriptor;
    char          task_path[PATH_MAX];
    bool          pending;
    int           ret;

    (void)user_data;

    descriptor.fd      = linkg_thread_get_wakeup_fd(thread);
    descriptor.events  = POLLIN;
    descriptor.revents = 0;

    while (true)
    {
        pthread_mutex_lock(&g_upgrade.lock);
        pending = g_upgrade.task_active && g_upgrade.status.state == LINKG_UPGRADE_STATE_PENDING;

        if (pending)
        {
            memcpy(task_path, g_upgrade.package_path, sizeof(task_path));
            g_upgrade.status.state = LINKG_UPGRADE_STATE_VERIFYING;
            g_upgrade.status.stage = LINKG_UPGRADE_STAGE_ARCHIVE_VERIFY;
            (void)snprintf(g_upgrade.status.message, sizeof(g_upgrade.status.message), "%s", "开始处理升级包");
        }

        pthread_mutex_unlock(&g_upgrade.lock);

        if (pending)
        {
            _linkg_upgrade_process_package(task_path);
            continue;
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        do
        {
            descriptor.revents = 0;
            ret = poll(&descriptor, 1U, -1);
        }
        while (ret < 0 && errno == EINTR);

        if (ret > 0 && (descriptor.revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(thread);
            if (ret == 0)
            {
                continue;
            }
        }
        else if (ret > 0)
        {
            ret = -EIO;
        }
        else if (ret < 0)
        {
            ret = -errno;
        }
        else
        {
            continue;
        }

        pthread_mutex_lock(&g_upgrade.lock);
        _linkg_upgrade_set_failed_locked(LINKG_UPGRADE_STAGE_NONE, ret, "升级线程等待事件失败");
        pthread_mutex_unlock(&g_upgrade.lock);
        break;
    }
}

/**
 * @brief 停止接收请求，并等待已接受的升级任务处理完成。
 *
 * 必须持有lifecycle_lock调用，不能持有状态锁等待线程join。
 */
static int _linkg_upgrade_stop_worker(void)
{
    int ret;

    pthread_mutex_lock(&g_upgrade.lock);

    if (!g_upgrade.initialized)
    {
        pthread_mutex_unlock(&g_upgrade.lock);
        return 0;
    }

    g_upgrade.started  = false;
    g_upgrade.stopping = true;
    pthread_mutex_unlock(&g_upgrade.lock);

    ret = linkg_thread_stop(&g_upgrade.worker);

    pthread_mutex_lock(&g_upgrade.lock);
    if (ret == 0)
    {
        g_upgrade.stopping = false;
    }
    pthread_mutex_unlock(&g_upgrade.lock);

    return ret;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化升级模块状态与通用线程对象，暂不启动线程。
 */
int linkg_upgrade_init(void)
{
    int ret;

    pthread_mutex_lock(&g_upgrade.lifecycle_lock);
    pthread_mutex_lock(&g_upgrade.lock);

    if (g_upgrade.initialized)
    {
        pthread_mutex_unlock(&g_upgrade.lock);
        pthread_mutex_unlock(&g_upgrade.lifecycle_lock);
        return -EALREADY;
    }

    pthread_mutex_unlock(&g_upgrade.lock);
    ret = linkg_thread_init(&g_upgrade.worker, LINKG_UPGRADE_THREAD_NAME, _linkg_upgrade_worker, NULL);

    if (ret == 0)
    {
        pthread_mutex_lock(&g_upgrade.lock);
        memset(&g_upgrade.status, 0, sizeof(g_upgrade.status));
        g_upgrade.status.state    = LINKG_UPGRADE_STATE_IDLE;
        g_upgrade.package_path[0] = '\0';
        g_upgrade.initialized     = true;
        g_upgrade.started         = false;
        g_upgrade.stopping        = false;
        g_upgrade.task_active     = false;
        pthread_mutex_unlock(&g_upgrade.lock);
    }

    pthread_mutex_unlock(&g_upgrade.lifecycle_lock);
    return ret;
}

/**
 * @brief 启动升级专用线程并开始接受Web升级请求。
 */
int linkg_upgrade_start(void)
{
    int ret;

    pthread_mutex_lock(&g_upgrade.lifecycle_lock);
    pthread_mutex_lock(&g_upgrade.lock);

    if (!g_upgrade.initialized)
    {
        ret = -EINVAL;
    }
    else if (g_upgrade.stopping)
    {
        ret = -EBUSY;
    }
    else if (g_upgrade.started)
    {
        ret = -EALREADY;
    }
    else
    {
        ret = linkg_thread_start(&g_upgrade.worker);
        if (ret == 0)
        {
            g_upgrade.started = true;
        }
    }

    pthread_mutex_unlock(&g_upgrade.lock);
    pthread_mutex_unlock(&g_upgrade.lifecycle_lock);
    return ret;
}

/**
 * @brief 停止升级线程，等待已接受任务完成，避免中断关键写入。
 */
int linkg_upgrade_stop(void)
{
    int ret;

    pthread_mutex_lock(&g_upgrade.lifecycle_lock);
    ret = _linkg_upgrade_stop_worker();
    pthread_mutex_unlock(&g_upgrade.lifecycle_lock);

    return ret;
}

/**
 * @brief 停止线程并释放通用线程对象，不销毁静态互斥锁。
 */
int linkg_upgrade_deinit(void)
{
    int ret;

    pthread_mutex_lock(&g_upgrade.lifecycle_lock);
    ret = _linkg_upgrade_stop_worker();

    if (ret == 0)
    {
        pthread_mutex_lock(&g_upgrade.lock);
        if (g_upgrade.initialized)
        {
            linkg_thread_deinit(&g_upgrade.worker);
            memset(&g_upgrade.status, 0, sizeof(g_upgrade.status));
            g_upgrade.package_path[0] = '\0';
            g_upgrade.initialized = false;
            g_upgrade.started     = false;
            g_upgrade.stopping    = false;
            g_upgrade.task_active = false;
        }
        pthread_mutex_unlock(&g_upgrade.lock);
    }

    pthread_mutex_unlock(&g_upgrade.lifecycle_lock);
    return ret;
}

/****************************** 升级任务 ******************************/

/**
 * @brief 在CGI锁保护下接管上传包，并通过eventfd唤醒升级线程。
 */
int linkg_upgrade_request_install(void)
{
    linkg_upgrade_status_t previous_status;
    char                   task_path[PATH_MAX];
    uint32_t               next_id;
    int                    upload_lock_fd;
    int                    restore_error;
    int                    ret;

    pthread_mutex_lock(&g_upgrade.lock);

    if (!g_upgrade.initialized || !g_upgrade.started || g_upgrade.stopping ||
        !linkg_thread_is_running(&g_upgrade.worker))
    {
        pthread_mutex_unlock(&g_upgrade.lock);
        return -ESHUTDOWN;
    }

    if (g_upgrade.task_active || g_upgrade.status.state == LINKG_UPGRADE_STATE_REBOOT_PENDING)
    {
        pthread_mutex_unlock(&g_upgrade.lock);
        return -EBUSY;
    }

    ret = _linkg_upgrade_claim_upload(task_path, sizeof(task_path), &upload_lock_fd);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_upgrade.lock);
        return ret;
    }

    previous_status = g_upgrade.status;
    next_id         = previous_status.task_id + 1U;
    if (next_id == 0U)
    {
        next_id = 1U;
    }

    g_upgrade.status.task_id    = next_id;
    g_upgrade.status.state      = LINKG_UPGRADE_STATE_PENDING;
    g_upgrade.status.stage      = LINKG_UPGRADE_STAGE_NONE;
    g_upgrade.status.progress   = 0U;
    g_upgrade.status.error_code = 0;
    (void)snprintf(g_upgrade.status.message, sizeof(g_upgrade.status.message), "%s", "等待升级线程处理");
    (void)snprintf(g_upgrade.package_path, sizeof(g_upgrade.package_path), "%s", task_path);
    g_upgrade.task_active = true;

    ret = linkg_thread_wakeup(&g_upgrade.worker);
    if (ret != 0)
    {
        restore_error = rename(task_path, LINKG_UPGRADE_UPLOAD_PATH) == 0 ? 0 : -errno;
        g_upgrade.status          = previous_status;
        g_upgrade.package_path[0] = '\0';
        g_upgrade.task_active     = false;
        if (restore_error != 0)
        {
            _linkg_upgrade_set_failed_locked(LINKG_UPGRADE_STAGE_NONE, restore_error, "唤醒失败且无法恢复上传包");
        }
        close(upload_lock_fd);
        pthread_mutex_unlock(&g_upgrade.lock);
        return restore_error != 0 ? restore_error : ret;
    }

    close(upload_lock_fd);
    pthread_mutex_unlock(&g_upgrade.lock);
    return 0;
}

/****************************** 状态查询 ******************************/

/**
 * @brief 复制当前升级任务的线程安全状态快照。
 */
int linkg_upgrade_get_status(linkg_upgrade_status_t *status)
{
    if (status == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_upgrade.lock);

    if (!g_upgrade.initialized)
    {
        pthread_mutex_unlock(&g_upgrade.lock);
        return -ENODEV;
    }

    *status = g_upgrade.status;
    pthread_mutex_unlock(&g_upgrade.lock);
    return 0;
}

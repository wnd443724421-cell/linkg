/**
 * @file ota_driver.c
 * @brief 使用真实升级线程及Web处理器的隔离测试入口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linkg_json.h"
#include "linkg_lifecycle.h"
#include "linkg_log.h"
#include "linkg_release_state.h"
#include "linkg_supervisor_client.h"
#include "linkg_upgrade.h"
#include "web_internal.h"

/****************************** 测试状态 ******************************/

static volatile sig_atomic_t g_stop; // 测试退出请求

int __real_open(const char *path, int flags, ...);
int __real_fstat(int descriptor, struct stat *information);
int __real_fsync(int descriptor);
int __real_ioctl(int descriptor, unsigned long request, ...);

/**
 * @brief 将唯一的系统安装根目录重定向到本次临时目录。
 */
int __wrap_open(const char *path, int flags, ...)
{
    va_list arguments;
    mode_t  mode;

    mode = 0;
    if ((flags & O_CREAT) != 0)
    {
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    return __real_open(strcmp(path, "/") == 0 ? getenv("LINKG_OTA_TEST_SYSTEM_ROOT") : path, flags, mode);
}

/**
 * @brief 只有测试目录内的mtd文件视作Flash替身。
 */
static bool _ota_driver_mtd(int descriptor)
{
    char    link[64];
    char    path[4096];
    const char *root;
    ssize_t length;

    root = getenv("LINKG_OTA_TEST_DEV_ROOT");
    snprintf(link, sizeof(link), "/proc/self/fd/%d", descriptor);
    length = readlink(link, path, sizeof(path) - 1U);
    if (root == NULL || length <= 0)
    {
        return false;
    }
    path[length] = '\0';
    return strncmp(path, root, strlen(root)) == 0 && strncmp(path + strlen(root), "/mtd", 4U) == 0;
}

/**
 * @brief 与Linux 5.4 mtdchar一致，字符设备不提供fsync文件操作。
 */
int __wrap_fsync(int descriptor)
{
    if (_ota_driver_mtd(descriptor))
    {
        errno = EINVAL;
        return -1;
    }
    return __real_fsync(descriptor);
}

/**
 * @brief 模拟字符设备类型，产品代码仍完整检查MTD属性。
 */
int __wrap_fstat(int descriptor, struct stat *information)
{
    int ret;

    ret = __real_fstat(descriptor, information);
    if (ret == 0 && _ota_driver_mtd(descriptor))
    {
        information->st_mode = (information->st_mode & ~S_IFMT) | S_IFCHR;
    }
    return ret;
}

/**
 * @brief 使用普通文件模拟NOR属性和擦除，绝不调用真实Flash ioctl。
 */
int __wrap_ioctl(int descriptor, unsigned long request, ...)
{
    va_list arguments;
    void   *argument;
    mtd_info_t *information;
    struct erase_info_user *erase;
    unsigned char buffer[4096];
    uint32_t offset;

    va_start(arguments, request);
    argument = va_arg(arguments, void *);
    va_end(arguments);
    if (!_ota_driver_mtd(descriptor))
    {
        errno = ENODEV;
        return -1;
    }
    if (request == MEMGETINFO)
    {
        information = argument;
        memset(information, 0, sizeof(*information));
        information->type = MTD_NORFLASH;
        information->flags = MTD_WRITEABLE;
        information->size = 32768U;
        information->erasesize = 4096U;
        information->writesize = 1U;
        return 0;
    }
    if (request == MEMERASE)
    {
        linkg_release_state_t state;

        if (linkg_release_load(getenv("LINKG_OTA_TEST_STATE"), &state) != 0 || state.upgrade != LINKG_UPGRADE_PHASE_FLASHING)
        {
            errno = EPERM;
            return -1;
        }
        erase = argument;
        memset(buffer, 0xff, sizeof(buffer));
        for (offset = 0U; offset < erase->length; offset += sizeof(buffer))
        {
            if (pwrite(descriptor, buffer, sizeof(buffer), (off_t)(erase->start + offset)) != (ssize_t)sizeof(buffer))
            {
                return -1;
            }
        }
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

/**
 * @brief 重启仅记录测试标记，不执行真实系统调用。
 */
int linkg_lifecycle_request_exit(linkg_exit_action_t action)
{
    FILE *stream;

    if (action != LINKG_EXIT_ACTION_REBOOT_SYSTEM)
    {
        return -EINVAL;
    }
    stream = fopen(getenv("LINKG_OTA_TEST_REBOOT"), "w");
    if (stream == NULL)
    {
        return -errno;
    }
    fputs("reboot requested\n", stream);
    return fclose(stream) == 0 ? 0 : -errno;
}

/**
 * @brief 线程日志使用测试标准错误输出，不初始化真实日志目录。
 */
void linkg_log_write_internal(linkg_log_level_t level, const char *file, int line, const char *format, ...)
{
    va_list arguments;

    (void)level;
    (void)file;
    (void)line;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

/**
 * @brief 信号处理仅通知主循环退出。
 */
static void _ota_driver_stop(int signal_number)
{
    (void)signal_number;
    g_stop = 1;
}

/**
 * @brief stdin接收s开始、g查询命令，stdout输出真实Web响应。
 */
int main(void)
{
    struct pollfd descriptor;
    cJSON        *param;
    char         *response;
    char          command;
    int           ret;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, _ota_driver_stop);
    signal(SIGINT, _ota_driver_stop);
    if (linkg_supervisor_client_init() != 0 || linkg_upgrade_init() != 0 || linkg_upgrade_start() != 0 ||
        linkg_supervisor_client_ready() != 0)
    {
        return 2;
    }
    puts("DRIVER_READY");
    descriptor.fd = STDIN_FILENO;
    descriptor.events = POLLIN;
    while (!g_stop)
    {
        if (poll(&descriptor, 1U, 100) <= 0)
        {
            continue;
        }
        if ((descriptor.revents & POLLIN) == 0)
        {
            if ((descriptor.revents & POLLHUP) != 0)
            {
                break;
            }
            continue;
        }
        if (read(STDIN_FILENO, &command, 1U) != 1)
        {
            break;
        }
        param = cJSON_CreateObject();
        cJSON_AddBoolToObject(param, "confirm", true);
        response = NULL;
        ret = command == 's' ? _linkg_web_handler_upgrade_start(param, &response) : _linkg_web_handler_upgrade_status_get(param, &response);
        printf("RESPONSE %d %s\n", ret, response == NULL ? "null" : response);
        cJSON_Delete(param);
        linkg_json_string_free(response);
    }
    linkg_supervisor_client_deinit();
    linkg_upgrade_stop();
    linkg_upgrade_deinit();
    return 0;
}

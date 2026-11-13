/**
 * @file wifi_scan.c
 * @brief LinkG独占维护期间的有界被动信道扫描
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */

#define _GNU_SOURCE

#include "wifi_scan.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_os.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"

#include "wpa_ctrl.h"

/****************************** 模块常量 ******************************/

#define WIFI_SCAN_TEXT_MAX    32768U                                          // 有界命令输出容量
#define WIFI_SCAN_READY_MS    10000U                                          // 扫描WPA就绪超时
#define WIFI_SCAN_RESULT_MS   8000U                                           // 单轮扫描结果超时
#define WIFI_SCAN_SURVEY_MS   3000U                                           // survey子进程超时
#define WIFI_SCAN_POLL_MS     100                                             // 取消轮询间隔
#define WIFI_SCAN_FREQUENCIES "5745,5765,5785,5805,5825"                      // 五个用户信道
#define WIFI_SCAN_COMMAND     "SCAN freq=" WIFI_SCAN_FREQUENCIES " passive=1" // 仅执行被动扫描

/****************************** 内部辅助 ******************************/

/**
 * @brief 向专用扫描WPA发命令，输出始终保留结束符。
 */
static int _wifi_scan_request(struct wpa_ctrl *control, const char *command, char *reply, size_t capacity)
{
    size_t  length;
    int     ret;

    if (control == NULL || command == NULL || reply == NULL || capacity < 2U)
    {
        return -EINVAL;
    }
    length = capacity - 1U;
    ret = wpa_ctrl_request(control, command, strlen(command), reply, &length, NULL);
    if (ret != 0)
    {
        return ret == -2 ? -ETIMEDOUT : -EIO;
    }
    if (length >= capacity - 1U)
    {
        return -EOVERFLOW;
    }
    reply[length] = '\0';
    return strncmp(reply, "FAIL", 4U) == 0 ? -EIO : 0;
}

/**
 * @brief 等待本轮扫描完成或失败，不在完成前读取旧survey。
 */
static int _wifi_scan_wait_result(linkg_thread_t *thread, struct wpa_ctrl *events)
{
    struct pollfd descriptor;
    char      message[1024];
    size_t    length;
    uint64_t  deadline;
    int       ret;

    deadline = linkg_time_elapsed_ms() + WIFI_SCAN_RESULT_MS;
    descriptor.fd = wpa_ctrl_get_fd(events);
    descriptor.events = POLLIN;
    while (linkg_thread_is_running(thread))
    {
        if (linkg_time_elapsed_ms() >= deadline)
        {
            return -ETIMEDOUT;
        }
        descriptor.revents = 0;
        ret = poll(&descriptor, 1U, WIFI_SCAN_POLL_MS);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -errno;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -EIO;
        }
        if ((descriptor.revents & POLLIN) == 0)
        {
            continue;
        }
        length = sizeof(message) - 1U;
        if (wpa_ctrl_recv(events, message, &length) != 0)
        {
            return -EIO;
        }
        message[length] = '\0';
        if (strstr(message, "CTRL-EVENT-SCAN-FAILED") != NULL)
        {
            return -EIO;
        }
        if (strstr(message, "CTRL-EVENT-SCAN-RESULTS") != NULL)
        {
            return 0;
        }
    }
    return -ECANCELED;
}

/**
 * @brief 有界读取iw survey输出；取消或超时后只回收本函数创建的子进程。
 */
static int _wifi_scan_read_survey(linkg_thread_t *thread, char *text, size_t capacity, pid_t *owned)
{
    struct pollfd descriptor;
    int       pipes[2];
    int       status;
    int       ret;
    pid_t     process;
    pid_t     waited;
    ssize_t   count;
    size_t    used;
    uint64_t  deadline;
    bool      eof;

    if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0)
    {
        return -errno;
    }
    process = fork();
    if (process < 0)
    {
        ret = -errno;
        close(pipes[0]);
        close(pipes[1]);
        return ret;
    }
    if (process == 0)
    {
        (void)dup2(pipes[1], STDOUT_FILENO);
        (void)dup2(pipes[1], STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execlp("iw", "iw", "dev", LINKG_RESOURCE_INTERFACE_WIFI, "survey", "dump", (char *)NULL);
        _exit(127);
    }
    *owned = process;
    close(pipes[1]);
    descriptor.fd = pipes[0];
    descriptor.events = POLLIN;
    deadline = linkg_time_elapsed_ms() + WIFI_SCAN_SURVEY_MS;
    used = 0U;
    eof = false;
    status = 0;
    ret = 0;
    for (;;)
    {
        if (!linkg_thread_is_running(thread))
        {
            ret = -ECANCELED;
            break;
        }
        if (linkg_time_elapsed_ms() >= deadline)
        {
            ret = -ETIMEDOUT;
            break;
        }
        count = read(pipes[0], text + used, capacity - 1U - used);
        if (count > 0)
        {
            used += (size_t)count;
            if (used >= capacity - 1U)
            {
                ret = -EOVERFLOW;
                break;
            }
        }
        else if (count == 0)
        {
            eof = true;
        }
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            ret = -errno;
            break;
        }
        if (process > 0)
        {
            waited = waitpid(process, &status, WNOHANG);
            if (waited == process)
            {
                process = -1;
                *owned = -1;
            }
            else if (waited < 0 && errno != EINTR)
            {
                ret = -errno;
                break;
            }
        }
        if (eof && process < 0)
        {
            ret = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO;
            break;
        }
        descriptor.revents = 0;
        (void)poll(&descriptor, 1U, WIFI_SCAN_POLL_MS);
    }
    close(pipes[0]);
    text[used] = '\0';
    if (process > 0)
    {
        int stop_ret = linkg_os_process_stop(&process, 1000U);

        if (stop_ret != 0)
        {
            LINKG_LOG_ERROR("SCAN: survey child cleanup failed, error=%d", stop_ret);
            return stop_ret;
        }
    }
    *owned = process;
    return ret;
}

/**
 * @brief 逐条读取BSS，避免SCAN_RESULTS响应长度限制截断附近网络列表。
 */
static int _wifi_scan_read_bss(linkg_thread_t *thread, struct wpa_ctrl *control, linkg_wifi_scan_result_t *result, char *text)
{
    char      command[32];
    char      row[512];
    char      bssid[18];
    char      ssid[129];
    char     *line;
    char     *save;
    int       frequency;
    int       signal;
    int       ret;
    uint32_t  index;

    for (index = 0U; index <= LINKG_WIFI_SCAN_BSS_MAX; index++)
    {
        if (!linkg_thread_is_running(thread))
        {
            return -ECANCELED;
        }
        (void)snprintf(command, sizeof(command), "BSS %u", index);
        ret = _wifi_scan_request(control, command, text, WIFI_SCAN_TEXT_MAX);
        if (ret != 0)
        {
            return ret == -EIO && strncmp(text, "FAIL", 4U) == 0 ? 0 : ret;
        }
        if (text[0] == '\0')
        {
            return 0;
        }
        if (index == LINKG_WIFI_SCAN_BSS_MAX)
        {
            result->bss_truncated = true;
            return 0;
        }
        bssid[0] = '\0';
        ssid[0] = '\0';
        frequency = 0;
        signal = 0;
        save = NULL;
        for (line = strtok_r(text, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save))
        {
            if (strncmp(line, "bssid=", 6U) == 0)
            {
                (void)snprintf(bssid, sizeof(bssid), "%s", line + 6U);
            }
            else if (strncmp(line, "ssid=", 5U) == 0)
            {
                (void)snprintf(ssid, sizeof(ssid), "%s", line + 5U);
            }
            else if (sscanf(line, "freq=%d", &frequency) == 1)
            {
                continue;
            }
            else
            {
                (void)sscanf(line, "level=%d", &signal);
            }
        }
        (void)snprintf(row, sizeof(row), "%s\t%d\t%d\t\t%s\n", bssid, frequency, signal, ssid);
        ret = wifi_scan_parse_bss(result, row);
        if (ret != 0)
        {
            return ret;
        }
    }
    return 0;
}

/****************************** 被动测量 ******************************/

/**
 * @brief 原Owner退出后执行扫描，所有退出路径均停止专用WPA并回收临时资源。
 *
 * 不修改用户配置、驱动INI、国家码或窄带参数。
 * released为false时Network Manager不得启动原Wi-Fi Owner。
 */
int wifi_scan_platform_execute(linkg_thread_t *thread, linkg_wifi_scan_result_t *result, wifi_scan_progress_t progress, void *context, bool *released)
{
    char             directory[] = "/var/run/linkg/wifi-scan-XXXXXX";
    char  config_path[128];
    char  control_dir[128];
    char  control_path[160];
    char *text;
    struct wpa_ctrl *control;
    struct wpa_ctrl *events;
    FILE     *config;
    pid_t     process;
    pid_t     survey_process;
    uint64_t  deadline;
    uint32_t  round;
    uint32_t  index;
    bool      any_sample;
    int       stop_ret;
    int       ret;

    if (thread == NULL || result == NULL || released == NULL)
    {
        return -EINVAL;
    }
    *released = true;
    control = NULL;
    events = NULL;
    process = -1;
    survey_process = -1;
    text = NULL;
    if (mkdtemp(directory) == NULL)
    {
        return -errno;
    }
    (void)snprintf(config_path, sizeof(config_path), "%s/scan.conf", directory);
    (void)snprintf(control_dir, sizeof(control_dir), "%s/ctrl", directory);
    (void)snprintf(control_path, sizeof(control_path), "%s/%s", control_dir, LINKG_RESOURCE_INTERFACE_WIFI);
    ret = -ENOMEM;
    text = malloc(WIFI_SCAN_TEXT_MAX);
    if (text == NULL)
    {
        goto cleanup;
    }
    if (mkdir(control_dir, 0700) != 0)
    {
        ret = -errno;
        goto cleanup;
    }
    config = fopen(config_path, "w");
    if (config == NULL)
    {
        ret = -errno;
        goto cleanup;
    }
    ret = fprintf(config, "ctrl_interface=%s\nupdate_config=0\nap_scan=1\nfreq_list=5745 5765 5785 5805 5825\n", control_dir) < 0 ? -EIO : 0;
    if (fclose(config) != 0)
    {
        ret = -EIO;
    }
    if (ret != 0 || chmod(config_path, 0600) != 0)
    {
        ret = ret != 0 ? ret : -errno;
        goto cleanup;
    }
    ret = linkg_network_interface_set_up(LINKG_RESOURCE_INTERFACE_WIFI, true);
    if (ret != 0)
    {
        goto cleanup;
    }
    ret = linkg_os_spawn(&process, "wpa_supplicant", "-D", "nl80211", "-i", LINKG_RESOURCE_INTERFACE_WIFI, "-c", config_path, NULL);
    if (ret != 0)
    {
        goto cleanup;
    }
    *released = false;
    deadline = linkg_time_elapsed_ms() + WIFI_SCAN_READY_MS;
    while (linkg_thread_is_running(thread) && linkg_time_elapsed_ms() < deadline)
    {
        control = wpa_ctrl_open(control_path);
        if (control != NULL)
        {
            break;
        }
        usleep(WIFI_SCAN_POLL_MS * 1000U);
    }
    if (control == NULL)
    {
        ret = linkg_thread_is_running(thread) ? -ETIMEDOUT : -ECANCELED;
        goto cleanup;
    }
    events = wpa_ctrl_open(control_path);
    if (events == NULL || wpa_ctrl_attach(events) != 0)
    {
        ret = -EIO;
        goto cleanup;
    }

    for (round = 0U; round < LINKG_WIFI_SCAN_ROUND_COUNT; round++)
    {
        if (!linkg_thread_is_running(thread))
        {
            ret = -ECANCELED;
            goto cleanup;
        }
        // 清理旧BSS缓存；完成当前扫描和读取后才允许启动下一轮。
        ret = _wifi_scan_request(control, "BSS_FLUSH 0", text, WIFI_SCAN_TEXT_MAX);
        if (ret == 0)
        {
            ret = _wifi_scan_request(control, WIFI_SCAN_COMMAND, text, WIFI_SCAN_TEXT_MAX);
        }
        if (ret == 0)
        {
            ret = _wifi_scan_wait_result(thread, events);
        }
        if (ret != 0)
        {
            goto cleanup;
        }
        ret = _wifi_scan_read_survey(thread, text, WIFI_SCAN_TEXT_MAX, &survey_process);
        if (ret == -ECANCELED || survey_process > 0)
        {
            goto cleanup;
        }
        if (ret != 0 || wifi_scan_parse_survey(result, text) != 0)
        {
            result->survey_failures++;
        }
        ret = _wifi_scan_read_bss(thread, control, result, text);
        if (ret == -ECANCELED)
        {
            goto cleanup;
        }
        if (ret == 0)
        {
            result->bss_rounds++;
        }
        result->rounds = round + 1U;
        if (progress != NULL)
        {
            progress(context, result);
        }
    }
    any_sample = false;
    for (index = 0U; index < LINKG_WIFI_SCAN_CHANNEL_COUNT; index++)
    {
        any_sample = any_sample || result->channels[index].samples > 0U;
    }
    ret = any_sample ? 0 : -ENODATA;

cleanup:
    if (events != NULL)
    {
        wpa_ctrl_close(events);
    }
    if (control != NULL)
    {
        wpa_ctrl_close(control);
    }
    stop_ret = process > 0 ? linkg_os_process_stop(&process, 5000U) : 0;
    if (survey_process > 0)
    {
        int survey_stop = linkg_os_process_stop(&survey_process, 1000U);
        if (stop_ret == 0)
        {
            stop_ret = survey_stop;
        }
    }
    *released = stop_ret == 0;
    if (stop_ret == 0)
    {
        (void)linkg_network_interface_set_up(LINKG_RESOURCE_INTERFACE_WIFI, false);
        (void)unlink(control_path);
        (void)unlink(config_path);
        (void)rmdir(control_dir);
        (void)rmdir(directory);
    }
    else
    {
        LINKG_LOG_ERROR("SCAN: WPA cleanup failed; original Wi-Fi will not be started, error=%d", stop_ret);
        ret = stop_ret;
    }
    free(text);
    return ret;
}

/**
 * @file linkg_release_state.c
 * @brief 固定版本JSON校验、原子保存及Release安装检查
 * @author Dawn
 * @version 0.2.0
 * @date 2026-10-20
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "linkg_release_state.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

#include "linkg_atomic_file.h"

/****************************** 模块常量 ******************************/

#define LINKG_RELEASE_JSON_SIZE         4096U // 固定结构状态文件读取上限
#define LINKG_RELEASE_FAILURE_JSON_SIZE 1024U // 失败诊断文件读取上限

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查对象键集合，拒绝未知键及重复键。
 */
static bool _linkg_release_keys(const cJSON *object, const char *const keys[], size_t count)
{
    const cJSON *member;
    size_t       index;
    size_t       matches;

    if (!cJSON_IsObject(object) || cJSON_GetArraySize(object) != (int)count)
    {
        return false;
    }

    for (index = 0U; index < count; index++)
    {
        matches = 0U;
        cJSON_ArrayForEach(member, object)
        {
            if (member->string != NULL && strcmp(member->string, keys[index]) == 0)
            {
                matches++;
            }
        }

        if (matches != 1U)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 将固定Release名称解析为数组索引。
 */
static int _linkg_release_release(const cJSON *value, unsigned int *release)
{
    if (!cJSON_IsString(value))
    {
        return -EINVAL;
    }

    if (strcmp(value->valuestring, "v1") == 0 || strcmp(value->valuestring, "v2") == 0)
    {
        *release = (unsigned int)(value->valuestring[1] - '1');
        return 0;
    }

    return -EINVAL;
}

/**
 * @brief 校验内存状态，稳定Release必须已经安装。
 */
static bool _linkg_release_valid(const linkg_release_state_t *version)
{
    return version != NULL && version->target < 2U && version->stable < 2U &&
           version->kernel[0] >= LINKG_KERNEL_NONE && version->kernel[0] <= LINKG_KERNEL_B &&
           version->kernel[1] >= LINKG_KERNEL_NONE && version->kernel[1] <= LINKG_KERNEL_B &&
           version->kernel[version->stable] != LINKG_KERNEL_NONE &&
           version->upgrade >= LINKG_UPGRADE_PHASE_IDLE && version->upgrade < LINKG_UPGRADE_PHASE_COUNT;
}

/****************************** 状态文件 ******************************/

/**
 * @brief 读取严格四字段JSON，保留未安装候选供选择阶段回滚。
 */
int linkg_release_load(const char *path, linkg_release_state_t *version)
{
    const char *const root_keys[]    = {"schema_version", "release_partition", "stable_release_partition", "releases", "upgrade_state"};
    const char *const release_keys[] = {"v1", "v2"};
    const char *const kernel_keys[]  = {"kernel_slot"};
    char              text[LINKG_RELEASE_JSON_SIZE];
    cJSON            *root;
    const cJSON      *releases;
    const cJSON      *entry;
    const cJSON      *value;
    unsigned int      index;
    int               ret;

    if (version == NULL)
    {
        return -EINVAL;
    }

    memset(version, 0, sizeof(*version));
    ret = linkg_atomic_read(path, text, sizeof(text));
    if (ret != 0)
    {
        return ret;
    }

    // cJSON字符串不保存原始长度，显式拒绝可能隐藏尾部内容的零字符转义。
    if (strstr(text, "\\u0000") != NULL)
    {
        return -EINVAL;
    }

    root = cJSON_ParseWithOpts(text, NULL, 1);
    if (!_linkg_release_keys(root, root_keys, 4U) && !_linkg_release_keys(root, root_keys, 5U))
    {
        cJSON_Delete(root);
        return -EINVAL;
    }

    ret   = -EINVAL;
    value = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(value) || value->valuedouble != 1.0 ||
        _linkg_release_release(cJSON_GetObjectItemCaseSensitive(root, "release_partition"), &version->target) != 0 ||
        _linkg_release_release(cJSON_GetObjectItemCaseSensitive(root, "stable_release_partition"), &version->stable) != 0)
    {
        goto finish;
    }

    releases = cJSON_GetObjectItemCaseSensitive(root, "releases");
    if (!_linkg_release_keys(releases, release_keys, 2U))
    {
        goto finish;
    }

    for (index = 0U; index < 2U; index++)
    {
        entry = cJSON_GetObjectItemCaseSensitive(releases, release_keys[index]);
        if (cJSON_IsObject(entry) && cJSON_GetArraySize(entry) == 0)
        {
            continue;
        }

        if (!_linkg_release_keys(entry, kernel_keys, 1U))
        {
            goto finish;
        }

        value = cJSON_GetObjectItemCaseSensitive(entry, "kernel_slot");
        if (!cJSON_IsString(value))
        {
            goto finish;
        }

        if (strcmp(value->valuestring, "boot_a") == 0)
        {
            version->kernel[index] = LINKG_KERNEL_A;
        }
        else if (strcmp(value->valuestring, "boot_b") == 0)
        {
            version->kernel[index] = LINKG_KERNEL_B;
        }
        else
        {
            goto finish;
        }
    }

    value = cJSON_GetObjectItemCaseSensitive(root, "upgrade_state");
    if (value != NULL)
    {
        if (!cJSON_IsString(value))
        {
            goto finish;
        }

        for (index = 0U; index < LINKG_UPGRADE_PHASE_COUNT; index++)
        {
            if (strcmp(value->valuestring, linkg_release_phase_name((linkg_upgrade_phase_t)index)) == 0)
            {
                break;
            }
        }

        if (index == LINKG_UPGRADE_PHASE_COUNT)
        {
            goto finish;
        }

        version->upgrade = (linkg_upgrade_phase_t)index;
    }

    ret = _linkg_release_valid(version) ? 0 : -EINVAL;

finish:
    cJSON_Delete(root);
    return ret;
}

/**
 * @brief 将完整固定结构写入同目录临时文件并同步原子替换。
 */
int linkg_release_save(const char *path, const linkg_release_state_t *version)
{
    char text[512];
    char releases[2][64];
    int  length;
    int  index;

    if (!_linkg_release_valid(version))
    {
        return -EINVAL;
    }

    for (index = 0; index < 2; index++)
    {
        if (version->kernel[index] == LINKG_KERNEL_NONE)
        {
            strcpy(releases[index], "{}");
        }
        else
        {
            snprintf(releases[index], sizeof(releases[index]), "{\"kernel_slot\":\"%s\"}", linkg_release_kernel_name(version->kernel[index]));
        }
    }

    length = snprintf(text, sizeof(text),
                      "{\n  \"schema_version\":1,\n  \"release_partition\":\"%s\",\n  \"stable_release_partition\":\"%s\",\n"
                      "  \"upgrade_state\":\"%s\",\n  \"releases\":{\"v1\":%s,\"v2\":%s}\n}\n",
                      linkg_release_name(version->target), linkg_release_name(version->stable), linkg_release_phase_name(version->upgrade), releases[0], releases[1]);
    if (length < 0 || (size_t)length >= sizeof(text))
    {
        return -EOVERFLOW;
    }

    return linkg_atomic_write(path, text, (size_t)length);
}

/**
 * @brief 比较全部业务字段，避免结构体填充影响一致性判断。
 */
bool linkg_release_equal(const linkg_release_state_t *left, const linkg_release_state_t *right)
{
    return left->target == right->target && left->stable == right->stable &&
           left->kernel[0] == right->kernel[0] && left->kernel[1] == right->kernel[1] && left->upgrade == right->upgrade;
}

/****************************** 版本选择 ******************************/

/**
 * @brief 获取固定Release名称。
 */
const char *linkg_release_name(unsigned int release)
{
    return release < 2U ? (release == 0U ? "v1" : "v2") : NULL;
}

/**
 * @brief 获取固定Kernel分区名称。
 */
const char *linkg_release_kernel_name(linkg_kernel_slot_t kernel)
{
    return kernel == LINKG_KERNEL_A ? "boot_a" : kernel == LINKG_KERNEL_B ? "boot_b" : NULL;
}

/**
 * @brief 返回持久化升级状态白名单名称。
 */
const char *linkg_release_phase_name(linkg_upgrade_phase_t phase)
{
    const char *const names[] = {"idle", "verifying", "installing", "flashing", "rebooting", "testing",
                                 "completed", "rolling_back", "rolled_back", "failed"};

    return phase >= LINKG_UPGRADE_PHASE_IDLE && phase < LINKG_UPGRADE_PHASE_COUNT ? names[phase] : NULL;
}

/**
 * @brief 运行期锁不随状态rename更换inode，关闭描述符释放而不删除文件。
 */
int linkg_release_lock(const char *path, const char *suffix, bool nonblocking)
{
    struct stat information;
    char        lock_path[PATH_MAX];
    int         descriptor;
    int         ret;

    if (path == NULL || suffix == NULL || path[0] != '/')
    {
        return -EINVAL;
    }

    ret = snprintf(lock_path, sizeof(lock_path), "%s%s", path, suffix);
    if (ret < 0 || (size_t)ret >= sizeof(lock_path))
    {
        return -ENAMETOOLONG;
    }

    descriptor = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
    {
        return -errno;
    }

    ret = fstat(descriptor, &information) == 0 ? 0 : -errno;
    if (ret == 0 && (!S_ISREG(information.st_mode) || information.st_uid != geteuid() || information.st_nlink != 1))
    {
        ret = -EPERM;
    }

    if (ret == 0 && flock(descriptor, LOCK_EX | (nonblocking ? LOCK_NB : 0)) != 0)
    {
        ret = errno == EWOULDBLOCK || errno == EAGAIN ? -EBUSY : -errno;
    }

    if (ret != 0)
    {
        close(descriptor);
        return ret;
    }

    return descriptor;
}

/**
 * @brief 跨进程锁下比较完整旧状态再原子提交，接受已生效但目录同步失败的重试。
 */
int linkg_release_replace(const char *path, const linkg_release_state_t *expected, const linkg_release_state_t *desired)
{
    linkg_release_state_t current;
    int                   descriptor;
    int                   ret;

    if (!_linkg_release_valid(expected) || !_linkg_release_valid(desired))
    {
        return -EINVAL;
    }

    descriptor = linkg_release_lock(path, ".lock", false);
    if (descriptor < 0)
    {
        return descriptor;
    }

    ret = linkg_release_load(path, &current);
    if (ret == 0 && !linkg_release_equal(&current, expected) && !linkg_release_equal(&current, desired))
    {
        ret = -ESTALE;
    }

    if (ret == 0)
    {
        ret = linkg_release_save(path, desired);
    }

    close(descriptor);
    return ret;
}


/****************************** 升级诊断 ******************************/

/**
 * @brief 从版本状态路径构造同目录独立诊断文件，不修改版本状态Schema。
 */
static int _linkg_release_failure_path(const char *state_path, char *path, size_t capacity)
{
    const char *separator;
    size_t      prefix_length;
    const char *filename = "upgrade_failure.json";
    size_t      name_length;

    if (state_path == NULL || path == NULL || capacity == 0U || state_path[0] != '/')
    {
        return -EINVAL;
    }

    separator = strrchr(state_path, '/');
    if (separator == NULL || separator[1] == '\0')
    {
        return -EINVAL;
    }

    prefix_length = (size_t)(separator - state_path) + 1U;
    name_length   = strlen(filename);
    if (prefix_length + name_length >= capacity)
    {
        return -ENAMETOOLONG;
    }

    memcpy(path, state_path, prefix_length);
    memcpy(path + prefix_length, filename, name_length + 1U);
    return 0;
}

/**
 * @brief 校验诊断字段有界且Release索引合法。
 */
static bool _linkg_release_failure_valid(const linkg_release_failure_t *failure)
{
    return failure != NULL && failure->candidate < 2U && failure->rollback < 2U &&
           failure->candidate != failure->rollback &&
           strnlen(failure->failed_stage, sizeof(failure->failed_stage)) > 0U &&
           strnlen(failure->failed_stage, sizeof(failure->failed_stage)) < sizeof(failure->failed_stage) &&
           strnlen(failure->failed_check, sizeof(failure->failed_check)) > 0U &&
           strnlen(failure->failed_check, sizeof(failure->failed_check)) < sizeof(failure->failed_check) &&
           strnlen(failure->failure_reason, sizeof(failure->failure_reason)) > 0U &&
           strnlen(failure->failure_reason, sizeof(failure->failure_reason)) < sizeof(failure->failure_reason);
}

/**
 * @brief 原子写入最近一次升级失败记录，诊断文件不参与JSON/ENV事务。
 */
int linkg_release_failure_save(const char *state_path, const linkg_release_failure_t *failure)
{
    char        path[PATH_MAX];
    char        timestamp[LINKG_RELEASE_FAILURE_TIME_MAX];
    struct tm   utc;
    time_t      now;
    cJSON      *root;
    char       *text;
    int         ret;

    if (!_linkg_release_failure_valid(failure))
    {
        return -EINVAL;
    }

    ret = _linkg_release_failure_path(state_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    now = time(NULL);
    if (now == (time_t)-1 || gmtime_r(&now, &utc) == NULL ||
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0U)
    {
        (void)snprintf(timestamp, sizeof(timestamp), "%s", "unknown");
    }

    root = cJSON_CreateObject();
    if (root == NULL)
    {
        return -ENOMEM;
    }

    if (cJSON_AddNumberToObject(root, "schema_version", 1) == NULL ||
        cJSON_AddStringToObject(root, "candidate_release", linkg_release_name(failure->candidate)) == NULL ||
        cJSON_AddStringToObject(root, "rollback_release", linkg_release_name(failure->rollback)) == NULL ||
        cJSON_AddStringToObject(root, "failed_stage", failure->failed_stage) == NULL ||
        cJSON_AddStringToObject(root, "failed_check", failure->failed_check) == NULL ||
        cJSON_AddNumberToObject(root, "error_code", failure->error_code) == NULL ||
        cJSON_AddStringToObject(root, "failure_time", timestamp) == NULL ||
        cJSON_AddStringToObject(root, "failure_reason", failure->failure_reason) == NULL)
    {
        cJSON_Delete(root);
        return -ENOMEM;
    }

    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL)
    {
        return -ENOMEM;
    }

    if (strlen(text) >= LINKG_RELEASE_FAILURE_JSON_SIZE)
    {
        cJSON_free(text);
        return -EOVERFLOW;
    }

    ret = linkg_atomic_write(path, text, strlen(text));
    cJSON_free(text);
    return ret;
}

/**
 * @brief 读取固定长度的诊断字符串，拒绝超长或非法类型。
 */
static int _linkg_release_failure_read_string(const cJSON *root, const char *name, char *output, size_t capacity)
{
    const cJSON *value;

    value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= capacity)
    {
        return -EINVAL;
    }

    strcpy(output, value->valuestring);
    return 0;
}

/**
 * @brief 读取最近一次失败记录，文件不存在返回-ENOENT，损坏时明确返回错误。
 */
int linkg_release_failure_load(const char *state_path, linkg_release_failure_t *failure)
{
    const char *const keys[] = {"schema_version", "candidate_release", "rollback_release", "failed_stage",
                                "failed_check", "error_code", "failure_time", "failure_reason"};
    char             path[PATH_MAX];
    char             text[LINKG_RELEASE_FAILURE_JSON_SIZE];
    const cJSON     *value;
    cJSON           *root;
    int              ret;

    if (failure == NULL)
    {
        return -EINVAL;
    }

    memset(failure, 0, sizeof(*failure));
    ret = _linkg_release_failure_path(state_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_atomic_read(path, text, sizeof(text));
    if (ret != 0)
    {
        return ret;
    }

    root = cJSON_ParseWithOpts(text, NULL, 1);
    if (!_linkg_release_keys(root, keys, sizeof(keys) / sizeof(keys[0])))
    {
        cJSON_Delete(root);
        return -EINVAL;
    }

    value = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(value) || value->valuedouble != 1.0 ||
        _linkg_release_release(cJSON_GetObjectItemCaseSensitive(root, "candidate_release"), &failure->candidate) != 0 ||
        _linkg_release_release(cJSON_GetObjectItemCaseSensitive(root, "rollback_release"), &failure->rollback) != 0)
    {
        ret = -EINVAL;
        goto finish;
    }

    value = cJSON_GetObjectItemCaseSensitive(root, "error_code");
    if (!cJSON_IsNumber(value) || value->valuedouble != (double)value->valueint)
    {
        ret = -EINVAL;
        goto finish;
    }
    failure->error_code = value->valueint;

    ret = _linkg_release_failure_read_string(root, "failed_stage", failure->failed_stage, sizeof(failure->failed_stage));
    if (ret == 0)
    {
        ret = _linkg_release_failure_read_string(root, "failed_check", failure->failed_check, sizeof(failure->failed_check));
    }
    if (ret == 0)
    {
        ret = _linkg_release_failure_read_string(root, "failure_time", failure->failure_time, sizeof(failure->failure_time));
    }
    if (ret == 0)
    {
        ret = _linkg_release_failure_read_string(root, "failure_reason", failure->failure_reason, sizeof(failure->failure_reason));
    }
    if (ret != 0)
    {
        goto finish;
    }



    ret = _linkg_release_failure_valid(failure) && failure->failure_time[0] != '\0' ? 0 : -EINVAL;

finish:
    cJSON_Delete(root);
    if (ret != 0)
    {
        memset(failure, 0, sizeof(*failure));
    }
    return ret;
}

/**
 * @brief 仅在成功确认候选版本后清除历史失败记录并同步父目录。
 */
int linkg_release_failure_clear(const char *state_path)
{
    char path[PATH_MAX];
    int  ret;

    ret = _linkg_release_failure_path(state_path, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    if (unlink(path) != 0)
    {
        return errno == ENOENT ? 0 : -errno;
    }

    return linkg_atomic_sync_parent(path);
}

/**
 * @brief 仅隔离测试构建接收临时路径，产品程序始终使用固定路径。
 */
const char *linkg_release_path(const char *test_name, const char *production)
{
#if defined(LINKG_OTA_TEST_IO)
    const char *path;

    (void)production;
    path = getenv(test_name);
    return path != NULL && path[0] == '/' ? path : NULL;
#else
    (void)test_name;
    return production;
#endif
}

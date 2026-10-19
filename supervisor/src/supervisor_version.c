/**
 * @file supervisor_version.c
 * @brief 固定版本JSON校验、原子保存及Release安装检查
 * @author Dawn
 * @version 0.2.0
 * @date 2026-10-19
 */

#include "supervisor_version.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

#include "supervisor_storage.h"

/****************************** 模块常量 ******************************/

#define SUPERVISOR_VERSION_JSON_SIZE 4096U // 固定结构状态文件读取上限

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查对象键集合，拒绝未知键及重复键。
 */
static bool _supervisor_version_keys(const cJSON *object, const char *const keys[], size_t count)
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
static int _supervisor_version_release(const cJSON *value, unsigned int *release)
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
static bool _supervisor_version_valid(const supervisor_version_t *version)
{
    return version != NULL && version->target < 2U && version->stable < 2U &&
           version->kernel[0] >= SUPERVISOR_KERNEL_NONE && version->kernel[0] <= SUPERVISOR_KERNEL_B &&
           version->kernel[1] >= SUPERVISOR_KERNEL_NONE && version->kernel[1] <= SUPERVISOR_KERNEL_B &&
           version->kernel[version->stable] != SUPERVISOR_KERNEL_NONE;
}

/****************************** 状态文件 ******************************/

/**
 * @brief 读取严格四字段JSON，保留未安装候选供选择阶段回滚。
 */
int supervisor_version_load(const char *path, supervisor_version_t *version)
{
    const char *const root_keys[]    = {"schema_version", "release_partition", "stable_release_partition", "releases"};
    const char *const release_keys[] = {"v1", "v2"};
    const char *const kernel_keys[]  = {"kernel_slot"};
    char              text[SUPERVISOR_VERSION_JSON_SIZE];
    cJSON            *root;
    const cJSON      *releases;
    const cJSON      *entry;
    const cJSON      *value;
    unsigned int      index;
    int               ret;

    memset(version, 0, sizeof(*version));
    ret = supervisor_storage_read(path, text, sizeof(text));
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
    if (!_supervisor_version_keys(root, root_keys, 4U))
    {
        cJSON_Delete(root);
        return -EINVAL;
    }

    ret   = -EINVAL;
    value = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(value) || value->valuedouble != 1.0 ||
        _supervisor_version_release(cJSON_GetObjectItemCaseSensitive(root, "release_partition"), &version->target) != 0 ||
        _supervisor_version_release(cJSON_GetObjectItemCaseSensitive(root, "stable_release_partition"), &version->stable) != 0)
    {
        goto finish;
    }

    releases = cJSON_GetObjectItemCaseSensitive(root, "releases");
    if (!_supervisor_version_keys(releases, release_keys, 2U))
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

        if (!_supervisor_version_keys(entry, kernel_keys, 1U))
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
            version->kernel[index] = SUPERVISOR_KERNEL_A;
        }
        else if (strcmp(value->valuestring, "boot_b") == 0)
        {
            version->kernel[index] = SUPERVISOR_KERNEL_B;
        }
        else
        {
            goto finish;
        }
    }

    ret = _supervisor_version_valid(version) ? 0 : -EINVAL;

finish:
    cJSON_Delete(root);
    return ret;
}

/**
 * @brief 将完整固定结构写入同目录临时文件并同步原子替换。
 */
int supervisor_version_save(const char *path, const supervisor_version_t *version)
{
    char text[512];
    char releases[2][64];
    int  length;
    int  index;

    if (!_supervisor_version_valid(version))
    {
        return -EINVAL;
    }

    for (index = 0; index < 2; index++)
    {
        if (version->kernel[index] == SUPERVISOR_KERNEL_NONE)
        {
            strcpy(releases[index], "{}");
        }
        else
        {
            snprintf(releases[index], sizeof(releases[index]), "{\"kernel_slot\":\"%s\"}", supervisor_version_kernel_name(version->kernel[index]));
        }
    }

    length = snprintf(text, sizeof(text),
                      "{\n  \"schema_version\":1,\n  \"release_partition\":\"%s\",\n  \"stable_release_partition\":\"%s\",\n"
                      "  \"releases\":{\"v1\":%s,\"v2\":%s}\n}\n",
                      supervisor_version_name(version->target), supervisor_version_name(version->stable), releases[0], releases[1]);
    if (length < 0 || (size_t)length >= sizeof(text))
    {
        return -EOVERFLOW;
    }

    return supervisor_storage_write(path, text, (size_t)length);
}

/**
 * @brief 比较全部业务字段，避免结构体填充影响一致性判断。
 */
bool supervisor_version_equal(const supervisor_version_t *left, const supervisor_version_t *right)
{
    return left->target == right->target && left->stable == right->stable &&
           left->kernel[0] == right->kernel[0] && left->kernel[1] == right->kernel[1];
}

/****************************** 版本选择 ******************************/

/**
 * @brief 获取固定Release名称。
 */
const char *supervisor_version_name(unsigned int release)
{
    return release < 2U ? (release == 0U ? "v1" : "v2") : NULL;
}

/**
 * @brief 获取固定Kernel分区名称。
 */
const char *supervisor_version_kernel_name(supervisor_kernel_t kernel)
{
    return kernel == SUPERVISOR_KERNEL_A ? "boot_a" : kernel == SUPERVISOR_KERNEL_B ? "boot_b" : NULL;
}

/**
 * @brief 验证App和Web安装目录以及LinkG可执行文件。
 */
int supervisor_version_check(const char *app_root, const char *web_root, unsigned int release)
{
    const char *roots[2] = {app_root, web_root};
    struct stat information;
    char        path[PATH_MAX];
    int         index;
    int         length;

    if (release >= 2U)
    {
        return -EINVAL;
    }

    for (index = 0; index < 2; index++)
    {
        length = snprintf(path, sizeof(path), "%s/releases/%s", roots[index], supervisor_version_name(release));
        if (length < 0 || (size_t)length >= sizeof(path))
        {
            return -ENAMETOOLONG;
        }

        if (stat(path, &information) != 0)
        {
            return -errno;
        }

        if (!S_ISDIR(information.st_mode))
        {
            return -ENOTDIR;
        }
    }

    length = snprintf(path, sizeof(path), "%s/releases/%s/bin/linkg", app_root, supervisor_version_name(release));
    if (length < 0 || (size_t)length >= sizeof(path))
    {
        return -ENAMETOOLONG;
    }

    if (stat(path, &information) != 0)
    {
        return -errno;
    }

    if (!S_ISREG(information.st_mode))
    {
        return -EINVAL;
    }

    return access(path, X_OK) == 0 ? 0 : -errno;
}

/**
 * @brief 构造唯一current程序路径，供进程模块复用原有exec流程。
 */
int supervisor_version_program(const char *app_root, char *program, size_t capacity)
{
    int length;

    length = snprintf(program, capacity, "%s/current/bin/linkg", app_root);
    return length >= 0 && (size_t)length < capacity ? 0 : -ENAMETOOLONG;
}

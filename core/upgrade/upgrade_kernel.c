/**
 * @file upgrade_kernel.c
 * @brief LinkG备用Kernel MTD分区的SHA-256比较、刷写及读回校验
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#define _GNU_SOURCE

#include "upgrade_kernel.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mtd/mtd-user.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/****************************** 模块常量 ******************************/

#define UPGRADE_KERNEL_IMAGE_RELATIVE_PATH  "/kernel/boot.img" // 升级包中Kernel镜像的相对位置
#define UPGRADE_KERNEL_MTD_TABLE_PATH       "/proc/mtd"        // 当前系统实际MTD分区表
#define UPGRADE_KERNEL_CMDLINE_PATH         "/proc/cmdline"    // 当前运行Kernel的实际槽位
#define UPGRADE_KERNEL_IO_BYTES             16384U             // 分块哈希与Flash写入缓冲区大小
#define UPGRADE_KERNEL_SHA256_BYTES         32U                // SHA-256摘要长度

/****************************** 内部类型 ******************************/

typedef struct
{
    uint32_t state[8];                            // SHA-256链式状态
    uint64_t total_bytes;                         // 已输入的完整字节数
    uint8_t  block[64];                           // 当前哈希数据块
    size_t   used;                                // 数据块有效字节数
} upgrade_kernel_sha256_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 计算SHA-256循环右移。
 */
static uint32_t _upgrade_kernel_rotr(uint32_t value, unsigned int bits)
{
    return (value >> bits) | (value << (32U - bits));
}

/**
 * @brief 压缩一个SHA-256数据块。
 */
static void _upgrade_kernel_sha256_block(upgrade_kernel_sha256_t *context)
{
    static const uint32_t constants[64] =
    {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    uint32_t words[64];
    uint32_t values[8];
    uint32_t sigma0;
    uint32_t sigma1;
    uint32_t first;
    uint32_t second;
    unsigned int index;
    unsigned int step;

    for (index = 0U; index < 16U; index++)
    {
        step = index * 4U;
        words[index] = (uint32_t)context->block[step] << 24U | (uint32_t)context->block[step + 1U] << 16U |
                       (uint32_t)context->block[step + 2U] << 8U | (uint32_t)context->block[step + 3U];
    }

    for (index = 16U; index < 64U; index++)
    {
        sigma0 = _upgrade_kernel_rotr(words[index - 15U], 7U) ^ _upgrade_kernel_rotr(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
        sigma1 = _upgrade_kernel_rotr(words[index - 2U], 17U) ^ _upgrade_kernel_rotr(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + sigma0 + words[index - 7U] + sigma1;
    }

    memcpy(values, context->state, sizeof(values));
    for (index = 0U; index < 64U; index++)
    {
        sigma0 = _upgrade_kernel_rotr(values[0], 2U) ^ _upgrade_kernel_rotr(values[0], 13U) ^ _upgrade_kernel_rotr(values[0], 22U);
        sigma1 = _upgrade_kernel_rotr(values[4], 6U) ^ _upgrade_kernel_rotr(values[4], 11U) ^ _upgrade_kernel_rotr(values[4], 25U);
        first  = values[7] + sigma1 + ((values[4] & values[5]) ^ (~values[4] & values[6])) + constants[index] + words[index];
        second = sigma0 + ((values[0] & values[1]) ^ (values[0] & values[2]) ^ (values[1] & values[2]));
        values[7] = values[6];
        values[6] = values[5];
        values[5] = values[4];
        values[4] = values[3] + first;
        values[3] = values[2];
        values[2] = values[1];
        values[1] = values[0];
        values[0] = first + second;
    }

    for (index = 0U; index < 8U; index++)
    {
        context->state[index] += values[index];
    }
}

/**
 * @brief 初始化SHA-256流式摘要。
 */
static void _upgrade_kernel_sha256_init(upgrade_kernel_sha256_t *context)
{
    static const uint32_t initial[8] =
    {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memset(context, 0, sizeof(*context));
    memcpy(context->state, initial, sizeof(initial));
}

/**
 * @brief 分块更新SHA-256，无需缓存完整镜像。
 */
static void _upgrade_kernel_sha256_update(upgrade_kernel_sha256_t *context, const uint8_t *data, size_t length)
{
    size_t chunk;

    context->total_bytes += length;
    while (length > 0U)
    {
        chunk = sizeof(context->block) - context->used;
        if (chunk > length)
        {
            chunk = length;
        }

        memcpy(context->block + context->used, data, chunk);
        context->used += chunk;
        data          += chunk;
        length        -= chunk;
        if (context->used == sizeof(context->block))
        {
            _upgrade_kernel_sha256_block(context);
            context->used = 0U;
        }
    }
}

/**
 * @brief 补齐SHA-256结束标记并输出摘要。
 */
static void _upgrade_kernel_sha256_finish(upgrade_kernel_sha256_t *context, uint8_t digest[UPGRADE_KERNEL_SHA256_BYTES])
{
    uint64_t bit_count;
    unsigned int index;

    bit_count = context->total_bytes * 8U;
    context->block[context->used++] = 0x80U;
    if (context->used > 56U)
    {
        memset(context->block + context->used, 0, 64U - context->used);
        _upgrade_kernel_sha256_block(context);
        context->used = 0U;
    }

    memset(context->block + context->used, 0, 56U - context->used);
    for (index = 0U; index < 8U; index++)
    {
        context->block[56U + index] = (uint8_t)(bit_count >> (56U - 8U * index));
    }

    _upgrade_kernel_sha256_block(context);
    for (index = 0U; index < 8U; index++)
    {
        digest[index * 4U]      = (uint8_t)(context->state[index] >> 24U);
        digest[index * 4U + 1U] = (uint8_t)(context->state[index] >> 16U);
        digest[index * 4U + 2U] = (uint8_t)(context->state[index] >> 8U);
        digest[index * 4U + 3U] = (uint8_t)context->state[index];
    }
}

/**
 * @brief 计算镜像或MTD的SHA-256；镜像不足分区的尾部按已擦除0xFF填充。
 */
static int _upgrade_kernel_hash(int descriptor, uint64_t length, uint64_t padding, uint8_t digest[UPGRADE_KERNEL_SHA256_BYTES])
{
    upgrade_kernel_sha256_t context;
    uint8_t                 buffer[UPGRADE_KERNEL_IO_BYTES];
    uint64_t                offset;
    size_t                  chunk;
    ssize_t                 count;

    _upgrade_kernel_sha256_init(&context);
    offset = 0U;
    while (offset < length)
    {
        chunk = length - offset < sizeof(buffer) ? (size_t)(length - offset) : sizeof(buffer);
        count = pread(descriptor, buffer, chunk, (off_t)offset);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return count == 0 ? -EIO : -errno;
        }

        _upgrade_kernel_sha256_update(&context, buffer, (size_t)count);
        offset += (uint64_t)count;
    }

    memset(buffer, 0xFF, sizeof(buffer));
    while (padding > 0U)
    {
        chunk = padding < sizeof(buffer) ? (size_t)padding : sizeof(buffer);
        _upgrade_kernel_sha256_update(&context, buffer, chunk);
        padding -= chunk;
    }

    _upgrade_kernel_sha256_finish(&context, digest);
    return 0;
}

/**
 * @brief 查找命名MTD分区，不依赖固定mtd编号。
 */
static int _upgrade_kernel_find_mtd(const char *name, char *path, size_t capacity)
{
    char         line[128];
    char         found_name[32];
    FILE        *stream;
    unsigned int index;
    unsigned int matched_index;
    unsigned int found;
    unsigned int partition_size;
    unsigned int erase_size;
    int          length;
    int          ret;

    stream = fopen(UPGRADE_KERNEL_MTD_TABLE_PATH, "r");
    if (stream == NULL)
    {
        return -errno;
    }

    found         = 0U;
    matched_index = 0U;
    while (fgets(line, sizeof(line), stream) != NULL)
    {
        if (sscanf(line, "mtd%u: %x %x \"%31[^\"]\"", &index, &partition_size, &erase_size, found_name) != 4 ||
            strcmp(name, found_name) != 0)
        {
            continue;
        }

        matched_index = index;
        found++;
    }

    ret = ferror(stream) ? -EIO : 0;
    if (fclose(stream) != 0 && ret == 0)
    {
        ret = -errno;
    }
    if (ret != 0)
    {
        return ret;
    }
    if (found != 1U)
    {
        return found == 0U ? -ENOENT : -EINVAL;
    }

    length = snprintf(path, capacity, "/dev/mtd%u", matched_index);
    return length < 0 || (size_t)length >= capacity ? -ENAMETOOLONG : 0;
}

/**
 * @brief 验证当前运行Kernel确实属于指定稳定槽位，避免擦写活动分区。
 */
static int _upgrade_kernel_check_active(const char *stable_kernel_slot)
{
    char   command_line[4096];
    char  *save;
    char  *token;
    FILE  *stream;
    size_t used;
    bool   found;
    int    ret;

    stream = fopen(UPGRADE_KERNEL_CMDLINE_PATH, "r");
    if (stream == NULL)
    {
        return -errno;
    }

    used = fread(command_line, 1U, sizeof(command_line) - 1U, stream);
    ret  = ferror(stream) ? -EIO : 0;
    if (ret == 0 && used == sizeof(command_line) - 1U && fgetc(stream) != EOF)
    {
        ret = -EOVERFLOW;
    }
    if (fclose(stream) != 0 && ret == 0)
    {
        ret = -errno;
    }
    if (ret != 0)
    {
        return ret;
    }

    command_line[used] = '\0';
    found = false;
    for (token = strtok_r(command_line, " \t\r\n", &save); token != NULL; token = strtok_r(NULL, " \t\r\n", &save))
    {
        if (strncmp(token, "kernel_slot=", 12U) != 0)
        {
            continue;
        }
        if (found || strcmp(token + 12U, stable_kernel_slot) != 0)
        {
            return -ESTALE;
        }
        found = true;
    }

    return found ? 0 : -ENODATA;
}

/**
 * @brief 打开并确认MTD设备是可读的NOR Kernel分区。
 */
static int _upgrade_kernel_open_mtd(const char *name, bool writable, mtd_info_t *information)
{
    char        path[PATH_MAX];
    struct stat status;
    int         descriptor;
    int         ret;

    ret = _upgrade_kernel_find_mtd(name, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    descriptor = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
    {
        return -errno;
    }

    if (fstat(descriptor, &status) != 0)
    {
        ret = -errno;
    }
    else if (!S_ISCHR(status.st_mode) || ioctl(descriptor, MEMGETINFO, information) != 0)
    {
        ret = -ENODEV;
    }
    else if (information->type != MTD_NORFLASH || information->size == 0U || information->erasesize == 0U ||
             information->size % information->erasesize != 0U ||
             (writable && (information->flags & MTD_WRITEABLE) == 0U))
    {
        ret = -EINVAL;
    }
    else
    {
        ret = 0;
    }

    if (ret != 0)
    {
        close(descriptor);
        return ret;
    }
    return descriptor;
}

/**
 * @brief 擦除备用NOR分区并从/tmp中的镜像逐块写入，不创建中间副本。
 */
static int _upgrade_kernel_flash(int source_fd, int target_fd, uint64_t length, uint32_t partition_size)
{
    struct erase_info_user erase;
    uint8_t                buffer[UPGRADE_KERNEL_IO_BYTES];
    uint64_t               offset;
    size_t                 chunk;
    size_t                 done;
    ssize_t                count;

    erase.start  = 0U;
    erase.length = partition_size;
    if (ioctl(target_fd, MEMERASE, &erase) != 0)
    {
        return -errno;
    }

    offset = 0U;
    while (offset < length)
    {
        chunk = length - offset < sizeof(buffer) ? (size_t)(length - offset) : sizeof(buffer);
        count = pread(source_fd, buffer, chunk, (off_t)offset);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return count == 0 ? -EIO : -errno;
        }

        done = 0U;
        while (done < (size_t)count)
        {
            ssize_t written = pwrite(target_fd, buffer + done, (size_t)count - done, (off_t)offset + (off_t)done);
            if (written < 0 && errno == EINTR)
            {
                continue;
            }
            if (written <= 0)
            {
                return written == 0 ? -EIO : -errno;
            }
            done += (size_t)written;
        }
        offset += (uint64_t)count;
    }

    return fsync(target_fd) == 0 ? 0 : -errno;
}

/****************************** Kernel处理 ******************************/

/**
 * @brief 复用相同Kernel；不同时刷写备用MTD并用SHA-256读回验证。
 *
 * 升级包必须已通过Manifest校验；不写入/app、不修改JSON或ENV。
 */
int upgrade_kernel_prepare(const char *upgrade_dir, const char *stable_kernel_slot, char *target_kernel_slot, size_t slot_size)
{
    uint8_t     image_hash[UPGRADE_KERNEL_SHA256_BYTES];
    uint8_t     flash_hash[UPGRADE_KERNEL_SHA256_BYTES];
    mtd_info_t  stable_info;
    mtd_info_t  target_info;
    struct stat image_stat;
    char        image_path[PATH_MAX];
    const char *opposite;
    int         image_fd;
    int         stable_fd;
    int         target_fd;
    int         ret;
    int         length;

    if (upgrade_dir == NULL || stable_kernel_slot == NULL || target_kernel_slot == NULL || slot_size < 7U)
    {
        return -EINVAL;
    }

    target_kernel_slot[0] = '\0';
    if (strcmp(stable_kernel_slot, "boot_a") == 0)
    {
        opposite = "boot_b";
    }
    else if (strcmp(stable_kernel_slot, "boot_b") == 0)
    {
        opposite = "boot_a";
    }
    else
    {
        return -EINVAL;
    }

    if (lstat(upgrade_dir, &image_stat) != 0)
    {
        return -errno;
    }
    if (!S_ISDIR(image_stat.st_mode))
    {
        return -ENOTDIR;
    }

    length = snprintf(image_path, sizeof(image_path), "%s%s", upgrade_dir, UPGRADE_KERNEL_IMAGE_RELATIVE_PATH);
    if (length < 0 || (size_t)length >= sizeof(image_path))
    {
        return -ENAMETOOLONG;
    }

    if (lstat(image_path, &image_stat) != 0)
    {
        if (errno != ENOENT)
        {
            return -errno;
        }
        strcpy(target_kernel_slot, stable_kernel_slot);
        return 0;
    }
    if (!S_ISREG(image_stat.st_mode) || image_stat.st_size <= 0 || image_stat.st_nlink != 1)
    {
        return -EINVAL;
    }

    ret = _upgrade_kernel_check_active(stable_kernel_slot);
    if (ret != 0)
    {
        return ret;
    }

    image_fd = open(image_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (image_fd < 0)
    {
        return -errno;
    }
    stable_fd = _upgrade_kernel_open_mtd(stable_kernel_slot, false, &stable_info);
    if (stable_fd < 0)
    {
        close(image_fd);
        return stable_fd;
    }

    ret = image_stat.st_size > (off_t)stable_info.size ? -EFBIG :
          _upgrade_kernel_hash(image_fd, (uint64_t)image_stat.st_size, stable_info.size - (uint64_t)image_stat.st_size, image_hash);
    if (ret == 0)
    {
        ret = _upgrade_kernel_hash(stable_fd, stable_info.size, 0U, flash_hash);
    }
    close(stable_fd);
    if (ret != 0)
    {
        close(image_fd);
        return ret;
    }

    if (memcmp(image_hash, flash_hash, sizeof(image_hash)) == 0)
    {
        close(image_fd);
        strcpy(target_kernel_slot, stable_kernel_slot);
        return 0;
    }

    target_fd = _upgrade_kernel_open_mtd(opposite, true, &target_info);
    if (target_fd < 0)
    {
        close(image_fd);
        return target_fd;
    }

    if (target_info.size != stable_info.size || image_stat.st_size > (off_t)target_info.size ||
        target_info.writesize == 0U || (uint64_t)image_stat.st_size % target_info.writesize != 0U)
    {
        ret = -EINVAL;
    }
    else
    {
        ret = _upgrade_kernel_flash(image_fd, target_fd, (uint64_t)image_stat.st_size, target_info.size);
    }
    close(image_fd);
    close(target_fd);
    if (ret != 0)
    {
        return ret;
    }

    // 重新打开备用MTD，确保读回校验取自设备，而不是写入时的缓存。
    target_fd = _upgrade_kernel_open_mtd(opposite, false, &target_info);
    if (target_fd < 0)
    {
        return target_fd;
    }
    ret = _upgrade_kernel_hash(target_fd, target_info.size, 0U, flash_hash);
    close(target_fd);
    if (ret != 0)
    {
        return ret;
    }
    if (memcmp(image_hash, flash_hash, sizeof(image_hash)) != 0)
    {
        return -EIO;
    }

    strcpy(target_kernel_slot, opposite);
    return 0;
}

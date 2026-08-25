/**
 * @file linkg_log.c
 * @brief LinkG日志模块实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-28
 */

#define _POSIX_C_SOURCE 200809L

#include "linkg_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "linkg_file.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_LOG_META_MAGIC                  0x4C474D31UL               // 元数据魔数：LGM1
#define LINKG_LOG_LEVEL_COUNT                 5U                         // 日志等级数量
#define LINKG_LOG_MESSAGE_MAX                 1024U                      // 用户日志内容最大长度
#define LINKG_LOG_LINE_MAX                    1400U                      // 完整日志行最大长度
#define LINKG_LOG_COLOR_LINE_MAX              1440U                      // 彩色日志行最大长度
#define LINKG_LOG_REPEAT_KEY_MAX              1200U                      // 重复日志比较键最大长度
#define LINKG_LOG_REPEAT_SUMMARY_MAX          128U                       // 重复日志摘要最大长度
#define LINKG_LOG_SESSION_HEADER_TEXT_MAX     512U                       // 会话头文本最大长度
#define LINKG_LOG_SPACE_CHECK_INTERVAL_US     5000000ULL                 // 剩余空间检测周期
#define LINKG_LOG_COLOR_RESET                 "\033[0m"                  // 终端颜色复位序列
#define LINKG_LOG_WRAP_MARKER                 "========== LOG BUFFER WRAPPED ==========\n" // 文件回绕标记

_Static_assert(LINKG_LOG_SESSION_HEADER_BYTES < LINKG_LOG_SESSION_FILE_MAX_BYTES, "session header must be smaller than session file");

/****************************** 内部类型 ******************************/

typedef enum
{
    LINKG_LOG_OUTPUT_CONSOLE = 0,    // 控制台输出
    LINKG_LOG_OUTPUT_FILE    = 1     // 文件输出
} linkg_log_output_type_t;

typedef struct
{
    uint32_t magic;              // 元数据魔数
    uint8_t  last_slot_id;       // 上一次使用的文件槽位
    uint8_t  last_session_id;    // 上一次使用的会话编号
    uint16_t reserved;           // 保留字段
    uint32_t checksum;           // 元数据校验值
} linkg_log_file_meta_t;

typedef struct
{
    linkg_log_output_type_t type;                                         // 输出类型
    FILE                   *stream;                                       // 输出流
    bool                    enable_color;                                 // 是否启用终端颜色
    bool                    is_used;                                      // 输出槽位是否使用
    bool                    disabled_by_space;                            // 是否因空间不足停写
    char                    base_dir[LINKG_LOG_PATH_MAX];                 // 日志目录
    char                    meta_path[LINKG_LOG_PATH_MAX];                // 元数据文件路径
    char                    session_path[LINKG_LOG_PATH_MAX];             // 当前会话文件路径
    char                    storage_check_path[LINKG_LOG_PATH_MAX];       // 剩余空间检测路径
    uint8_t                 slot_id;                                      // 当前文件槽位
    uint8_t                 session_id;                                   // 当前会话编号
    off_t                   write_pos;                                    // 当前文件写入位置
    bool                    wrapped;                                      // 文件是否已经回绕
    uint64_t                next_space_check_us;                          // 下次空间检测时间
    char                    last_message[LINKG_LOG_REPEAT_KEY_MAX];       // 上一条日志比较键
    linkg_log_level_t       last_level;                                   // 上一条日志等级
    uint32_t                repeat_count;                                 // 连续重复次数
    bool                    repeat_active;                                // 是否存在重复统计
} linkg_log_output_t;

typedef struct
{
    pthread_mutex_t    mutex;                              // 日志互斥锁
    linkg_log_level_t  level;                              // 最低输出等级
    linkg_log_output_t outputs[LINKG_LOG_OUTPUT_MAX];      // 日志输出列表
    bool               initialized;                        // 是否已经初始化
} linkg_logger_t;

/****************************** 全局上下文 ******************************/

static linkg_logger_t g_logger = {
    .mutex       = PTHREAD_MUTEX_INITIALIZER,
    .level       = LINKG_LOG_DEFAULT_LEVEL,
    .initialized = false
};

static const char *g_level_names[LINKG_LOG_LEVEL_COUNT] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR",
    "FATAL"
};

static const char *g_level_colors[LINKG_LOG_LEVEL_COUNT] = {
    "\033[36m",
    "\033[32m",
    "\033[33m",
    "\033[31m",
    "\033[41;37m"
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查等级is是否有效。
 */
static bool _level_is_valid(linkg_log_level_t level)
{
    return level >= LINKG_LOG_LEVEL_DEBUG && level <= LINKG_LOG_LEVEL_FATAL;
}

/**
 * @brief 获取路径中的文件名。
 */
static const char *_get_basename(const char *path)
{
    const char *basename;

    if (path == NULL)
    {
        return "unknown";
    }

    basename = strrchr(path, '/');
    return basename != NULL ? basename + 1 : path;
}

/**
 * @brief 复制字符串。
 */
static int _copy_string(char *destination, size_t destination_size, const char *source)
{
    size_t length;

    if (destination == NULL || destination_size == 0U || source == NULL)
    {
        return -EINVAL;
    }

    length = strnlen(source, destination_size);
    if (length >= destination_size)
    {
        destination[0] = '\0';
        return -ENAMETOOLONG;
    }

    memcpy(destination, source, length + 1U);
    return 0;
}

/**
 * @brief 移除路径末尾的斜杠。
 */
static void _trim_trailing_slashes(char *path)
{
    size_t length;

    if (path == NULL)
    {
        return;
    }

    length = strlen(path);
    while (length > 1U && path[length - 1U] == '/')
    {
        path[--length] = '\0';
    }
}

/**
 * @brief 根据日志等级决定是否同步文件数据。
 */
static void _sync_file_by_level(FILE *stream, linkg_log_level_t level)
{
#if LINKG_LOG_FSYNC_ON_ERROR_ONLY
    (void)linkg_file_stream_flush(stream, level >= LINKG_LOG_LEVEL_ERROR);
#else
    (void)level;
    (void)linkg_file_stream_flush(stream, true);
#endif
}

/**
 * @brief 检查日志存储空间是否充足。
 */
static bool _storage_has_enough_space(const char *path)
{
    uint64_t free_bytes;

    if (linkg_file_path_free_bytes(path, &free_bytes) != 0)
    {
        return false;
    }

    return free_bytes >= LINKG_LOG_STORAGE_MIN_FREE_BYTES;
}

/**
 * @brief 计算字节数据校验值。
 */
static uint32_t _checksum_bytes(const void *data, size_t length)
{
    const uint8_t *bytes;
    uint32_t       checksum;
    size_t         index;

    bytes    = (const uint8_t *)data;
    checksum = 2166136261U;

    for (index = 0U; index < length; index++)
    {
        checksum ^= bytes[index];
        checksum *= 16777619U;
    }

    return checksum;
}

/**
 * @brief 计算日志元数据校验值。
 */
static uint32_t _meta_checksum(const linkg_log_file_meta_t *meta)
{
    linkg_log_file_meta_t copy;

    if (meta == NULL)
    {
        return 0U;
    }

    copy          = *meta;
    copy.checksum = 0U;
    return _checksum_bytes(&copy, sizeof(copy));
}

/****************************** 元数据管理 ******************************/

/**
 * @brief 加载元数据。
 */
static int _meta_load(const char *path, linkg_log_file_meta_t *meta)
{
    FILE  *stream;
    int    result;

    if (path == NULL || meta == NULL)
    {
        return -EINVAL;
    }

    result = linkg_file_stream_open(path, "rb", &stream);
    if (result != 0)
        return result;
    result = linkg_file_stream_read_exact(stream, meta, sizeof(*meta));

    {
        int close_result;

        close_result = linkg_file_stream_close(&stream);
        if (result == 0)
        {
            result = close_result;
        }
    }

    if (result != 0)
    {
        return result;
    }

    if (meta->magic != LINKG_LOG_META_MAGIC || meta->checksum != _meta_checksum(meta))
    {
        return -EINVAL;
    }

    if (meta->last_slot_id >= LINKG_LOG_SESSION_MAX)
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 保存元数据。
 */
static int _meta_save(const char *path, const linkg_log_file_meta_t *meta)
{
    linkg_log_file_meta_t copy;

    if (path == NULL || meta == NULL)
    {
        return -EINVAL;
    }

    copy          = *meta;
    copy.magic    = LINKG_LOG_META_MAGIC;
    copy.checksum = 0U;
    copy.checksum = _meta_checksum(&copy);

    return linkg_file_write_all_atomic(path, &copy, sizeof(copy), 0644);
}

/**
 * @brief 生成日志输出文件路径。
 */
static int _build_output_paths(linkg_log_output_t *output)
{
    int length;

    if (output == NULL)
    {
        return -EINVAL;
    }

    length = snprintf(output->meta_path, sizeof(output->meta_path), "%s/%s", output->base_dir, LINKG_LOG_META_FILE_NAME);
    if (length < 0 || (size_t)length >= sizeof(output->meta_path))
    {
        return -ENAMETOOLONG;
    }

    length = snprintf(output->session_path, sizeof(output->session_path), "%s/" LINKG_LOG_SESSION_FILE_FORMAT, output->base_dir, (unsigned int)output->slot_id);
    if (length < 0 || (size_t)length >= sizeof(output->session_path))
    {
        return -ENAMETOOLONG;
    }

    return 0;
}

/****************************** 会话文件 ******************************/

/**
 * @brief 写入当前会话信息。
 */
static int _write_current_session_info(const linkg_log_output_t *output)
{
    char path[LINKG_LOG_PATH_MAX];
    FILE *stream = NULL;
    int   length;
    int   result;

    if (output == NULL)
    {
        return -EINVAL;
    }

    length = snprintf(path, sizeof(path), "%s/%s", output->base_dir, LINKG_LOG_CURRENT_SESSION_FILE_NAME);
    if (length < 0 || (size_t)length >= sizeof(path))
    {
        return -ENAMETOOLONG;
    }

    result = linkg_file_stream_open(path, "w", &stream);
    if (result == 0 && fprintf(stream,
                               "========== CURRENT SESSION ==========\n"
                               "slot_id=%02u\n"
                               "session_id=%u\n"
                               "file=" LINKG_LOG_SESSION_FILE_FORMAT "\n"
                               "=====================================\n",
                               (unsigned int)output->slot_id,
                               (unsigned int)output->session_id,
                               (unsigned int)output->slot_id) < 0)
    {
        result = -EIO;
    }

    if (result == 0)
    {
        result = linkg_file_stream_flush(stream, true);
    }

    {
        int close_result;

        close_result = linkg_file_stream_close(&stream);
        if (result == 0)
        {
            result = close_result;
        }
    }

    return result;
}

/**
 * @brief 格式化程序已运行时间。
 */
static int _format_elapsed_time(char *buffer, size_t buffer_size)
{
#if LINKG_LOG_USE_MICROSECOND
    return linkg_time_format_us(linkg_time_elapsed_us(), buffer, buffer_size);
#else
    return linkg_time_format_ms(linkg_time_elapsed_ms(), buffer, buffer_size);
#endif
}

/**
 * @brief 写入日志会话头。
 */
static int _write_session_header(linkg_log_output_t *output)
{
    char   header[LINKG_LOG_SESSION_HEADER_BYTES];
    char   text[LINKG_LOG_SESSION_HEADER_TEXT_MAX];
    char   time_text[LINKG_TIME_US_STR_SIZE];
    size_t copy_length;
    int    length;
    int    result;

    if (output == NULL || output->stream == NULL)
    {
        return -EINVAL;
    }

    if (_format_elapsed_time(time_text, sizeof(time_text)) != 0)
    {
        (void)_copy_string(time_text, sizeof(time_text), "00:00:00.000000");
    }

#if LINKG_LOG_USE_MICROSECOND
    length = snprintf(text, sizeof(text),
                      "========== SESSION START ==========\n"
                      "slot_id=%u\n"
                      "session_id=%u\n"
                      "time_base=monotonic_us_since_start\n"
                      "start_time=%s\n"
                      "===================================\n\n",
                      (unsigned int)output->slot_id,
                      (unsigned int)output->session_id,
                      time_text);
#else
    length = snprintf(text, sizeof(text),
                      "========== SESSION START ==========\n"
                      "slot_id=%u\n"
                      "session_id=%u\n"
                      "time_base=monotonic_ms_since_start\n"
                      "start_time=%s\n"
                      "===================================\n\n",
                      (unsigned int)output->slot_id,
                      (unsigned int)output->session_id,
                      time_text);
#endif

    if (length < 0 || (size_t)length >= sizeof(text) || (size_t)length >= sizeof(header))
    {
        return -EIO;
    }

    memset(header, ' ', sizeof(header));
    copy_length = (size_t)length;
    memcpy(header, text, copy_length);
    header[sizeof(header) - 1U] = '\n';

    result = linkg_file_stream_seek(output->stream, 0, SEEK_SET);
    if (result != 0)
    {
        return result;
    }

    result = linkg_file_stream_write_exact(output->stream, header, sizeof(header));
    if (result != 0)
    {
        return result;
    }

    return linkg_file_stream_flush(output->stream, false);
}

/**
 * @brief 写入会话文件。
 */
static int _session_file_write(linkg_log_output_t *output, const char *buffer, size_t length)
{
    const off_t  data_start    = (off_t)LINKG_LOG_SESSION_HEADER_BYTES;
    const off_t  file_max      = (off_t)LINKG_LOG_SESSION_FILE_MAX_BYTES;
    const size_t marker_length = sizeof(LINKG_LOG_WRAP_MARKER) - 1U;
    int          result;

    if (output == NULL || output->stream == NULL || buffer == NULL)
    {
        return -EINVAL;
    }

    if (length == 0U)
    {
        return 0;
    }

    if ((off_t)length > file_max - data_start)
    {
        return -E2BIG;
    }

    if (output->write_pos < data_start)
    {
        output->write_pos = data_start;
    }

    if (output->write_pos + (off_t)length > file_max)
    {
        output->write_pos = data_start;
        output->wrapped   = true;

        result = linkg_file_stream_seek(output->stream, output->write_pos, SEEK_SET);
        if (result != 0)
        {
            return result;
        }

        if ((off_t)(marker_length + length) <= file_max - data_start)
        {
            result = linkg_file_stream_write_exact(output->stream, LINKG_LOG_WRAP_MARKER, marker_length);
            if (result != 0)
            {
                return result;
            }

            output->write_pos += (off_t)marker_length;
        }
    }

    result = linkg_file_stream_seek(output->stream, output->write_pos, SEEK_SET);
    if (result != 0)
    {
        return result;
    }

    result = linkg_file_stream_write_exact(output->stream, buffer, length);
    if (result != 0)
    {
        return result;
    }

    output->write_pos += (off_t)length;
    return 0;
}

/****************************** 重复日志 ******************************/

/**
 * @brief 写入重复日志摘要。
 */
static int _flush_repeat_summary(linkg_log_output_t *output)
{
    char     buffer[LINKG_LOG_REPEAT_SUMMARY_MAX];
    uint32_t suppressed_count;
    int      length;

    if (output == NULL)
    {
        return -EINVAL;
    }

    if (!output->repeat_active)
    {
        return 0;
    }

    if (output->repeat_count <= LINKG_LOG_REPEAT_LIMIT)
    {
        output->repeat_active   = false;
        output->repeat_count    = 0U;
        output->last_message[0] = '\0';
        return 0;
    }

    suppressed_count = output->repeat_count - LINKG_LOG_REPEAT_LIMIT;
    length = snprintf(buffer, sizeof(buffer), "[REPEAT] previous message suppressed %u times\n", suppressed_count);

    if (length > 0 && (size_t)length < sizeof(buffer) && output->stream != NULL)
    {
        if (_session_file_write(output, buffer, (size_t)length) == 0)
        {
            _sync_file_by_level(output->stream, LINKG_LOG_LEVEL_WARNING);
        }
    }

    output->repeat_active   = false;
    output->repeat_count    = 0U;
    output->last_message[0] = '\0';
    return 0;
}

/**
 * @brief 判断是否抑制重复日志。
 */
static bool _should_suppress_repeat(linkg_log_output_t *output, linkg_log_level_t level, const char *message)
{
    if (output == NULL || message == NULL)
    {
        return false;
    }

    if (output->repeat_active && output->last_level == level && strcmp(output->last_message, message) == 0)
    {
        output->repeat_count++;
        return output->repeat_count > LINKG_LOG_REPEAT_LIMIT;
    }

    if (output->repeat_active)
    {
        (void)_flush_repeat_summary(output);
    }

    (void)_copy_string(output->last_message, sizeof(output->last_message), message);
    output->last_level    = level;
    output->repeat_count  = 1U;
    output->repeat_active = true;
    return false;
}

/****************************** 输出辅助 ******************************/

/**
 * @brief 在持锁状态下查找空闲日志输出。
 */
static int _find_free_output_locked(void)
{
    size_t index;

    for (index = 0U; index < LINKG_LOG_OUTPUT_MAX; index++)
    {
        if (!g_logger.outputs[index].is_used)
        {
            return (int)index;
        }
    }

    return -1;
}

/**
 * @brief 在持锁状态下关闭日志输出。
 */
static void _close_output_locked(linkg_log_output_t *output)
{
    if (output == NULL || !output->is_used)
    {
        return;
    }

    if (output->type == LINKG_LOG_OUTPUT_FILE && output->stream != NULL)
    {
        (void)_flush_repeat_summary(output);
        (void)linkg_file_stream_flush(output->stream, true);
        (void)linkg_file_stream_close(&output->stream);
    }

    memset(output, 0, sizeof(*output));
}

/**
 * @brief 因存储空间不足禁用文件日志。
 */
static void _disable_file_output_by_space(linkg_log_output_t *output)
{
    if (output == NULL || output->type != LINKG_LOG_OUTPUT_FILE)
    {
        return;
    }

    output->disabled_by_space = true;

    if (output->stream != NULL)
    {
        (void)linkg_file_stream_flush(output->stream, true);
        (void)linkg_file_stream_close(&output->stream);
    }
}

/**
 * @brief 规范化日志行内容。
 */
static size_t _normalize_log_line(char *buffer, size_t buffer_size, int formatted_length)
{
    size_t length;

    if (buffer == NULL || buffer_size < 2U || formatted_length < 0)
    {
        return 0U;
    }

    if ((size_t)formatted_length < buffer_size)
    {
        return (size_t)formatted_length;
    }

    length = buffer_size - 1U;
    buffer[length - 1U] = '\n';
    buffer[length]      = '\0';
    return length;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化日志。
 */
int  linkg_log_init(linkg_log_level_t min_level)
{
    if (!_level_is_valid(min_level))
    {
        return -1;
    }

    pthread_mutex_lock(&g_logger.mutex);

    if (g_logger.initialized)
    {
        pthread_mutex_unlock(&g_logger.mutex);
        return 0;
    }

    memset(g_logger.outputs, 0, sizeof(g_logger.outputs));
    g_logger.level       = min_level;
    g_logger.initialized = true;

    pthread_mutex_unlock(&g_logger.mutex);
    return 0;
}

/****************************** 输出管理 ******************************/

/**
 * @brief 添加控制台日志输出。
 */
int  linkg_log_add_console_output(bool use_stderr, bool enable_color)
{
    linkg_log_output_t *output;
    int                 output_id;

    pthread_mutex_lock(&g_logger.mutex);

    if (!g_logger.initialized)
    {
        pthread_mutex_unlock(&g_logger.mutex);
        return -1;
    }

    output_id = _find_free_output_locked();
    if (output_id < 0)
    {
        pthread_mutex_unlock(&g_logger.mutex);
        return -1;
    }

    output               = &g_logger.outputs[output_id];
    output->type         = LINKG_LOG_OUTPUT_CONSOLE;
    output->stream       = use_stderr ? stderr : stdout;
    output->enable_color = enable_color;
    output->is_used      = true;

    pthread_mutex_unlock(&g_logger.mutex);
    return output_id;
}

/**
 * @brief 添加目录输出。
 */
int  linkg_log_add_directory_output(const char *directory_path)
{
    linkg_log_output_t    output;
    linkg_log_file_meta_t meta;
    const char           *directory;
    int                   output_id;
    int                   result;

    directory = directory_path != NULL ? directory_path : LINKG_LOG_DEFAULT_DIR;

    pthread_mutex_lock(&g_logger.mutex);

    if (!g_logger.initialized)
    {
        pthread_mutex_unlock(&g_logger.mutex);
        return -1;
    }

    output_id = _find_free_output_locked();
    if (output_id < 0)
    {
        pthread_mutex_unlock(&g_logger.mutex);
        return -1;
    }

    memset(&output, 0, sizeof(output));
    output.type      = LINKG_LOG_OUTPUT_FILE;
    output.write_pos = (off_t)LINKG_LOG_SESSION_HEADER_BYTES;

    result = _copy_string(output.base_dir, sizeof(output.base_dir), directory);
    if (result == 0)
    {
        _trim_trailing_slashes(output.base_dir);
        result = _copy_string(output.storage_check_path, sizeof(output.storage_check_path), directory_path == NULL ? LINKG_LOG_STORAGE_CHECK_PATH : output.base_dir);
    }

    if (result == 0)
    {
        result = linkg_file_mkdirs(output.base_dir, 0775);
    }

    if (result == 0 && !_storage_has_enough_space(output.storage_check_path))
    {
        result = -ENOSPC;
    }

    if (result == 0)
    {
        int meta_length = snprintf(output.meta_path, sizeof(output.meta_path), "%s/%s", output.base_dir, LINKG_LOG_META_FILE_NAME);
        if (meta_length < 0 || (size_t)meta_length >= sizeof(output.meta_path))
        {
            result = -ENAMETOOLONG;
        }
    }

    if (result == 0)
    {
        if (_meta_load(output.meta_path, &meta) != 0)
        {
            memset(&meta, 0, sizeof(meta));
            meta.magic           = LINKG_LOG_META_MAGIC;
            meta.last_slot_id    = (uint8_t)(LINKG_LOG_SESSION_MAX - 1U);
            meta.last_session_id = (uint8_t)(LINKG_LOG_SESSION_ID_MODULO - 1U);
        }

        output.slot_id    = (uint8_t)((meta.last_slot_id + 1U) % LINKG_LOG_SESSION_MAX);
        output.session_id = (uint8_t)((meta.last_session_id + 1U) % LINKG_LOG_SESSION_ID_MODULO);
        result            = _build_output_paths(&output);
    }

    if (result == 0)
    {
        result = linkg_file_stream_open(output.session_path, "w+b", &output.stream);
    }

    if (result == 0)
    {
        result = _write_session_header(&output);
    }

    if (result == 0)
    {
        meta.last_slot_id    = output.slot_id;
        meta.last_session_id = output.session_id;
        result               = _meta_save(output.meta_path, &meta);
    }

    if (result == 0)
    {
        result = _write_current_session_info(&output);
    }

    if (result != 0)
    {
        if (output.stream != NULL)
        {
            (void)linkg_file_stream_close(&output.stream);
        }

        pthread_mutex_unlock(&g_logger.mutex);
        return -1;
    }

    output.is_used             = true;
    output.next_space_check_us = linkg_time_monotonic_us() + LINKG_LOG_SPACE_CHECK_INTERVAL_US;
    g_logger.outputs[output_id] = output;

    pthread_mutex_unlock(&g_logger.mutex);
    return output_id;
}

/**
 * @brief 添加默认目录日志输出。
 */
int  linkg_log_add_default_directory_output(void)
{
    return linkg_log_add_directory_output(NULL);
}

/**
 * @brief 移除指定日志输出。
 */
void linkg_log_remove_output(int output_id)
{
    if (output_id < 0 || output_id >= (int)LINKG_LOG_OUTPUT_MAX)
    {
        return;
    }

    pthread_mutex_lock(&g_logger.mutex);

    if (g_logger.initialized)
    {
        _close_output_locked(&g_logger.outputs[output_id]);
    }

    pthread_mutex_unlock(&g_logger.mutex);
}

/****************************** 运行控制 ******************************/

/**
 * @brief 设置最低日志输出等级。
 */
void linkg_log_set_level(linkg_log_level_t level)
{
    if (!_level_is_valid(level))
    {
        return;
    }

    pthread_mutex_lock(&g_logger.mutex);

    if (g_logger.initialized)
    {
        g_logger.level = level;
    }

    pthread_mutex_unlock(&g_logger.mutex);
}

/**
 * @brief 刷新所有日志输出。
 */
void linkg_log_flush(void)
{
    size_t index;

    pthread_mutex_lock(&g_logger.mutex);

    if (!g_logger.initialized)
    {
        pthread_mutex_unlock(&g_logger.mutex);
        return;
    }

    for (index = 0U; index < LINKG_LOG_OUTPUT_MAX; index++)
    {
        linkg_log_output_t *output = &g_logger.outputs[index];

        if (!output->is_used || output->stream == NULL)
        {
            continue;
        }

        (void)linkg_file_stream_flush(output->stream, output->type == LINKG_LOG_OUTPUT_FILE);
    }

    pthread_mutex_unlock(&g_logger.mutex);
}

/**
 * @brief 反初始化日志模块。
 */
void linkg_log_deinit(void)
{
    size_t index;

    pthread_mutex_lock(&g_logger.mutex);

    if (!g_logger.initialized)
    {
        pthread_mutex_unlock(&g_logger.mutex);
        return;
    }

    for (index = 0U; index < LINKG_LOG_OUTPUT_MAX; index++)
    {
        _close_output_locked(&g_logger.outputs[index]);
    }

    g_logger.level       = LINKG_LOG_DEFAULT_LEVEL;
    g_logger.initialized = false;

    pthread_mutex_unlock(&g_logger.mutex);
}

/****************************** 日志写入 ******************************/

/**
 * @brief 格式化并写入一条日志。
 */
void linkg_log_write_internal(linkg_log_level_t level, const char *file, int line, const char *format, ...)
{
    char                time_text[LINKG_TIME_US_STR_SIZE];
    char                user_message[LINKG_LOG_MESSAGE_MAX];
    char                plain_line[LINKG_LOG_LINE_MAX];
    char                color_line[LINKG_LOG_COLOR_LINE_MAX];
    char                repeat_key[LINKG_LOG_REPEAT_KEY_MAX];
    const char         *basename;
    uint64_t            now_us;
    size_t              plain_length;
    size_t              color_length;
    size_t              index;
    int                 length;
    va_list             arguments;

    if (!_level_is_valid(level) || format == NULL)
    {
        return;
    }

    va_start(arguments, format);
    (void)vsnprintf(user_message, sizeof(user_message), format, arguments);
    va_end(arguments);

    if (_format_elapsed_time(time_text, sizeof(time_text)) != 0)
    {
        (void)_copy_string(time_text, sizeof(time_text), "00:00:00.000000");
    }

    basename = _get_basename(file);

    length = snprintf(plain_line, sizeof(plain_line), "[%s] [%-5s] [%s:%d] %s\n", time_text, g_level_names[level], basename, line, user_message);
    plain_length = _normalize_log_line(plain_line, sizeof(plain_line), length);

    length = snprintf(color_line, sizeof(color_line), "[%s] %s[%-5s]%s [%s:%d] %s\n", time_text, g_level_colors[level], g_level_names[level], LINKG_LOG_COLOR_RESET, basename, line, user_message);
    color_length = _normalize_log_line(color_line, sizeof(color_line), length);

    length = snprintf(repeat_key, sizeof(repeat_key), "[%-5s] [%s:%d] %s", g_level_names[level], basename, line, user_message);
    if (length < 0)
    {
        repeat_key[0] = '\0';
    }
    else if ((size_t)length >= sizeof(repeat_key))
    {
        repeat_key[sizeof(repeat_key) - 1U] = '\0';
    }

    now_us = linkg_time_monotonic_us();

    pthread_mutex_lock(&g_logger.mutex);

    if (!g_logger.initialized || level < g_logger.level)
    {
        pthread_mutex_unlock(&g_logger.mutex);
        return;
    }

    for (index = 0U; index < LINKG_LOG_OUTPUT_MAX; index++)
    {
        linkg_log_output_t *output = &g_logger.outputs[index];

        if (!output->is_used)
        {
            continue;
        }

        if (output->type == LINKG_LOG_OUTPUT_CONSOLE)
        {
            const char *line_buffer = output->enable_color ? color_line : plain_line;
            size_t      line_length = output->enable_color ? color_length : plain_length;

            if (output->stream != NULL && line_length > 0U)
            {
                (void)linkg_file_stream_write_exact(output->stream, line_buffer, line_length);
                (void)fflush(output->stream);
            }

            continue;
        }

        if (output->type != LINKG_LOG_OUTPUT_FILE || output->disabled_by_space || output->stream == NULL)
        {
            continue;
        }

        if (now_us >= output->next_space_check_us)
        {
            output->next_space_check_us = now_us + LINKG_LOG_SPACE_CHECK_INTERVAL_US;

            if (!_storage_has_enough_space(output->storage_check_path))
            {
                _disable_file_output_by_space(output);
                continue;
            }
        }

        if (_should_suppress_repeat(output, level, repeat_key))
        {
            continue;
        }

        if (plain_length == 0U || _session_file_write(output, plain_line, plain_length) != 0)
        {
            continue;
        }

        _sync_file_by_level(output->stream, level);
    }

    pthread_mutex_unlock(&g_logger.mutex);
}

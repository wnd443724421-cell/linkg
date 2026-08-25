/**
 * @file linkg_log.h
 * @brief LinkG通用日志接口
 */

#ifndef LINKG_LOG_H
#define LINKG_LOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 编译器属性 ******************************/

#if defined(__GNUC__) || defined(__clang__)
#define LINKG_LOG_PRINTF_ATTR(format_index, first_argument) __attribute__((format(printf, format_index, first_argument))) // printf格式检查
#else
#define LINKG_LOG_PRINTF_ATTR(format_index, first_argument)                                                               // 空属性
#endif

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_LOG_LEVEL_DEBUG   = 0, // 调试信息
    LINKG_LOG_LEVEL_INFO    = 1, // 常规信息
    LINKG_LOG_LEVEL_WARNING = 2, // 警告信息
    LINKG_LOG_LEVEL_ERROR   = 3, // 错误信息
    LINKG_LOG_LEVEL_FATAL   = 4  // 致命错误
} linkg_log_level_t;             // 日志等级

/****************************** 默认配置 ******************************/

#define LINKG_LOG_DEFAULT_LEVEL             LINKG_LOG_LEVEL_DEBUG       // 默认日志等级
#define LINKG_LOG_DEFAULT_DIR               "/app/logger"               // 默认日志目录
#define LINKG_LOG_STORAGE_CHECK_PATH        "/app"                      // 空间检测路径
#define LINKG_LOG_META_FILE_NAME            "session.meta"              // 会话元数据文件
#define LINKG_LOG_CURRENT_SESSION_FILE_NAME "current_session.info"      // 当前会话信息文件
#define LINKG_LOG_SESSION_FILE_FORMAT       "session_%02u.log"          // 会话日志文件格式
#define LINKG_LOG_PATH_MAX                  256U                        // 日志路径最大长度
#define LINKG_LOG_OUTPUT_MAX                4U                          // 最大输出数量
#define LINKG_LOG_SESSION_MAX               16U                         // 最大会话数量
#define LINKG_LOG_SESSION_ID_MODULO         256U                        // 会话编号循环模数
#define LINKG_LOG_SESSION_FILE_MAX_BYTES    (128U * 1024U)              // 单会话文件最大长度
#define LINKG_LOG_SESSION_HEADER_BYTES      1024U                       // 会话文件头保留长度
#define LINKG_LOG_STORAGE_MIN_FREE_BYTES    (4ULL * 1024ULL * 1024ULL)  // 分区最小剩余空间
#define LINKG_LOG_REPEAT_LIMIT              10U                         // 相同日志连续输出上限
#define LINKG_LOG_USE_MICROSECOND           1                           // 使用微秒时间戳
#define LINKG_LOG_FSYNC_ON_ERROR_ONLY       1                           // 仅错误日志执行fsync

/****************************** 生命周期 ******************************/

int  linkg_log_init(linkg_log_level_t min_level);
void linkg_log_deinit(void);

/****************************** 输出管理 ******************************/

int  linkg_log_add_console_output(bool use_stderr, bool enable_color);
int  linkg_log_add_directory_output(const char *directory_path);
int  linkg_log_add_default_directory_output(void);
void linkg_log_remove_output(int output_id);

/****************************** 日志控制 ******************************/

void linkg_log_set_level(linkg_log_level_t level);
void linkg_log_flush(void);

/****************************** 日志写入 ******************************/

void linkg_log_write_internal(linkg_log_level_t level, const char *file, int line, const char *format, ...) LINKG_LOG_PRINTF_ATTR(4, 5);

/****************************** 日志宏 ******************************/

#define LINKG_LOG_DEBUG(...) linkg_log_write_internal(LINKG_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)   // 调试日志
#define LINKG_LOG_INFO(...)  linkg_log_write_internal(LINKG_LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__)    // 常规日志
#define LINKG_LOG_WARN(...)  linkg_log_write_internal(LINKG_LOG_LEVEL_WARNING, __FILE__, __LINE__, __VA_ARGS__) // 警告日志
#define LINKG_LOG_ERROR(...) linkg_log_write_internal(LINKG_LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)   // 错误日志
#define LINKG_LOG_FATAL(...) linkg_log_write_internal(LINKG_LOG_LEVEL_FATAL, __FILE__, __LINE__, __VA_ARGS__)   // 致命日志

#ifdef __cplusplus
}
#endif

#endif // LINKG_LOG_H

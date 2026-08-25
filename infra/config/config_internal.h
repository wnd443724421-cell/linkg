/**
 * @file config_internal.h
 * @brief 配置模块内部公共定义
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-25
 */

#ifndef CONFIG_INTERNAL_H
#define CONFIG_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************** 错误类型 ******************************/

typedef enum
{
    CONFIG_OK           =  0, // 操作成功
    CONFIG_ERR_PARAM    = -1, // 参数无效
    CONFIG_ERR_FILE     = -2, // 文件操作失败
    CONFIG_ERR_PARSE    = -3, // 配置解析失败
    CONFIG_ERR_VALIDATE = -4, // 配置校验失败
    CONFIG_ERR_MEMORY   = -5  // 内存分配失败
} config_error_t;

/****************************** 错误处理 ******************************/

const char *config_error_string(int error);
int         config_json_parse_error(int error);
int         config_json_write_error(int error);

/****************************** 通用校验 ******************************/

bool config_string_length_valid(const char *string, size_t min_length, size_t max_length);
bool config_int_range_valid(int value, int min_value, int max_value);
bool config_uint32_range_valid(uint32_t value, uint32_t min_value, uint32_t max_value);

#endif

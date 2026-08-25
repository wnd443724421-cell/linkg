/**
 * @file config_internal.c
 * @brief 配置模块内部公共定义实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-25
 */

#include "config_internal.h"

#include <string.h>

#include "linkg_json.h"

/****************************** 错误处理 ******************************/

/**
 * @brief 获取配置错误描述。
 */
const char *config_error_string(int error)
{
    switch (error)
    {
        case CONFIG_OK:
            return "操作成功";

        case CONFIG_ERR_PARAM:
            return "参数无效";

        case CONFIG_ERR_FILE:
            return "文件操作失败";

        case CONFIG_ERR_PARSE:
            return "配置解析失败";

        case CONFIG_ERR_VALIDATE:
            return "配置校验失败";

        case CONFIG_ERR_MEMORY:
            return "内存分配失败";

        default:
            return "未知配置错误";
    }
}

/**
 * @brief 将JSON读取错误转换为配置错误。
 */
int config_json_parse_error(int error)
{
    switch (error)
    {
        case LINKG_JSON_OK:
            return CONFIG_OK;

        case LINKG_JSON_ERR_PARAM:
            return CONFIG_ERR_PARAM;

        case LINKG_JSON_ERR_MEMORY:
            return CONFIG_ERR_MEMORY;

        case LINKG_JSON_ERR_RANGE:
        case LINKG_JSON_ERR_BUFFER_SMALL:
            return CONFIG_ERR_VALIDATE;

        default:
            return CONFIG_ERR_PARSE;
    }
}

/**
 * @brief 将JSON写入错误转换为配置错误。
 */
int config_json_write_error(int error)
{
    switch (error)
    {
        case LINKG_JSON_OK:
            return CONFIG_OK;

        case LINKG_JSON_ERR_PARAM:
        case LINKG_JSON_ERR_TYPE:
            return CONFIG_ERR_PARAM;

        case LINKG_JSON_ERR_RANGE:
        case LINKG_JSON_ERR_BUFFER_SMALL:
            return CONFIG_ERR_VALIDATE;

        case LINKG_JSON_ERR_MEMORY:
            return CONFIG_ERR_MEMORY;

        default:
            return CONFIG_ERR_PARSE;
    }
}

/****************************** 通用校验 ******************************/

/**
 * @brief 检查配置字符串长度是否有效。
 */
bool config_string_length_valid(const char *string, size_t min_length, size_t max_length)
{
    size_t length;

    if (string == NULL || min_length > max_length)
    {
        return false;
    }

    length = strlen(string);

    return length >= min_length && length <= max_length;
}

/**
 * @brief 检查有符号配置值是否在有效范围内。
 */
bool config_int_range_valid(int value, int min_value, int max_value)
{
    if (min_value > max_value)
    {
        return false;
    }

    return value >= min_value && value <= max_value;
}

/**
 * @brief 检查无符号配置值是否在有效范围内。
 */
bool config_uint32_range_valid(uint32_t value, uint32_t min_value, uint32_t max_value)
{
    if (min_value > max_value)
    {
        return false;
    }

    return value >= min_value && value <= max_value;
}

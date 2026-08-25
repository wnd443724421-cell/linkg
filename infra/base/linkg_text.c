/**
 * @file linkg_text.c
 * @brief LinkG通用文本处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-26
 */

#include "linkg_text.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "linkg_log.h"

/****************************** 日志定义 ******************************/

#define LINKG_TEXT_LOG_TAG "TEXT" // 文本模块日志标签

#define TEXT_ERROR(fmt, ...) LINKG_LOG_ERROR("%s: " fmt, LINKG_TEXT_LOG_TAG, ##__VA_ARGS__) // 文本模块错误日志

/****************************** 内部类型 ******************************/

typedef enum
{
    LINKG_TEXT_EDIT_REPLACE = 0, // 替换已存在的键值
    LINKG_TEXT_EDIT_SET,         // 替换或追加键值
    LINKG_TEXT_EDIT_REMOVE       // 删除指定键值
} linkg_text_edit_mode_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查键名称是否合法。
 */
static bool _text_key_valid(const char *key)
{
    const char *cursor;

    if (key == NULL || key[0] == '\0')
    {
        return false;
    }

    for (cursor = key; *cursor != '\0'; cursor++)
    {
        if (*cursor == '=' || *cursor == '#' ||
            *cursor == '\r' || *cursor == '\n' ||
            *cursor == ' ' || *cursor == '\t')
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 判断配置行是否对应指定键。
 *
 * @note 允许行首及键与等号之间存在空格或制表符，忽略空行和注释行。
 */
static bool _text_line_matches_key(const char *line, size_t line_length, const char *key)
{
    size_t key_length;
    size_t offset;

    if (line == NULL || key == NULL || key[0] == '\0')
    {
        return false;
    }

    key_length = strlen(key);
    offset     = 0U;

    // 跳过行首空格和制表符。
    while (offset < line_length && (line[offset] == ' ' || line[offset] == '\t'))
    {
        offset++;
    }

    // 忽略空行和注释行。
    if (offset >= line_length || line[offset] == '#')
    {
        return false;
    }

    // 检查行首内容是否与目标键一致。
    if (line_length - offset < key_length || memcmp(line + offset, key, key_length) != 0)
    {
        return false;
    }

    offset += key_length;

    // 跳过键与等号之间的空格和制表符。
    while (offset < line_length && (line[offset] == ' ' || line[offset] == '\t'))
    {
        offset++;
    }

    return offset < line_length && line[offset] == '=';
}

/**
 * @brief 修改内存文本中的指定键值。
 *
 * @note 替换操作只保留第一个同名键，删除操作移除全部同名键。
 */
static int _text_edit_key_value(char **content, size_t *length, const char *key, const char *value, linkg_text_edit_mode_t mode)
{
    char       *output;
    const char *input;
    size_t      capacity;
    size_t      input_offset;
    size_t      line_start;
    size_t      line_length;
    size_t      segment_length;
    size_t      output_length;
    size_t      key_length;
    size_t      value_length;
    size_t      replacement_length;
    bool        remove;
    bool        found;

    if (content == NULL || *content == NULL || length == NULL)
    {
        TEXT_ERROR("invalid text buffer parameter");
        return -EINVAL;
    }

    if (!_text_key_valid(key))
    {
        TEXT_ERROR("invalid text key");
        return -EINVAL;
    }

    if (mode != LINKG_TEXT_EDIT_REPLACE &&
        mode != LINKG_TEXT_EDIT_SET &&
        mode != LINKG_TEXT_EDIT_REMOVE)
    {
        TEXT_ERROR("invalid edit mode, mode=%d", (int)mode);
        return -EINVAL;
    }

    remove = mode == LINKG_TEXT_EDIT_REMOVE;

    if (!remove && value == NULL)
    {
        TEXT_ERROR("text value is null, key=%s", key);
        return -EINVAL;
    }

    if (!remove && (strchr(value, '\r') != NULL || strchr(value, '\n') != NULL))
    {
        TEXT_ERROR("text value contains line break, key=%s", key);
        return -EINVAL;
    }

    input              = *content;
    key_length         = strlen(key);
    value_length       = remove ? 0U : strlen(value);
    replacement_length = remove ? 0U : key_length + value_length + 2U;

    /**
     * 输出缓冲区需要容纳完整原文本、一条新键值、可能补充的换行符
     * 以及字符串结束符。
     */
    if (replacement_length > SIZE_MAX - 2U ||
        *length > SIZE_MAX - replacement_length - 2U)
    {
        TEXT_ERROR("text size overflow, length=%zu, replacement=%zu", *length, replacement_length);

        return -EOVERFLOW;
    }

    capacity = *length + replacement_length + 2U;
    output   = malloc(capacity);
    if (output == NULL)
    {
        TEXT_ERROR("allocate text buffer failed, capacity=%zu", capacity);
        return -ENOMEM;
    }

    input_offset  = 0U;
    output_length = 0U;
    found         = false;

    while (input_offset < *length)
    {
        line_start = input_offset;

        // 查找当前行结尾。
        while (input_offset < *length && input[input_offset] != '\n')
        {
            input_offset++;
        }

        // 当前行正文长度，不包含换行符。
        line_length = input_offset - line_start;

        // 跳过当前行末尾的换行符。
        if (input_offset < *length && input[input_offset] == '\n')
        {
            input_offset++;
        }

        // 当前完整行长度，包含可能存在的换行符。
        segment_length = input_offset - line_start;

        if (_text_line_matches_key(input + line_start, line_length, key))
        {
            /**
             * 第一次命中时写入新的键值。
             * 重复键和删除模式下的目标键均不写入输出文本。
             */
            if (!found && !remove)
            {
                memcpy(output + output_length, key, key_length);
                output_length += key_length;

                output[output_length++] = '=';

                memcpy(output + output_length, value, value_length);
                output_length += value_length;

                output[output_length++] = '\n';
            }

            found = true;
            continue;
        }

        // 非目标行保持原内容不变。
        memcpy(output + output_length, input + line_start, segment_length);
        output_length += segment_length;
    }

    if (!found)
    {
        if (mode == LINKG_TEXT_EDIT_REPLACE)
        {
            free(output);

            TEXT_ERROR("text key not found, key=%s", key);
            return -ENOENT;
        }

        if (mode == LINKG_TEXT_EDIT_REMOVE)
        {
            free(output);
            return 0;
        }

        // 原文本末尾没有换行符时先补充换行符。
        if (output_length > 0U && output[output_length - 1U] != '\n')
        {
            output[output_length++] = '\n';
        }

        memcpy(output + output_length, key, key_length);
        output_length += key_length;

        output[output_length++] = '=';

        memcpy(output + output_length, value, value_length);
        output_length += value_length;

        output[output_length++] = '\n';
    }

    output[output_length] = '\0';

    free(*content);

    *content = output;
    *length  = output_length;

    return 0;
}

/****************************** 键值操作 ******************************/

/**
 * @brief 替换文本中的指定键值。
 *
 * @note 指定键必须存在，多个同名键只保留一个。
 */
int  linkg_text_replace_key_value(char **content, size_t *length, const char *key, const char *value)
{
    return _text_edit_key_value(content, length, key, value, LINKG_TEXT_EDIT_REPLACE);
}

/**
 * @brief 设置文本中的指定键值。
 *
 * @note 键存在时替换，不存在时追加，多个同名键只保留一个。
 */
int  linkg_text_set_key_value(char **content, size_t *length, const char *key, const char *value)
{
    return _text_edit_key_value(content, length, key, value, LINKG_TEXT_EDIT_SET);
}

/**
 * @brief 删除文本中的指定键值。
 *
 * @note 删除全部同名有效键，指定键不存在时仍返回成功。
 */
int  linkg_text_remove_key_value(char **content, size_t *length, const char *key)
{
    return _text_edit_key_value(content, length, key, NULL, LINKG_TEXT_EDIT_REMOVE);
}

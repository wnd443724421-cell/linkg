/**
 * @file linkg_json.h
 * @brief LinkG通用JSON操作接口
 */

#ifndef LINKG_JSON_H
#define LINKG_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 错误类型 ******************************/

typedef enum
{
    LINKG_JSON_OK               =  0, // 操作成功
    LINKG_JSON_ERR_PARAM        = -1, // 参数无效
    LINKG_JSON_ERR_TYPE         = -2, // 字段类型错误
    LINKG_JSON_ERR_MISSING      = -3, // 必选字段缺失
    LINKG_JSON_ERR_RANGE        = -4, // 数值超出范围
    LINKG_JSON_ERR_BUFFER_SMALL = -5, // 接收缓冲区不足
    LINKG_JSON_ERR_MEMORY       = -6, // 内存分配失败
    LINKG_JSON_ERR_PARSE        = -7, // JSON解析失败
    LINKG_JSON_ERR_DUPLICATE    = -8  // JSON对象存在重复字段
} linkg_json_error_t;

/****************************** 错误处理 ******************************/

const char *linkg_json_error_string(int error);

/****************************** JSON解析 ******************************/

int linkg_json_parse(const char *text, cJSON **out);
int linkg_json_parse_buffer(const char *data, size_t length, cJSON **out);

/****************************** 必选字段 ******************************/

int linkg_json_get_bool(const cJSON *object, const char *key, bool *out);
int linkg_json_get_int(const cJSON *object, const char *key, int *out);
int linkg_json_get_uint16(const cJSON *object, const char *key, uint16_t *out);
int linkg_json_get_uint32(const cJSON *object, const char *key, uint32_t *out);
int linkg_json_get_string(const cJSON *object, const char *key, char *out, size_t out_size);
int linkg_json_get_object(const cJSON *object, const char *key, const cJSON **out);
int linkg_json_get_array(const cJSON *object, const char *key, const cJSON **out);

/****************************** 可选字段 ******************************/

int linkg_json_try_get_bool(const cJSON *object, const char *key, bool *out, bool *found);
int linkg_json_try_get_int(const cJSON *object, const char *key, int *out, bool *found);
int linkg_json_try_get_uint16(const cJSON *object, const char *key, uint16_t *out, bool *found);
int linkg_json_try_get_uint32(const cJSON *object, const char *key, uint32_t *out, bool *found);
int linkg_json_try_get_string(const cJSON *object, const char *key, char *out, size_t out_size, bool *found);
int linkg_json_try_get_object(const cJSON *object, const char *key, const cJSON **out, bool *found);
int linkg_json_try_get_array(const cJSON *object, const char *key, const cJSON **out, bool *found);

/****************************** 字段写入 ******************************/

int linkg_json_add_bool(cJSON *object, const char *key, bool value);
int linkg_json_add_int(cJSON *object, const char *key, int value);
int linkg_json_add_uint16(cJSON *object, const char *key, uint16_t value);
int linkg_json_add_uint32(cJSON *object, const char *key, uint32_t value);
int linkg_json_add_string(cJSON *object, const char *key, const char *value);
int linkg_json_add_object(cJSON *parent, const char *key, cJSON *child);
int linkg_json_add_array(cJSON *parent, const char *key, cJSON *child);

/****************************** JSON序列化 ******************************/

int  linkg_json_print_unformatted(const cJSON *root, char **out);
int  linkg_json_print_formatted(const cJSON *root, char **out);
void linkg_json_string_free(char *string);

#ifdef __cplusplus
}
#endif

#endif

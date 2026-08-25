/**
 * @file linkg_json.c
 * @brief LinkG通用JSON操作实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-22
 */

#include "linkg_json.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/****************************** 内部辅助 ******************************/

/**
 * @brief 按大小写精确查找JSON对象成员。
 * @note 返回的成员指针归object所有，调用方不得释放。
 */
static const cJSON *_json_find_member(const cJSON *object, const char *key)
{
    const cJSON *item;

    if (object == NULL || key == NULL || !cJSON_IsObject(object))
    {
        return NULL;
    }

    for (item = object->child; item != NULL; item = item->next)
    {
        if (item->string != NULL && strcmp(item->string, key) == 0)
        {
            return item;
        }
    }

    return NULL;
}

/**
 * @brief 递归检查JSON树中所有对象的字段名称是否唯一。
 */
static int _json_validate_unique_keys_recursive(const cJSON *node)
{
    const cJSON *item;
    const cJSON *other;
    int ret;

    if (node == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    if (cJSON_IsObject(node))
    {
        for (item = node->child; item != NULL; item = item->next)
        {
            if (item->string == NULL)
            {
                return LINKG_JSON_ERR_PARSE;
            }

            for (other = item->next; other != NULL; other = other->next)
            {
                if (other->string != NULL && strcmp(item->string, other->string) == 0)
                {
                    return LINKG_JSON_ERR_DUPLICATE;
                }
            }

            ret = _json_validate_unique_keys_recursive(item);
            if (ret != LINKG_JSON_OK)
            {
                return ret;
            }
        }
    }
    else if (cJSON_IsArray(node))
    {
        for (item = node->child; item != NULL; item = item->next)
        {
            ret = _json_validate_unique_keys_recursive(item);
            if (ret != LINKG_JSON_OK)
            {
                return ret;
            }
        }
    }

    return LINKG_JSON_OK;
}

/**
 * @brief 查找JSON对象成员并返回字段存在状态。
 */
static int _json_get_member(const cJSON *object, const char *key, const cJSON **item, bool *found)
{
    const cJSON *member;
    const cJSON *cursor;

    if (object == NULL || key == NULL || item == NULL || found == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    if (!cJSON_IsObject(object))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    member = NULL;

    for (cursor = object->child; cursor != NULL; cursor = cursor->next)
    {
        if (cursor->string == NULL || strcmp(cursor->string, key) != 0)
        {
            continue;
        }

        if (member != NULL)
        {
            return LINKG_JSON_ERR_DUPLICATE;
        }

        member = cursor;
    }

    *item = member;
    *found = member != NULL;

    return LINKG_JSON_OK;
}

/**
 * @brief 读取必选JSON对象成员。
 */
static int _json_get_required_member(const cJSON *object, const char *key, const cJSON **item)
{
    bool found;
    int  ret;

    if (item == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_member(object, key, item, &found);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    if (!found)
    {
        return LINKG_JSON_ERR_MISSING;
    }

    return LINKG_JSON_OK;
}

/**
 * @brief 将JSON数值转换为int。
 */
static int _json_number_to_int(const cJSON *item, int *out)
{
    double value;
    int    converted;

    if (item == NULL || out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    if (!cJSON_IsNumber(item))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    value = item->valuedouble;

    if (!isfinite(value) || value < (double)INT_MIN || value > (double)INT_MAX)
    {
        return LINKG_JSON_ERR_RANGE;
    }

    converted = (int)value;

    if ((double)converted != value)
    {
        return LINKG_JSON_ERR_RANGE;
    }

    *out = converted;

    return LINKG_JSON_OK;
}

/**
 * @brief 将JSON数值转换为uint32_t。
 */
static int _json_number_to_uint32(const cJSON *item, uint32_t *out)
{
    double   value;
    uint32_t converted;

    if (item == NULL || out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    if (!cJSON_IsNumber(item))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    value = item->valuedouble;

    if (!isfinite(value) || value < 0.0 || value > (double)UINT32_MAX)
    {
        return LINKG_JSON_ERR_RANGE;
    }

    converted = (uint32_t)value;

    if ((double)converted != value)
    {
        return LINKG_JSON_ERR_RANGE;
    }

    *out = converted;

    return LINKG_JSON_OK;
}

/**
 * @brief 将JSON字符串复制到调用方缓冲区。
 */
static int _json_string_copy(const cJSON *item, char *out, size_t out_size)
{
    size_t length;

    if (item == NULL || out == NULL || out_size == 0U)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return LINKG_JSON_ERR_TYPE;
    }

    length = strlen(item->valuestring);

    if (length >= out_size)
    {
        return LINKG_JSON_ERR_BUFFER_SMALL;
    }

    memcpy(out, item->valuestring, length + 1U);

    return LINKG_JSON_OK;
}

/**
 * @brief 将现有JSON节点挂载到父对象。
 *
 * @note 挂载成功后child所有权转移给parent；失败时仍归调用方。
 */
static int _json_attach_item(cJSON *parent, const char *key, cJSON *child)
{
    int ret;

    if (parent == NULL || key == NULL || child == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    if (!cJSON_IsObject(parent))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    /**
     * child必须是未挂载节点，避免同一个节点同时属于多个JSON树，
     * 否则会造成链表损坏或重复释放。
     */
    if (child->next != NULL || child->prev != NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    // 拒绝在已经包含重复字段的父对象上继续写入。
    ret = _json_validate_unique_keys_recursive(parent);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    // 拒绝挂载内部已经包含重复字段的对象或数组。
    ret = _json_validate_unique_keys_recursive(child);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    // 对象字段名称必须唯一。
    if (_json_find_member(parent, key) != NULL)
    {
        return LINKG_JSON_ERR_DUPLICATE;
    }

    // 直接检查cJSON返回值，避免依赖child->string判断挂载是否成功。
    if (!cJSON_AddItemToObject(parent, key, child))
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    return LINKG_JSON_OK;
}

/**
 * @brief 将新建JSON节点挂载到父对象。
 *
 * @note 添加失败时自动释放child。
 */
static int _json_add_created_item(cJSON *parent, const char *key, cJSON *child)
{
    int ret;

    if (child == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    ret = _json_attach_item(parent, key, child);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(child);
        return ret;
    }

    return LINKG_JSON_OK;
}

/****************************** 错误信息 ******************************/

/**
 * @brief 获取JSON错误描述。
 */
const char *linkg_json_error_string(int error)
{
    switch (error)
    {
        case LINKG_JSON_OK:
            return "操作成功";

        case LINKG_JSON_ERR_PARAM:
            return "参数无效";

        case LINKG_JSON_ERR_TYPE:
            return "JSON字段类型错误";

        case LINKG_JSON_ERR_MISSING:
            return "JSON必选字段缺失";

        case LINKG_JSON_ERR_RANGE:
            return "JSON数值超出范围或不是有效整数";

        case LINKG_JSON_ERR_BUFFER_SMALL:
            return "接收缓冲区不足";

        case LINKG_JSON_ERR_MEMORY:
            return "内存分配失败";

        case LINKG_JSON_ERR_PARSE:
            return "JSON解析失败";

        case LINKG_JSON_ERR_DUPLICATE:
            return "JSON对象存在重复字段";

        default:
            return "未知JSON错误";
    }
}

/****************************** JSON解析 ******************************/

/**
 * @brief 解析以字符串形式提供的JSON文本。
 *
 * @note 成功返回的JSON树归调用方所有，使用完成后必须调用cJSON_Delete释放。
 */
int linkg_json_parse(const char *text, cJSON **out)
{
    cJSON *root;
    int ret;

    if (text == NULL || out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    *out = NULL;

    /**
     * require_null_terminated设为1，确保整个字符串都是有效JSON，
     * 避免只解析前半段而忽略尾部非法字符。
     */
    root = cJSON_ParseWithOpts(text, NULL, 1);
    if (root == NULL)
    {
        return LINKG_JSON_ERR_PARSE;
    }

    ret = _json_validate_unique_keys_recursive(root);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(root);
        return ret;
    }

    *out = root;

    return LINKG_JSON_OK;
}

/**
 * @brief 解析指定长度的JSON缓冲区。
 *
 * @note 成功返回的JSON树归调用方所有，使用完成后必须调用cJSON_Delete释放。
 */
int linkg_json_parse_buffer(const char *data, size_t length, cJSON **out)
{
    char  *buffer;
    cJSON *root;
    int    ret;

    if (data == NULL || out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    *out = NULL;

    if (length == 0U)
    {
        return LINKG_JSON_ERR_PARSE;
    }

    if (length == SIZE_MAX)
    {
        return LINKG_JSON_ERR_RANGE;
    }

    /**
     * WebSocket、网络接收缓冲区不保证以'\0'结尾，
     * 因此复制到临时缓冲区并补充字符串结束符。
     */
    buffer = malloc(length + 1U);
    if (buffer == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    memcpy(buffer, data, length);
    buffer[length] = '\0';

    /**
     * JSON文本中不允许嵌入'\0'，否则解析器可能提前结束，
     * 导致后半段数据没有被校验。
     */
    if (memchr(buffer, '\0', length) != NULL)
    {
        free(buffer);
        return LINKG_JSON_ERR_PARSE;
    }

    root = cJSON_ParseWithOpts(buffer, NULL, 1);

    free(buffer);

    if (root == NULL)
    {
        return LINKG_JSON_ERR_PARSE;
    }

    ret = _json_validate_unique_keys_recursive(root);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(root);
        return ret;
    }

    *out = root;

    return LINKG_JSON_OK;
}

/****************************** 必选字段读取 ******************************/

/**
 * @brief 读取JSON布尔字段。
 */
int linkg_json_get_bool(const cJSON *object, const char *key, bool *out)
{
    const cJSON *item;
    int          ret;

    if (out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_required_member(object, key, &item);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    if (!cJSON_IsBool(item))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    *out = cJSON_IsTrue(item);

    return LINKG_JSON_OK;
}

/**
 * @brief 读取JSON整数字段。
 */
int linkg_json_get_int(const cJSON *object, const char *key, int *out)
{
    const cJSON *item;
    int          ret;

    if (out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_required_member(object, key, &item);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    return _json_number_to_int(item, out);
}

/**
 * @brief 读取JSON无符号16位整数字段。
 */
int linkg_json_get_uint16(const cJSON *object, const char *key, uint16_t *out)
{
    uint32_t value;
    int      ret;

    if (out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = linkg_json_get_uint32(object, key, &value);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    if (value > UINT16_MAX)
    {
        return LINKG_JSON_ERR_RANGE;
    }

    *out = (uint16_t)value;

    return LINKG_JSON_OK;
}

/**
 * @brief 读取JSON无符号32位整数字段。
 */
int linkg_json_get_uint32(const cJSON *object, const char *key, uint32_t *out)
{
    const cJSON *item;
    int          ret;

    if (out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_required_member(object, key, &item);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    return _json_number_to_uint32(item, out);
}

/**
 * @brief 读取JSON字符串字段。
 */
int linkg_json_get_string(const cJSON *object, const char *key, char *out, size_t out_size)
{
    const cJSON *item;
    int          ret;

    if (out == NULL || out_size == 0U)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_required_member(object, key, &item);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    return _json_string_copy(item, out, out_size);
}

/**
 * @brief 读取JSON对象字段。
 *
 * @note 返回的对象指针归父JSON树所有，调用方不得释放。
 */
int linkg_json_get_object(const cJSON *object, const char *key, const cJSON **out)
{
    const cJSON *item;
    int          ret;

    if (out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_required_member(object, key, &item);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    if (!cJSON_IsObject(item))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    *out = item;

    return LINKG_JSON_OK;
}

/**
 * @brief 读取JSON数组字段。
 *
 * @note 返回的数组指针归父JSON树所有，调用方不得释放。
 */
int linkg_json_get_array(const cJSON *object, const char *key, const cJSON **out)
{
    const cJSON *item;
    int          ret;

    if (out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_required_member(object, key, &item);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    if (!cJSON_IsArray(item))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    *out = item;

    return LINKG_JSON_OK;
}

/****************************** 可选字段读取 ******************************/

/**
 * @brief 尝试读取可选JSON布尔字段。
 */
int linkg_json_try_get_bool(const cJSON *object, const char *key, bool *out, bool *found)
{
    const cJSON *item;
    int          ret;

    if (out == NULL || found == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_member(object, key, &item, found);
    if (ret != LINKG_JSON_OK || !*found)
    {
        return ret;
    }

    if (!cJSON_IsBool(item))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    *out = cJSON_IsTrue(item);

    return LINKG_JSON_OK;
}

/**
 * @brief 尝试读取可选JSON整数字段。
 */
int linkg_json_try_get_int(const cJSON *object, const char *key, int *out, bool *found)
{
    const cJSON *item;
    int          ret;

    if (out == NULL || found == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_member(object, key, &item, found);
    if (ret != LINKG_JSON_OK || !*found)
    {
        return ret;
    }

    return _json_number_to_int(item, out);
}

/**
 * @brief 尝试读取可选JSON无符号16位整数字段。
 */
int linkg_json_try_get_uint16(const cJSON *object, const char *key, uint16_t *out, bool *found)
{
    uint32_t value;
    int      ret;

    if (out == NULL || found == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    *found = false;

    ret = linkg_json_try_get_uint32(object, key, &value, found);
    if (ret != LINKG_JSON_OK || !*found)
    {
        return ret;
    }

    if (value > UINT16_MAX)
    {
        return LINKG_JSON_ERR_RANGE;
    }

    *out = (uint16_t)value;

    return LINKG_JSON_OK;
}

/**
 * @brief 尝试读取可选JSON无符号32位整数字段。
 */
int linkg_json_try_get_uint32(const cJSON *object, const char *key, uint32_t *out, bool *found)
{
    const cJSON *item;
    int          ret;

    if (out == NULL || found == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_member(object, key, &item, found);
    if (ret != LINKG_JSON_OK || !*found)
    {
        return ret;
    }

    return _json_number_to_uint32(item, out);
}

/**
 * @brief 读取JSON字符串字段。
 */
int linkg_json_try_get_string(const cJSON *object, const char *key, char *out, size_t out_size, bool *found)
{
    const cJSON *item;
    int          ret;

    if (out == NULL || out_size == 0U || found == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_member(object, key, &item, found);
    if (ret != LINKG_JSON_OK || !*found)
    {
        return ret;
    }

    return _json_string_copy(item, out, out_size);
}

/**
 * @brief 尝试读取可选JSON对象字段。
 *
 * @note 返回的对象指针归父JSON树所有，调用方不得释放。
 */
int linkg_json_try_get_object(const cJSON *object, const char *key, const cJSON **out, bool *found)
{
    const cJSON *item;
    int          ret;

    if (out == NULL || found == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_member(object, key, &item, found);
    if (ret != LINKG_JSON_OK || !*found)
    {
        return ret;
    }

    if (!cJSON_IsObject(item))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    *out = item;

    return LINKG_JSON_OK;
}

/**
 * @brief 尝试读取可选JSON数组字段。
 *
 * @note 返回的数组指针归父JSON树所有，调用方不得释放。
 */
int linkg_json_try_get_array(const cJSON *object, const char *key, const cJSON **out, bool *found)
{
    const cJSON *item;
    int          ret;

    if (out == NULL || found == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ret = _json_get_member(object, key, &item, found);
    if (ret != LINKG_JSON_OK || !*found)
    {
        return ret;
    }

    if (!cJSON_IsArray(item))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    *out = item;

    return LINKG_JSON_OK;
}

/****************************** JSON写入 ******************************/

/**
 * @brief 添加JSON布尔字段。
 */
int linkg_json_add_bool(cJSON *object, const char *key, bool value)
{
    return _json_add_created_item(object, key, cJSON_CreateBool(value));
}

/**
 * @brief 添加JSON整数字段。
 */
int linkg_json_add_int(cJSON *object, const char *key, int value)
{
    return _json_add_created_item(object, key, cJSON_CreateNumber((double)value));
}

/**
 * @brief 添加JSON无符号16位整数字段。
 */
int linkg_json_add_uint16(cJSON *object, const char *key, uint16_t value)
{
    return linkg_json_add_uint32(object, key, (uint32_t)value);
}

/**
 * @brief 添加JSON无符号32位整数字段。
 */
int linkg_json_add_uint32(cJSON *object, const char *key, uint32_t value)
{
    return _json_add_created_item(object, key, cJSON_CreateNumber((double)value));
}

/**
 * @brief 添加字符串。
 */
int linkg_json_add_string(cJSON *object, const char *key, const char *value)
{
    if (value == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    return _json_add_created_item(object, key, cJSON_CreateString(value));
}

/**
 * @brief 添加JSON对象字段。
 *
 * @note 添加成功后child所有权转移给parent；添加失败时仍归调用方。
 */
int linkg_json_add_object(cJSON *parent, const char *key, cJSON *child)
{
    if (child == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    if (!cJSON_IsObject(child))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    return _json_attach_item(parent, key, child);
}

/**
 * @brief 添加JSON数组字段。
 *
 * @note 添加成功后child所有权转移给parent；添加失败时仍归调用方。
 */
int linkg_json_add_array(cJSON *parent, const char *key, cJSON *child)
{
    if (child == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    if (!cJSON_IsArray(child))
    {
        return LINKG_JSON_ERR_TYPE;
    }

    return _json_attach_item(parent, key, child);
}

/****************************** JSON序列化 ******************************/

/**
 * @brief 生成紧凑JSON字符串。
 *
 * @note 成功返回的字符串必须使用linkg_json_string_free释放。
 */
int linkg_json_print_unformatted(const cJSON *root, char **out)
{
    char *string;
    int ret;

    if (root == NULL || out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    *out = NULL;

    ret = _json_validate_unique_keys_recursive(root);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    string = cJSON_PrintUnformatted(root);
    if (string == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    *out = string;

    return LINKG_JSON_OK;
}

/**
 * @brief 生成格式化JSON字符串。
 *
 * @note 成功返回的字符串必须使用linkg_json_string_free释放。
 */
int linkg_json_print_formatted(const cJSON *root, char **out)
{
    char *string;
    int ret;

    if (root == NULL || out == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    *out = NULL;

    ret = _json_validate_unique_keys_recursive(root);
    if (ret != LINKG_JSON_OK)
    {
        return ret;
    }

    string = cJSON_Print(root);
    if (string == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    *out = string;

    return LINKG_JSON_OK;
}

/****************************** 内存释放 ******************************/

/**
 * @brief 释放JSON字符串。
 */
void linkg_json_string_free(char *string)
{
    if (string != NULL)
    {
        cJSON_free(string);
    }
}

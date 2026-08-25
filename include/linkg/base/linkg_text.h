/**
 * @file linkg_text.h
 * @brief LinkG通用文本处理接口
 */

#ifndef LINKG_TEXT_H
#define LINKG_TEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 键值文本 ******************************/

int linkg_text_replace_key_value(char **content, size_t *length, const char *key, const char *value);
int linkg_text_set_key_value(char **content, size_t *length, const char *key, const char *value);
int linkg_text_remove_key_value(char **content, size_t *length, const char *key);

#ifdef __cplusplus
}
#endif

#endif // LINKG_TEXT_H

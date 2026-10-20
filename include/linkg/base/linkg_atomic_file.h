/**
 * @file linkg_atomic_file.h
 * @brief 版本状态及启动控制共用的有界文件操作
 */

#ifndef LINKG_ATOMIC_FILE_H
#define LINKG_ATOMIC_FILE_H

#include <stddef.h>

/****************************** 文件操作 ******************************/

int linkg_atomic_read(const char *path, char *buffer, size_t capacity);
int linkg_atomic_write(const char *path, const char *buffer, size_t length);
int linkg_atomic_sync_parent(const char *path);
int linkg_atomic_remove(const char *path);

#endif

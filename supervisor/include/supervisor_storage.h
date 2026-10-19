/**
 * @file supervisor_storage.h
 * @brief Supervisor状态文件原子持久化接口
 */

#ifndef SUPERVISOR_STORAGE_H
#define SUPERVISOR_STORAGE_H

#include <stddef.h>

/****************************** 文件操作 ******************************/

int supervisor_storage_read(const char *path, char *buffer, size_t capacity);
int supervisor_storage_write(const char *path, const char *buffer, size_t length);
int supervisor_storage_sync_parent(const char *path);
int supervisor_storage_remove(const char *path);

#endif

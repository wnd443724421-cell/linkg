/**
 * @file linkg_file.h
 * @brief LinkG通用文件操作接口
 */

#ifndef LINKG_FILE_H
#define LINKG_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 宏定义 ******************************/

#define LINKG_FILE_PATH_MAX 4096U // 文件路径最大长度

/****************************** 文件流 ******************************/

int linkg_file_stream_open(const char *path, const char *mode, FILE **stream);
int linkg_file_stream_close(FILE **stream);
int linkg_file_stream_seek(FILE *stream, off_t offset, int whence);
int linkg_file_stream_read_exact(FILE *stream, void *buffer, size_t length);
int linkg_file_stream_write_exact(FILE *stream, const void *data, size_t length);
int linkg_file_stream_flush(FILE *stream, bool sync_to_disk);

/****************************** 文件系统 ******************************/

int linkg_file_mkdirs(const char *directory_path, mode_t mode);
int linkg_file_path_free_bytes(const char *path, uint64_t *free_bytes);

/****************************** 完整文件 ******************************/

int linkg_file_read_all(const char *path, size_t max_length, char **data, size_t *length);
int linkg_file_write_all(const char *path, const void *data, size_t length);
int linkg_file_write_all_atomic(const char *path, const void *data, size_t length, mode_t mode);
int linkg_file_replace_atomic(const char *source_path, const char *target_path, size_t max_length);

#ifdef __cplusplus
}
#endif

#endif // LINKG_FILE_H

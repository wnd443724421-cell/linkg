/**
 * @file upgrade_archive.h
 * @brief LinkG升级包解压内部接口
 */

#ifndef UPGRADE_ARCHIVE_H
#define UPGRADE_ARCHIVE_H

/****************************** 归档解压 ******************************/

int upgrade_archive_extract(const char *package_path, const char *output_dir);

#endif // UPGRADE_ARCHIVE_H

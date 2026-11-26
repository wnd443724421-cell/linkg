/**
 * @file linkg_at.h
 * @brief LinkG AT服务接口
 */

#ifndef LINKG_AT_H
#define LINKG_AT_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_at_init(void);
int linkg_at_start(void);
int linkg_at_stop(void);
int linkg_at_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // LINKG_AT_H

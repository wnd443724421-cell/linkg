/**
 * @file linkg_web.h
 * @brief LinkG Web服务接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-14
 */

#ifndef LINKG_WEB_H
#define LINKG_WEB_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_web_init(void);
int linkg_web_start(void);
int linkg_web_stop(void);
int linkg_web_deinit(void);

#ifdef __cplusplus
}
#endif

#endif

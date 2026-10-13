/**
 * @file tun_driver_loader.h
 * @brief LinkG TUN内核驱动加载与卸载接口
 */

#ifndef TUN_DRIVER_LOADER_H
#define TUN_DRIVER_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 驱动生命周期 ******************************/

int tun_driver_load(void);
int tun_driver_unload(void);

#ifdef __cplusplus
}
#endif

#endif // TUN_DRIVER_LOADER_H

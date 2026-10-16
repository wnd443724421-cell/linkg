/**
 * @file linkg_device_info.h
 * @brief LinkG统一设备信息只读查询接口
 */
#ifndef LINKG_DEVICE_INFO_H
#define LINKG_DEVICE_INFO_H

#include "linkg_device_config.h"
#include "linkg_wifi_hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LINKG_DEVICE_MODEL_MAX            32U // 产品型号缓冲区长度
#define LINKG_DEVICE_SERIAL_MAX           64U // SN缓冲区长度
#define LINKG_DEVICE_HARDWARE_VERSION_MAX 32U // PCB硬件版本缓冲区长度
#define LINKG_DEVICE_SOFTWARE_VERSION_MAX 24U // 正式发布版本缓冲区长度
#define LINKG_DEVICE_BUILD_DATE_MAX       16U // 构建日期缓冲区长度

typedef enum
{
    LINKG_DEVICE_FIELD_MISSING = 0, // 文件未烧录或值为空
    LINKG_DEVICE_FIELD_VALID,       // 读取成功且为安全文本
    LINKG_DEVICE_FIELD_ERROR        // 挂载、读取或内容错误
} linkg_device_field_state_t;

typedef struct
{
    char model[LINKG_DEVICE_MODEL_MAX];                       // 产品型号
    char serial_number[LINKG_DEVICE_SERIAL_MAX];              // Private SN Key
    char hardware_version[LINKG_DEVICE_HARDWARE_VERSION_MAX]; // Private HW Key
    char software_version[LINKG_DEVICE_SOFTWARE_VERSION_MAX]; // 正式软件发布版本
    char build_date[LINKG_DEVICE_BUILD_DATE_MAX];             // 实际构建日期 YYYY-MM-DD

    linkg_wifi_hardware_type_t wifi_type;                     // PA硬件类型
    linkg_device_role_t role;                                 // 当前配置角色
    linkg_device_field_state_t serial_state;                  // SN读取状态
    linkg_device_field_state_t hardware_state;                // HW读取状态
} linkg_device_info_t;

/* 主进程生命周期初始化一次；Private缺失或读取错误均不阻止程序启动。 */
int  linkg_device_info_init(void);
void linkg_device_info_deinit(void);

/* 返回时按值复制快照，不向调用方暴露内部缓存指针。 */
int linkg_device_info_get(linkg_device_info_t *out);

#ifdef __cplusplus
}
#endif

#endif // LINKG_DEVICE_INFO_H

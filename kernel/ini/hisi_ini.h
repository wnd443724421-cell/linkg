/**
 * Copyright (c) @CompanyNameMagicTag 2021-2023. All rights reserved.
 *
 * @file hisi_ini.h
 * @brief HI1105 INI 配置读取与文件版本管理接口
 * Author: @CompanyNameTag
 */

#ifndef HISI_INI_H
#define HISI_INI_H

#include "hw_bfg_ps.h"
#include "bfgx_user_ctrl.h"
#include "plat_type.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

/****************************** 编译选项 **************************************/

#ifndef _PRE_LINUX_TEST
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35))
#if (defined _PRE_CONFIG_READ_DYNCALI_E2PROM) || (defined _PRE_NO_HISI_NVRAM)

#else
#define HISI_NVRAM_SUPPORT                                           // 启用原厂NVRAM读取能力
#endif
#endif
#endif

/****************************** 公共宏定义 *************************************/

#define ini_min(_A, _B)               (((_A) < (_B)) ? (_A) : (_B))  // 取两个值中的较小值

/****************************** 日志接口 **************************************/

#ifdef _PRE_LINUX_TEST
#define ini_debug(fmt, arg...)                                       // 测试模式禁用DEBUG日志
#define ini_info(fmt, arg...)                                        // 测试模式禁用INFO日志
#define ini_warning(fmt, arg...)                                     // 测试模式禁用WARNING日志
#define ini_error(fmt, arg...)                                       // 测试模式禁用ERROR日志
#else
#ifdef _PRE_WINDOWS_SUPPORT
#define ini_debug                                                    // Windows模式禁用DEBUG日志
#define ini_info                                                     // Windows模式禁用INFO日志
#define ini_warning                                                  // Windows模式禁用WARNING日志
#define ini_error                                                    // Windows模式禁用ERROR日志
#else
#define ini_debug(fmt, arg...)        \
    do \
    { \
        if (oal_atomic_read(&g_hi110x_loglevel) >= PLAT_LOG_DEBUG) \
        { \
            printk(KERN_INFO "INI_DRV:D]%s:%d]" fmt "\n", __FUNCTION__, __LINE__, ##arg); \
        } \
    } while (0)

#define ini_info(fmt, arg...)         \
    do \
    { \
        if (oal_atomic_read(&g_hi110x_loglevel) >= PLAT_LOG_INFO) \
        { \
            printk(KERN_INFO "INI_DRV:D]%s:%d]" fmt "\n", __FUNCTION__, __LINE__, ##arg); \
        } \
    } while (0)

#define ini_warning(fmt, arg...)      \
    do \
    { \
        if (oal_atomic_read(&g_hi110x_loglevel) >= PLAT_LOG_WARNING) \
        { \
            printk(KERN_WARNING "INI_DRV:W]%s:%d]" fmt "\n", __FUNCTION__, __LINE__, ##arg); \
        } \
    } while (0)

#define ini_error(fmt, arg...)        \
    do \
    { \
        if (oal_atomic_read(&g_hi110x_loglevel) >= PLAT_LOG_ERR) \
        { \
            printk(KERN_ERR "INI_DRV:E]%s:%d]" fmt "\n\n\n", __FUNCTION__, __LINE__, ##arg); \
        } \
    } while (0)
#endif
#endif

/****************************** NVRAM常量 ***********************************/

#define HISI_CUST_NVRAM_READ          1                              // NVRAM读取操作
#define HISI_CUST_NVRAM_WRITE         0                              // NVRAM写入操作
#define HISI_CUST_NVRAM_NUM           340                            // NVRAM配置编号
#define HISI_CUST_NVRAM_LEN           104                            // NVRAM数据长度
#define HISI_CUST_NVRAM_NAME          "WINVRAM"                      // NVRAM配置名称

/****************************** 配置节名称 *************************************/

#define INI_MODU_WIFI                 "[HOST_WIFI_NORMAL]"           // Wi-Fi主机配置节
#define INI_MODU_GNSS                 "[HOST_GNSS_NORMAL]"           // GNSS主机配置节
#define INI_MODU_BT                   "[HOST_BT_NORMAL]"             // BT主机配置节
#define INI_MODU_FM                   "[HOST_FM_NORMAL]"             // FM主机配置节
#define INI_MODU_GT                   "[HOST_GT_NORMAL]"             // GT主机配置节
#define INI_MODU_PLAT                 "[HOST_PLAT]"                  // 平台配置节
#define INI_MODU_HOST_VERSION         "[HOST_VERSION]"               // 主机版本配置节
#define INI_MODU_WIFI_MAC             "[HOST_WIFI_MAC]"              // Wi-Fi地址配置节
#define INI_MODU_COEXIST              "[HOST_COEXIST]"               // 共存配置节
#define INI_MODU_DEV_WIFI             "[DEVICE_WIFI]"                // Wi-Fi设备配置节
#define INI_MODU_DEV_GNSS             "[DEVICE_GNSS]"                // GNSS设备配置节
#define INI_MODU_DEV_BT               "[DEVICE_BT]"                  // BT设备配置节
#define INI_MODU_DEV_FM               "[DEVICE_FM]"                  // FM设备配置节
#define INI_MODU_DEV_BFG_PLAT         "[DEVICE_BFG_PLAT]"            // BFG平台配置节

/****************************** 配置限制与返回值 **********************************/

#define MAX_READ_LINE_NUM             192                            // 单行最大读取长度
#define INI_FILE_PATH_LEN             128                            // 配置路径缓冲区长度
#define INI_READ_VALUE_LEN            64                             // 配置值读取长度
#define INI_SUCC                      0                              // 操作成功
#define INI_FAILED                    (-1)                           // 操作失败
#define INI_CONTINUE                  1                              // 继续处理
#define INI_FILE_TIMESPEC_UNRECONFIG  0                              // INI未变化
#define INI_FILE_TIMESPEC_RECONFIG    BIT0                           // INI发生变化
#define INI_NVRAM_RECONFIG            BIT1                           // NVRAM需要更新

/****************************** 类型定义 **************************************/

typedef enum
{
    INI_SECTION_WIFI, // Wi-Fi配置子系统
    INI_SECTION_GT,   // GT配置子系统
    INI_SECTION_BUTT  // 配置子系统数量边界
} ini_section_type_enum;

// HI1105_INI_FIX_BEGIN [2026-11-22] 新增目标文件版本结构，用于替代原秒级ctime判断
struct ini_file_version
{
    uint64_t ino;         // 实际目标文件的inode编号
    int64_t  ctime_sec;   // inode状态变更时间，秒
    long     ctime_nsec;  // inode状态变更时间，纳秒
    uint32_t dev;         // 文件所在设备号
    bool     present;     // 当前目标文件是否能够打开
};

struct ini_conf_version
{
    struct ini_file_version primary;   // 主INI文件版本
    struct ini_file_version override;  // 覆盖INI文件版本
};
// HI1105_INI_FIX_END [2026-11-22]

typedef uint32_t (*fun_wifi_customize_t)(void);

/****************************** 配置接口 **************************************/

int32_t get_cust_conf_int32(const char *tag, const char *var, int32_t *value);
int32_t get_cust_conf_string(const char *tag, const char *var, char *value, uint32_t size);

#ifndef _PRE_CONFIG_READ_DYNCALI_E2PROM
int32_t read_conf_from_nvram(uint8_t *pc_out, uint32_t size, uint32_t nv_number, const char *nv_name);
#endif

char *  get_ini_file_name(void);
int32_t ini_cfg_init(void);
void    ini_cfg_exit(void);
int32_t ko_read_line(oal_file_stru *fp, char *addr, uint32_t buf_len);
int32_t ini_file_check_conf_update(ini_section_type_enum type);

// HI1105_INI_FIX_BEGIN [2026-11-22] 新增两阶段文件版本检查与提交接口
int32_t ini_file_prepare_conf_update(ini_section_type_enum type, struct ini_conf_version *version);
void    ini_file_commit_conf_update(ini_section_type_enum type, const struct ini_conf_version *version);
// HI1105_INI_FIX_END [2026-11-22]

char *  get_host_ini_path(uint32_t chip_id);
char *  get_host_ini_conn_path(uint32_t chip_id);
bool    ini_file_exist(const char *file_path);

/****************************** 定制化回调接口 ***********************************/

void register_wifi_customize(fun_wifi_customize_t fun_wifi_customize);
void wifi_customize_h2d(void);
void register_gt_customize(fun_wifi_customize_t fun_wifi_customize);
void gt_customize_h2d(void);

/****************************** 可选存储接口 ************************************/

#ifdef _PRE_CONFIG_READ_DYNCALI_E2PROM
#ifdef _PRE_PRODUCT_HI3751_PLATO
#define ini_eeprom_read               drv_eeprom_read                // HI3751读取入口别名
#endif
#ifdef _PRE_PRODUCT_ARMPC
#define ini_eeprom_read               drv_e2prom_read                // ARMPC读取入口别名
#endif
#ifdef _PRE_PRODUCT_HI1620S_KUNPENG
#define ini_eeprom_read               DrvEepromRead                  // 鲲鹏读取入口别名
#endif
int32_t ini_eeprom_read(uint8_t *partName, uint32_t offSet, char *rBuf, uint32_t rSize);
int32_t read_conf_from_eeprom(uint8_t *pc_out, uint32_t size, uint32_t offset);
#endif

#ifdef _PRE_SUSPORT_OEMINFO
int32_t read_conf_from_oeminfo(uint8_t *pc_out, uint32_t size, uint32_t offset);
#endif

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif

#endif // HISI_INI_H

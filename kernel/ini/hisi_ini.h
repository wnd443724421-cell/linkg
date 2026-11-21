/**
 * Copyright (c) @CompanyNameMagicTag 2021-2023. All rights reserved.
 *
 * Description: hisi_ini.c header file
 * Author: @CompanyNameTag
 */

#ifndef HISI_INI_H
#define HISI_INI_H

/****************************** 头文件依赖 ******************************/
#include "hw_bfg_ps.h"
#include "bfgx_user_ctrl.h"
#include "plat_type.h"

/* C++ 链接兼容 */
#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

/****************************** 编译配置 ******************************/
#ifndef _PRE_LINUX_TEST
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35))
#if (defined _PRE_CONFIG_READ_DYNCALI_E2PROM) || (defined _PRE_NO_HISI_NVRAM)

#else
#define HISI_NVRAM_SUPPORT
#endif
#endif
#endif

/****************************** 公共宏定义 ******************************/
#define ini_min(_A, _B)        (((_A) < (_B)) ? (_A) : (_B))

/* INI 日志接口：保持原厂条件编译及宏实现 */
#ifdef _PRE_LINUX_TEST
#define ini_debug(fmt, arg...)
#define ini_info(fmt, arg...)
#define ini_warning(fmt, arg...)
#define ini_error(fmt, arg...)
#else
#ifdef _PRE_WINDOWS_SUPPORT
#define ini_debug
#define ini_info
#define ini_warning
#define ini_error
#else
#define ini_debug(fmt, arg...) \
    do {                                                                                   \
        if (oal_atomic_read(&g_hi110x_loglevel) >= PLAT_LOG_DEBUG) {                                         \
            printk(KERN_INFO "INI_DRV:D]%s:%d]" fmt "\n", __FUNCTION__, __LINE__, ##arg); \
        }                                                                                  \
    } while (0)

#define ini_info(fmt, arg...)                                                              \
    do {                                                                                   \
        if (oal_atomic_read(&g_hi110x_loglevel) >= PLAT_LOG_INFO) {                                          \
            printk(KERN_INFO "INI_DRV:D]%s:%d]" fmt "\n", __FUNCTION__, __LINE__, ##arg); \
        }                                                                                  \
    } while (0)

#define ini_warning(fmt, arg...)                                                             \
    do {                                                                                     \
        if (oal_atomic_read(&g_hi110x_loglevel) >= PLAT_LOG_WARNING) {                                         \
            printk(KERN_WARNING "INI_DRV:W]%s:%d]" fmt "\n", __FUNCTION__, __LINE__, ##arg); \
        }                                                                                    \
    } while (0)

#define ini_error(fmt, arg...)                                                               \
    do {                                                                                     \
        if (oal_atomic_read(&g_hi110x_loglevel) >= PLAT_LOG_ERR) {                                             \
            printk(KERN_ERR "INI_DRV:E]%s:%d]" fmt "\n\n\n", __FUNCTION__, __LINE__, ##arg); \
        }                                                                                    \
    } while (0)
#endif
#endif

/****************************** NVRAM 定义 ******************************/
#define HISI_CUST_NVRAM_READ  1
#define HISI_CUST_NVRAM_WRITE 0
#define HISI_CUST_NVRAM_NUM   340
#define HISI_CUST_NVRAM_LEN   104
#define HISI_CUST_NVRAM_NAME  "WINVRAM"

/****************************** 配置子系统类型 ******************************/
/*
 * INI 文件配置对应的子系统类型。
 * 原厂只定义 Wi-Fi 和 GT 两种类型。
 */
typedef enum {
    INI_SECTION_WIFI, /* WIFI子系统对应的INI配置 */
    INI_SECTION_GT,   /* GT子系统对应的INI配置 */

    INI_SECTION_BUTT
} ini_section_type_enum;

/****************************** INI 配置节名称 ******************************/
#define INI_MODU_WIFI         "[HOST_WIFI_NORMAL]"
#define INI_MODU_GNSS         "[HOST_GNSS_NORMAL]"
#define INI_MODU_BT           "[HOST_BT_NORMAL]"
#define INI_MODU_FM           "[HOST_FM_NORMAL]"
#define INI_MODU_GT           "[HOST_GT_NORMAL]"
#define INI_MODU_PLAT         "[HOST_PLAT]"
#define INI_MODU_HOST_VERSION "[HOST_VERSION]"
#define INI_MODU_WIFI_MAC     "[HOST_WIFI_MAC]"
#define INI_MODU_COEXIST      "[HOST_COEXIST]"
#define INI_MODU_DEV_WIFI     "[DEVICE_WIFI]"
#define INI_MODU_DEV_GNSS     "[DEVICE_GNSS]"
#define INI_MODU_DEV_BT       "[DEVICE_BT]"
#define INI_MODU_DEV_FM       "[DEVICE_FM]"
#define INI_MODU_DEV_BFG_PLAT "[DEVICE_BFG_PLAT]"

/****************************** 配置长度与返回码 ******************************/
#define MAX_READ_LINE_NUM   192
#define INI_FILE_PATH_LEN   128
#define INI_READ_VALUE_LEN  64

#define INI_SUCC   0
#define INI_FAILED (-1)
#define INI_CONTINUE  1

/* INI 重载检测返回值：保留原厂宏名称和取值 */
#define INI_FILE_TIMESPEC_UNRECONFIG    0
#define INI_FILE_TIMESPEC_RECONFIG      BIT0
#define INI_NVRAM_RECONFIG              BIT1

/****************************** INI 文件版本信息 ******************************/
/*
 * HI1105_INI_FIX_BEGIN [2026-11-22] 新增文件版本结构
 * 修改原因：旧宏 inf_file_get_ctime() 仅比较 ctime 秒值，无法识别同秒的不同文件。
 * 修改内容：删除旧宏；按实际打开的文件记录设备号、inode、完整 ctime 及存在状态。
 *           主配置文件和覆盖配置文件分别记录版本，避免缓存相互干扰。
 * 注意事项：字段顺序与 Codex 已验证的修复版本完全一致，不改变结构布局。
 */
struct ini_file_version {
    uint64_t ino;
    int64_t ctime_sec;
    long ctime_nsec;
    uint32_t dev;
    bool present;
};

struct ini_conf_version {
    struct ini_file_version primary;
    struct ini_file_version override;
};
/* HI1105_INI_FIX_END [2026-11-22] 新增文件版本结构 */

/****************************** 回调类型定义 ******************************/
typedef uint32_t (*fun_wifi_customize_t)(void);

/****************************** 公共接口声明 ******************************/
/* INI 参数读取 */
int32_t get_cust_conf_int32(const char *tag, const char *var, int32_t *value);
int32_t get_cust_conf_string(const char *tag, const char *var, char *value, uint32_t size);
#ifndef _PRE_CONFIG_READ_DYNCALI_E2PROM
int32_t read_conf_from_nvram(uint8_t *pc_out, uint32_t size, uint32_t nv_number, const char *nv_name);
#endif

/* INI 配置初始化及文件访问 */
char *get_ini_file_name(void);
int32_t ini_cfg_init(void);
void ini_cfg_exit(void);
int32_t ko_read_line(oal_file_stru *fp, char *addr, uint32_t buf_len);
/* 原厂兼容接口，声明保持不变 */
int32_t ini_file_check_conf_update(ini_section_type_enum type);

/*
 * HI1105_INI_FIX_BEGIN [2026-11-22] 新增 INI 版本检测与提交接口
 * prepare：取得当前文件版本，并判断是否需要重读；本阶段不更新缓存。
 * commit ：配置初始化函数返回成功后，提交上次检查得到的文件版本。
 * 保留原检查接口，避免影响其他子系统的调用约定。
 */
int32_t ini_file_prepare_conf_update(ini_section_type_enum type, struct ini_conf_version *version);
void ini_file_commit_conf_update(ini_section_type_enum type, const struct ini_conf_version *version);
/* HI1105_INI_FIX_END [2026-11-22] 新增 INI 版本检测与提交接口 */

char *get_host_ini_path(uint32_t chip_id);
char *get_host_ini_conn_path(uint32_t chip_id);
bool ini_file_exist(const char *file_path);

/* 平台定制化回调接口 */
void register_wifi_customize(fun_wifi_customize_t fun_wifi_customize);
void wifi_customize_h2d(void);
void register_gt_customize(fun_wifi_customize_t fun_wifi_customize);
void gt_customize_h2d(void);

/****************************** 可选 EEPROM/OEMINFO 接口 ******************************/
#ifdef _PRE_CONFIG_READ_DYNCALI_E2PROM
#ifdef _PRE_PRODUCT_HI3751_PLATO
#define ini_eeprom_read drv_eeprom_read
#endif
#ifdef _PRE_PRODUCT_ARMPC
#define ini_eeprom_read drv_e2prom_read
#endif

#ifdef _PRE_PRODUCT_HI1620S_KUNPENG
#define ini_eeprom_read DrvEepromRead
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

#endif /* HISI_INI_H */

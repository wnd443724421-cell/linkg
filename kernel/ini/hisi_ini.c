/**
 * Copyright (c) @CompanyNameMagicTag 2021-2023. All rights reserved.
 *
 * Description: Implementation of the ini configuration
 * Author: @CompanyNameTag
 */

/* 头文件包含 */
#include "hisi_ini.h"
#include "chr_user.h"
#include "oam_ext_if.h"

#include "board.h"

#ifdef HISI_NVRAM_SUPPORT
#include "nve_info_interface.h"
#endif /* end for HISI_NVRAM_SUPPORT */

#include "oal_util.h"
#include "securec.h"

#undef THIS_FILE_ID
#define THIS_FILE_ID OAM_FILE_ID_HISI_INI_C

/* 全局变量定义 */
#ifndef CUST_PATH_INI_CONN
#define CUST_PATH_INI_CONN "/data/vendor/cust_conn/ini_cfg" /* 某运营商在不同产品的差异配置 */
#endif
#ifndef INI_FILE_PATH_DEFAULT
#ifdef _PRE_WLAN_EXPORT
#define INI_FILE_PATH_DEFAULT "/app/cfg_hi1105.ini"
#else
#define INI_FILE_PATH_DEFAULT "/system/bin/wifi_hisi/cfg_e5_hisi.ini"
#endif
#endif
STATIC char g_ini_file_name[INI_FILE_PATH_LEN] = INI_FILE_PATH_DEFAULT;
STATIC char g_ini_conn_file_name[INI_FILE_PATH_LEN] = {0};

#if defined(_PRE_PRODUCT_HI1620S_KUNPENG) || defined(_PRE_WINDOWS_SUPPORT)
#ifdef _PRE_WINDOWS_SUPPORT
#define INI_PATH_INI_CONN "cfg_udp_1103_mpw2.ini"
#else
#define INI_PATH_INI_CONN "/vendor/etc/cfg_udp_1103_mpw2.ini"
#endif
#endif

#ifdef _PRE_SUSPORT_OEMINFO
#define WIFI_CALI_OEMINFO_PATH  "/data/vendor/wifi/wifi_calibrate.conf"
#endif

/* 按子系统记录已加载的主 INI 和覆盖 INI 文件版本。 */
STATIC struct ini_conf_version g_ini_conf_version[INI_SECTION_BUTT];
STATIC fun_wifi_customize_t g_fun_wifi_customize = NULL;
STATIC fun_wifi_customize_t g_fun_gt_customize = NULL;

#ifdef _PRE_CONFIG_READ_DYNCALI_E2PROM
#define MP13_TEL_HUAWEI_NV_WINVRAM_LENGTH 128
#define EEPROM_WIFI_CALI_PARTNAME    "WLAN"
#endif

STATIC bool is_line_character(char ch)
{
    return ((ch == '\n') || (ch == '\r'));
}

/*
 * 从当前文件指针位置开始，读一行(支持windows、linux、mac换行格式)
 * 一行最多读 buf_len - 1 个字节，大于此长度的会被截断
 * 末尾会添加字符串结束符，不返回任何换行符
 */
int32_t ko_read_line(oal_file_stru *fp, char *addr, uint32_t buf_len)
{
    int32_t index;
    int32_t ret = oal_file_read_ext(fp, fp->f_pos, addr, buf_len);
    if (ret <= 0) {
        ini_debug("kernel_line read ret = %d", ret);
        return INI_FAILED; // 读结束
    }

    // find the line end
    index = 0;
    ret = (ret < (int32_t)buf_len) ? ret : (int32_t)(buf_len - 1); // 预留1个字节结束符
    while ((index < ret) && (!is_line_character(addr[index]))) {
        index++;
    }
    addr[index++] = '\0';

    // move file pos to next line
    fp->f_pos += index;
    while ((index < ret) && is_line_character(addr[index])) {
        index++;
        fp->f_pos++;
    }

    return ret;
}

bool ini_file_exist(const char *file_path)
{
    oal_file_stru *fp = NULL;

    if (file_path == NULL) {
        ini_error("para file_path is NULL\n");
        return false;
    }

    fp = oal_file_open_readonly(file_path);
    if (fp == NULL) {
        ini_debug("%s not exist\n", file_path);
        return false;
    }

    oal_file_close(fp);

    ini_debug("%s exist\n", file_path);

    return true;
}

STATIC int32_t ini_str_trim_right(char *value, uint32_t value_len)
{
    uint32_t cnt;
    if (value_len < 1) {
        ini_error("ini_str_trim_right fail, value length is 0\n");
        return INI_FAILED;
    }

    /* 替换尾部空格为'\0' */
    cnt = value_len - 1;
    while ((cnt != 0) && (value[cnt] == ' ')) {
        value[cnt--] = '\0';
    }

    return INI_SUCC;
}

STATIC int32_t is_modu_exist(oal_file_stru *fp, const char *modu_name)
{
    int32_t ret = INI_FAILED;
    char tmp[MAX_READ_LINE_NUM] = {0};
    /* find the value of mode var, such as ini_wifi_mode
     * every mode except PLAT mode has only one mode var */
    for (;;) {
        ret = ko_read_line(fp, tmp, sizeof(tmp));
        if (ret == INI_FAILED) {
#ifdef _PRE_CONFIG_INI_PRINT_ERROR_TO_DEBUG
            ini_debug("not find %s!!!", modu_name);
#else
            ini_error("not find %s!!!", modu_name);
#endif
            break;
        }

        if (strncmp(tmp, modu_name, strlen(modu_name)) == 0) {
            ini_debug("have found %s", modu_name);
            return INI_SUCC;
        }
    }

    return INI_FAILED;
}

/*
 *  Prototype    : ini_find_var
 *  Description  : find difference mode variable value, and return value
 */
STATIC int32_t ini_find_var(oal_file_stru *fp, const char *tag, const char *var, char *value, uint32_t size)
{
    int32_t ret;
    char tmp[MAX_READ_LINE_NUM + 1] = {0};
    size_t search_var_len;

    /* find the modu of var, such as [HOST_WIFI_NORMAL] of wifi moduler */
    ret = is_modu_exist(fp, tag);
    if (ret != INI_SUCC) {
        return INI_FAILED;
    }

    /* find the var in modu, such as [HOST_WIFI_NORMAL] of wifi moduler */
    for (;;) {
        ret = ko_read_line(fp, tmp, sizeof(tmp));
        if (ret == INI_FAILED) {
            return INI_FAILED;
        }

        if (tmp[0] == '[') {
            ini_debug("not find %s!!!, check if var in correct mode", var);
            return INI_FAILED;
        }

        search_var_len = strlen(var);
        if ((strncmp(tmp, var, search_var_len) == 0) &&
            (tmp[search_var_len] == '=') && (tmp[search_var_len + 1] != ' ')) {  /* 等号左右两边不能有空格 */
            if (strncpy_s(value, size, &tmp[search_var_len + 1], size - 1)) {
                ini_error("ini_find_var:strncpy_s space is not enough");
                return INI_FAILED;
            }
            break;
        }
    }

    return INI_SUCC;
}

#ifdef _PRE_SUSPORT_OEMINFO
STATIC int32_t get_wifi_calibrate_param(uint32_t datalen, uint32_t offset, uint8_t *cali_param)
{
    int32_t ret;
    int wifi_cal_len = 0;
    uint32_t i;
    char data[MP13_TEL_HUAWEI_NV_WINVRAM_LENGTH];
    oal_file_stru *file = NULL;

    ret = memset_s(data, MP13_TEL_HUAWEI_NV_WINVRAM_LENGTH, 0, MP13_TEL_HUAWEI_NV_WINVRAM_LENGTH);
    if (ret != EOK) {
        ini_error("memset wifi cali data failed.\n");
        return INI_FAILED;
    }

    file = oal_file_open_readonly(WIFI_CALI_OEMINFO_PATH);
    if (file == NULL) {
        return INI_FAILED;
    }
    oal_file_read_ext(file, (loff_t)offset, data, datalen);
    oal_file_close(file);

    for (i = 0; i < datalen; i++) {
        if (data[i] == '\0') {
            wifi_cal_len = i + 1;
            break;
        }
    }
    ret = memcpy_s(cali_param, wifi_cal_len, data, wifi_cal_len);
    if (ret != EOK) {
        ini_error("memcpy wifi cal data failed\n");
        return INI_FAILED;
    }
    return INI_SUCC;
}
#endif

STATIC int32_t ini_find_var_value_by_path(const char *path, const char *tag, const char *var,
                                          char *value, uint32_t size)
{
    oal_file_stru *fp = NULL;

#ifdef INI_TIME_TEST
    oal_timespec_stru tv0;
    oal_timespec_stru tv1;
#endif

    int32_t l_ret;

    if ((var == NULL) || (var[0] == '\0') || (value == NULL) || (size <= 1)) {
        ini_error("check if var and value is NULL or blank");
        return INI_FAILED;
    }
    if ((tag == NULL) || (tag[0] != '[') || (tag[strlen(tag) - 1] != ']')) {
        ini_error("tag invalid");
        return INI_FAILED;
    }

#ifdef INI_TIME_TEST
    oal_do_gettimeofday(&tv0);
#endif

    fp = oal_file_open_readonly(path);
    if (fp == NULL) {
        ini_error("open %s failed!!!", path);
        return INI_FAILED;
    }

    /* find var in .ini return value */
    l_ret = ini_find_var(fp, tag, var, value, size);
    if (l_ret == INI_FAILED) {
        value[0] = '\0';
        oal_file_close(fp);
        return INI_FAILED;
    }

#ifdef INI_TIME_TEST
    oal_do_gettimeofday(&tv1);
    ini_debug("time take = %llu", (tv1.tv_sec - tv0.tv_sec) * MSEC_PER_SEC +
              (oal_get_usec(&tv1) - oal_get_usec(&tv0)) / USEC_PER_MSEC);
#endif

    oal_file_close(fp);

    /* check blank space of value */
    if (ini_str_trim_right(value, strlen(value)) == INI_SUCC) {
        ini_debug("::return %s:%s::", var, value);
        return INI_SUCC;
    }

    return INI_FAILED;
}

/*
 *  Prototype    : ini_find_var_value
 *  Description  : get var value from .ini file
 */
STATIC int32_t ini_find_var_value(const char *tag, const char *var, char *value, uint32_t size)
{
    /* read spec if exist */
    if (ini_file_exist(g_ini_conn_file_name)) {
        if (ini_find_var_value_by_path(g_ini_conn_file_name, tag, var, value, size) == INI_SUCC) {
            return INI_SUCC;
        }
    }

    if (ini_file_exist(g_ini_file_name) == 0) {
        ini_error(" %s not exist!!!", g_ini_file_name);
        chr_exception_report(CHR_PLATFORM_EXCEPTION_EVENTID, CHR_SYSTEM_PLAT, CHR_LAYER_DRV,
                             CHR_PLT_DRV_EVENT_INI, CHR_PLAT_DRV_ERROR_INI_MISS);
        return INI_FAILED;
    }

    return ini_find_var_value_by_path(g_ini_file_name, tag, var, value, size);
}
char *get_host_ini_path(uint32_t chip_id)
{
    return g_ini_file_name;
}

char *get_host_ini_conn_path(uint32_t chip_id)
{
    return g_ini_conn_file_name;
}

#ifdef HISI_NVRAM_SUPPORT
/*
 *  Prototype    : read_conf_from_nvram
 *  Description  : read nv buff from nvram
 *
 */
int32_t read_conf_from_nvram(uint8_t *pc_out, uint32_t size, uint32_t nv_number, const char *nv_name)
{
    struct external_nve_info_user info;
    int32_t ret;

    if (oal_unlikely(oal_any_null_ptr2(pc_out, nv_name))) {
        ini_error("check if var and value is NULL or blank");
        return INI_FAILED;
    }

    memset_s(&info, sizeof(info), 0, sizeof(info));
    memset_s(pc_out, size, 0, size);
    if (strcpy_s(info.nv_name, sizeof(info.nv_name), nv_name) != EOK) {
        ini_error("read nvm failed nv_name size[%lu] less than input[%s]", sizeof(info.nv_name), nv_name);
        oam_error_log1(0, OAM_SF_INI, "read_conf_from_nvram:read nvm failed nv_name size[%lu] less than input",
                       sizeof(info.nv_name));
        return INI_FAILED;
    }
    info.nv_name[strlen(HISI_CUST_NVRAM_NAME)] = '\0';
    info.nv_number = nv_number;
    info.valid_size = HISI_CUST_NVRAM_LEN;
    info.nv_operation = HISI_CUST_NVRAM_READ;

    ret = external_nve_direct_access_interface(&info);
    if (size < sizeof(info.nv_data) || size <= OAL_STRLEN(info.nv_data)) {
        ini_error("read nvm item[%s] fail, lenth[%d] longer than input[%d]",
                  nv_name, (uint32_t)OAL_STRLEN(info.nv_data), size);
        return INI_FAILED;
    }
    if (ret == INI_SUCC) {
        if (memcpy_s(pc_out, size, info.nv_data, sizeof(info.nv_data)) != EOK) {
            ini_error("read nvm{%s}lenth[%d] longer than input[%d]",
                      info.nv_data, (uint32_t)OAL_STRLEN(info.nv_data), size);
            oam_error_log2(0, OAM_SF_INI, "read_conf_from_nvram:read nvm lenth[%d] longer than input[%d]",
                           (uint32_t)OAL_STRLEN(info.nv_data), size);
            return INI_FAILED;
        }
        oal_io_print("read_conf_from_nvram::nvram id[%d] nv name[%s] get data{%s}, size[%d]\n",
                     nv_number, nv_name, info.nv_data, size);
    } else {
        ini_error("read nvm [%d] %s failed", nv_number, nv_name);
        return INI_FAILED;
    }

    return INI_SUCC;
}

/*
 *  Prototype    : write_conf_to_nvram
 *  Description  : change value and write to file
 */
int32_t write_conf_to_nvram(char *name, const char *pc_arr)
{
    struct external_nve_info_user info;
    int32_t ret;

    unref_param(name);

    memset_s(&info, sizeof(info), 0, sizeof(info));
    if (strcpy_s(info.nv_name, sizeof(info.nv_name), HISI_CUST_NVRAM_NAME) != EOK) {
        oam_error_log1(0, OAM_SF_INI,
                       "write_conf_to_nvram:write_conf_to_nvram failed nv_name size[%lu] less than input[WINVRAM]",
                       sizeof(info.nv_name));
        return INI_FAILED;
    }
    info.nv_name[sizeof(info.nv_name) - 1] = '\0';
    info.nv_number = HISI_CUST_NVRAM_NUM;
    info.valid_size = HISI_CUST_NVRAM_LEN;
    info.nv_operation = HISI_CUST_NVRAM_WRITE;
    if (memcpy_s(info.nv_data, sizeof(info.nv_data), pc_arr, HISI_CUST_NVRAM_LEN) != EOK) {
        oam_error_log0(0, OAM_SF_INI, "write_conf_to_nvram:write nvm[memcpy_s] failed");
        return INI_FAILED;
    }

    ret = external_nve_direct_access_interface(&info);
    if (ret < -1) {
        ini_error("write nvm failed");
        return INI_FAILED;
    }

    return INI_SUCC;
}
#else

#ifdef _PRE_SUSPORT_OEMINFO
int32_t read_conf_from_oeminfo(uint8_t *pc_out, uint32_t size, uint32_t offset)
{
    uint8_t eep_buff[MP13_TEL_HUAWEI_NV_WINVRAM_LENGTH + 1] = {0};
    int32_t ret;
    uint32_t i = 0;

    ret = memset_s((void *)pc_out, size, 0, size);
    if (ret != EOK) {
        ini_error("memset_s failed %d", ret);
        return INI_FAILED;
    }

    ret = get_wifi_calibrate_param(MP13_TEL_HUAWEI_NV_WINVRAM_LENGTH, offset, eep_buff);
    if (ret < 0) {
        chr_exception_report(CHR_READ_EEPROM_ERROR_EVENTID, CHR_SYSTEM_WIFI, CHR_LAYER_DRV,
                             CHR_PLT_DRV_EVENT_INIT, CHR_PLAT_DRV_ERROR_EEPROM_READ_INIT);
        ini_error("oeminfo read failed %d", ret);
        return INI_FAILED;
    }

    for (i = 0; i < MP13_TEL_HUAWEI_NV_WINVRAM_LENGTH; i++) {
        if (eep_buff[i] == ';') {
            eep_buff[i + 1] = 0;
            break;
        } else if (eep_buff[i] == 0xff) {
            eep_buff[i] = 0;
            break;
        }
    }
    ret = memcpy_s(pc_out, size, eep_buff, oal_min(size, i));
    if (ret != EOK) {
        ini_error("memcpy_s failed %d", ret);
        return INI_FAILED;
    }
    return INI_SUCC;
}
#endif

#ifdef _PRE_CONFIG_READ_DYNCALI_E2PROM
int32_t read_conf_from_eeprom(uint8_t *pc_out, uint32_t size, uint32_t offset)
{
    uint8_t eep_buff[MP13_TEL_HUAWEI_NV_WINVRAM_LENGTH + 1] = {0};
    int32_t ret;
    uint32_t i = 0;

    ret = memset_s((void *)pc_out, size, 0, size);
    if (ret != EOK) {
        ini_error("memset_s failed %d", ret);
        return INI_FAILED;
    }

#ifdef _PRE_WINDOWS_SUPPORT
    // no eeprom in windows project
#else
    ret = ini_eeprom_read(EEPROM_WIFI_CALI_PARTNAME, offset,  eep_buff,  MP13_TEL_HUAWEI_NV_WINVRAM_LENGTH);
    if (ret < 0) {
        chr_exception_report(CHR_READ_EEPROM_ERROR_EVENTID, CHR_SYSTEM_WIFI, CHR_LAYER_DRV,
                             CHR_PLT_DRV_EVENT_INIT, CHR_PLAT_DRV_ERROR_EEPROM_READ_INIT);
        ini_error("eeprom read failed %d", ret);
        return INI_FAILED;
    }
#endif

    for (i = 0; i < MP13_TEL_HUAWEI_NV_WINVRAM_LENGTH; i++) {
        if (eep_buff[i] == ';') {
            eep_buff[i + 1] = 0;
            break;
        } else if (eep_buff[i] == 0xff) {
            eep_buff[i] = 0;
            break;
        }
    }
    ret = memcpy_s(pc_out, size, eep_buff, oal_min(size, i));
    if (ret != EOK) {
        ini_error("memcpy_s failed %d", ret);
        return INI_FAILED;
    }
    return INI_SUCC;
}
#else
int32_t read_conf_from_nvram(uint8_t *pc_out, uint32_t size, uint32_t nv_number,  const char *nv_name)
{
    char buf[128]; // nv name不超过128字节

    if (oal_unlikely(oal_any_null_ptr2(pc_out, nv_name))) {
        ini_error("check if var and value is NULL or blank");
        return INI_FAILED;
    }

    if (sprintf_s(buf, sizeof(buf), "%s=0", nv_name) < 0) {
        ini_error("sprintf_s failed\n");
        return INI_FAILED;
    }

    if (memcpy_s(pc_out, size, buf, strlen(buf)) != EOK) {
        ini_error("memcpy_s failed\n");
        return INI_FAILED;
    }
    return INI_SUCC;
}
EXPORT_SYMBOL(read_conf_from_nvram);
#endif

int32_t write_conf_to_nvram(char *name, char *pc_arr)
{
    return INI_SUCC;
}

#endif

/*
 *  Prototype    : get_cust_conf_string
 *  Description  : get config form *.ini file or dts(kernel)
 */
int32_t get_cust_conf_string(const char *tag, const char *var, char *value, uint32_t size)
{
    memset_s(value, size, 0, size);
    return ini_find_var_value(tag, var, value, size);
}

/*
 *  Prototype    : get_cust_conf_int32
 *  Description  : get config form *.ini file or dts(kernel)
 */
int32_t get_cust_conf_int32(const char *tag, const char *var, int32_t *value)
{
    int32_t ret;
    char out_str[INI_READ_VALUE_LEN] = {0};

    ret = ini_find_var_value(tag, var, out_str, sizeof(out_str));
    if (ret < 0) {
        /* ini_find_var_value has error log, delete this log */
        ini_debug("cust modu didn't get var of %s.", var);
        return INI_FAILED;
    }

    if (!strncmp(out_str, "0x", strlen("0x")) || !strncmp(out_str, "0X", strlen("0X"))) {
        ini_debug("get hex of:%s.", var);
        ret = sscanf_s(out_str, "%x", value);
    } else {
        ret = sscanf_s(out_str, "%d", value);
    }

    if (ret <= 0) {
        ini_error("%s trans to int failed", var);
        return INI_FAILED;
    }

    ini_debug("conf %s get vale:%d", var, *value);

    return INI_SUCC;
}

/*
 * 函 数 名  : ini_file_get_version
 * 功能描述 : 获取配置文件实际目标的版本信息，文件路径为软链接时跟随软链接。
 * 输入参数 : path    - 配置文件路径
 *            version - 版本信息输出，调用方负责清零初始化
 * 修订日期 : 2026年11月22日（注释及排版整理）
 */
STATIC void ini_file_get_version(const char *path, struct ini_file_version *version)
{
    oal_file_stru *fp = oal_file_open_readonly(path);
#if !defined(_PRE_WINDOWS_SUPPORT) && !defined(_PRE_LINUX_TEST)
    struct inode *inode;
#endif

    if (fp == NULL) {
        return;
    }

    version->present = true;
#if !defined(_PRE_WINDOWS_SUPPORT) && !defined(_PRE_LINUX_TEST)
    inode = file_inode(fp);
    version->ino = inode->i_ino;
    version->dev = inode->i_sb->s_dev;
    version->ctime_sec = inode->i_ctime.tv_sec;
    version->ctime_nsec = inode->i_ctime.tv_nsec;
#endif
    oal_file_close(fp);
}

#ifndef _PRE_LINUX_TEST
/*
 * 函 数 名  : ini_file_version_equal
 * 功能描述 : 比较文件存在状态、设备号、inode 及完整 ctime 是否一致。
 * 返回值   : true - 文件版本一致；false - 文件版本不同
 * 修订日期 : 2026年11月22日（注释及排版整理）
 */
STATIC bool ini_file_version_equal(const struct ini_file_version *a, const struct ini_file_version *b)
{
    return a->present == b->present && a->ino == b->ino &&
           a->dev == b->dev && a->ctime_sec == b->ctime_sec &&
           a->ctime_nsec == b->ctime_nsec;
}
#endif

/*
 * 函 数 名  : ini_file_prepare_conf_update
 * 功能描述 : 检测主 INI 和覆盖 INI 的文件版本，判断是否需要重新加载配置。
 *            本函数只准备版本快照，不更新已加载版本缓存。
 * 输入参数 : type    - 配置子系统类型
 *            version - 当前文件版本输出
 * 返回值   : INI_FILE_TIMESPEC_RECONFIG   - 文件版本发生变化
 *            INI_FILE_TIMESPEC_UNRECONFIG - 文件版本未变化
 *            INI_FAILED                   - 类型无效或配置文件不可用
 * 修订日期 : 2026年11月22日（注释及排版整理）
 */
int32_t ini_file_prepare_conf_update(ini_section_type_enum type, struct ini_conf_version *version)
{
    if (oal_unlikely(type >= INI_SECTION_BUTT)) {
        ini_error("error type, type = %u\n", type);
        return INI_FAILED;
    }

    *version = (struct ini_conf_version){0};
    ini_file_get_version(g_ini_file_name, &version->primary);
    if (g_ini_conn_file_name[0] != '\0') {
        ini_file_get_version(g_ini_conn_file_name, &version->override);
    }
    if (!version->primary.present && !version->override.present) {
        ini_error("no INI file can be opened\n");
        return INI_FAILED;
    }

#ifndef _PRE_LINUX_TEST
    if (ini_file_version_equal(&version->primary, &g_ini_conf_version[type].primary) &&
        ini_file_version_equal(&version->override, &g_ini_conf_version[type].override)) {
        ini_info("no ini file is updated\n");
        return INI_FILE_TIMESPEC_UNRECONFIG;
    }
#endif
    ini_info("INI file version changed\n");
    return INI_FILE_TIMESPEC_RECONFIG;
}

/*
 * 函 数 名  : ini_file_commit_conf_update
 * 功能描述 : 配置初始化函数返回成功后，提交当前文件版本为已加载版本。
 * 输入参数 : type    - 配置子系统类型
 *            version - 待提交的文件版本快照
 * 修订日期 : 2026年11月22日（注释及排版整理）
 */
void ini_file_commit_conf_update(ini_section_type_enum type, const struct ini_conf_version *version)
{
    if (type < INI_SECTION_BUTT) {
        g_ini_conf_version[type] = *version;
    }
}

// HI1105_INI_FIX_BEGIN [2026-11-22] 加载失败后清除已加载版本，确保再次读取INI
void ini_file_invalidate_conf_update(ini_section_type_enum type)
{
    if (type < INI_SECTION_BUTT)
    {
        g_ini_conf_version[type] = (struct ini_conf_version){0};
    }
}
// HI1105_INI_FIX_END [2026-11-22]

/*
 * 函 数 名  : ini_file_check_conf_update
 * 功能描述 : 保留原厂平台接口，通过版本检查后立即提交缓存以兼容旧调用方。
 *            MP13 Wi-Fi 主路径使用 prepare/commit 分离接口。
 * 输入参数 : type - 配置子系统类型
 * 返回值   : INI_FILE_TIMESPEC_RECONFIG   - 文件版本发生变化
 *            INI_FILE_TIMESPEC_UNRECONFIG - 文件未变化或检查失败（兼容旧接口）
 * 修订日期 : 2026年11月22日（注释及排版整理）
 */
int32_t ini_file_check_conf_update(ini_section_type_enum type)
{
    struct ini_conf_version version;

    if (ini_file_prepare_conf_update(type, &version) != INI_FILE_TIMESPEC_RECONFIG) {
        return INI_FILE_TIMESPEC_UNRECONFIG;
    }
    ini_file_commit_conf_update(type, &version);
    return INI_FILE_TIMESPEC_RECONFIG;
}

char *get_ini_file_name(void)
{
    return g_ini_file_name;
}

#ifdef _PRE_WLAN_EXPORT
static int ini_cfg_ini_from_default_path(void)
{
    if (ini_file_exist(g_ini_file_name)) {
        g_st_board_info.ini_file_name = g_ini_file_name;

        if (snprintf_s(g_ini_conn_file_name, sizeof(g_ini_conn_file_name),
                    sizeof(g_ini_conn_file_name) - 1, "%s", CUST_PATH_INI_CONN) < 0) {
            ini_error("g_ini_conn_file_name space is not enough\n");
        }
        oal_io_print("ini_file_name@%s\n", g_ini_file_name);
        return INI_SUCC;
    }

    ini_warning("Can't find ini_file from default path %s!!\n", g_ini_file_name);

    if ((!ini_file_exist(g_ini_file_name)) && (!ini_file_exist(g_ini_conn_file_name))) {
        ini_error("Cannot find ini_file:[%s] or [%s]!!! Please Check!!!\n", g_ini_file_name, g_ini_conn_file_name);
    }

    return INI_FAILED;
}
#endif

#if defined(_PRE_PRODUCT_HI1620S_KUNPENG) || defined(_PRE_WINDOWS_SUPPORT)
int ini_cfg_init(void)
{
    ini_info("ini config search init!\n");

    if (snprintf_s(g_ini_file_name, sizeof(g_ini_file_name),
                   sizeof(g_ini_file_name) - 1, "%s", INI_PATH_INI_CONN) < 0) {
        ini_error("ini_file_name space is not enough\n");
        return INI_FAILED
    }
    oal_io_print("ini_file_name@ %s\n", g_ini_file_name);
    g_st_board_info.ini_file_name = g_ini_file_name;

    return INI_SUCC;
}
#else
int ini_cfg_init(void)
{
#ifdef _PRE_CONFIG_USE_DTS
    int32_t ret;
    char auc_dts_ini_path[INI_FILE_PATH_LEN] = {0};
#endif

    ini_info("ini config search init!\n");

#ifdef _PRE_WLAN_EXPORT
    if (ini_cfg_ini_from_default_path() == INI_SUCC) {
        return INI_SUCC;
    }
#endif

#ifdef _PRE_CONFIG_USE_DTS
    ret = mpxx_get_ini_file_name_from_dts(PROC_NAME_INI_FILE_NAME, auc_dts_ini_path, sizeof(auc_dts_ini_path));
    if (ret < 0) {
        ini_error("can't find dts proc %s\n", PROC_NAME_INI_FILE_NAME);
        return INI_FAILED;
    }
    if (snprintf_s(g_ini_file_name, sizeof(g_ini_file_name),
                   sizeof(g_ini_file_name) - 1, "%s", auc_dts_ini_path) < 0) {
        ini_error("space is not enough\n");
        return INI_FAILED;
    }
#endif

    if (!ini_file_exist(g_ini_file_name)) {
        ini_error("ini_file:%s is not exist!\n", g_ini_file_name);
        return INI_FAILED;
    }
    g_st_board_info.ini_file_name = g_ini_file_name;

    /* Note:"symbol snprintf()"has arg.count conflict(5 vs 4) */
    if (snprintf_s(g_ini_conn_file_name, sizeof(g_ini_conn_file_name),
                   sizeof(g_ini_conn_file_name) - 1, "%s", CUST_PATH_INI_CONN) < 0) {
        ini_error("space is not enough\n");
        return INI_FAILED;
    }
    oal_io_print("ini_file_name@%s\n", g_ini_file_name);

    ini_info("%s@%s\n", PROC_NAME_INI_FILE_NAME, g_ini_file_name);

    return INI_SUCC;
}
#endif

void ini_cfg_exit(void)
{
    ini_info("ini config search exit!\n");
}

void register_wifi_customize(fun_wifi_customize_t fun_wifi_customize)
{
    g_fun_wifi_customize = fun_wifi_customize;
}

void wifi_customize_h2d(void)
{
    if (g_fun_wifi_customize != NULL) {
        g_fun_wifi_customize();
    } else {
        ini_error("wifi_customize not register\n");
    }
}

void register_gt_customize(fun_wifi_customize_t fun_gt_customize)
{
    g_fun_gt_customize = fun_gt_customize;
}

void gt_customize_h2d(void)
{
    if (g_fun_gt_customize != NULL) {
        g_fun_gt_customize();
    } else {
        ini_error("gt_customize not register\n");
    }
}

oal_module_symbol(ini_file_check_conf_update);
oal_module_symbol(ini_file_prepare_conf_update);
oal_module_symbol(ini_file_commit_conf_update);
// HI1105_INI_FIX_BEGIN [2026-11-22] 导出配置版本失效接口
oal_module_symbol(ini_file_invalidate_conf_update);
// HI1105_INI_FIX_END [2026-11-22]
oal_module_symbol(get_cust_conf_string);
oal_module_symbol(get_cust_conf_int32);
#ifdef HISI_NVRAM_SUPPORT
oal_module_symbol(read_conf_from_nvram);
#endif
oal_module_symbol(register_wifi_customize);
oal_module_symbol(register_gt_customize);
oal_module_symbol(get_host_ini_conn_path);
oal_module_symbol(get_host_ini_path);
oal_module_symbol(ko_read_line);
oal_module_symbol(ini_file_exist);

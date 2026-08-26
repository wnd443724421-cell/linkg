/*
 * Copyright (c) @CompanyNameMagicTag 2021-2023. All rights reserved.
 * 功能说明 : NETDEV OPS 命令相关
 * 作    者 : wifi3
 * 创建日期 : 2020年7月14日
 */


#include "wal_linux_vendor.h"
#include "wal_linux_ioctl.h"
#include "wal_linux_netdev_ops.h"
#include "oal_ext_if.h"
#include "frw_ext_if.h"
#include "hmac_tx_data.h"
#include "mac_ftm.h"
#include "wlan_types.h"
#include "mac_vap.h"
#include "mac_user.h"
#include "mac_resource.h"
#include "mac_regdomain.h"
#include "mac_ie.h"
#include "mac_device.h"
#include "hmac_ext_if.h"
#include "hmac_chan_mgmt.h"
#include "hmac_config.h"
#include "wal_main.h"
#include "wal_ext_if.h"
#include "wal_config.h"
#include "hmac_resource.h"
#include "hmac_p2p.h"
#include "hmac_stat.h"
#include "wal_linux_atcmdsrv.h"
#include "wal_linux_cfg80211.h"
#include "wal_linux_cfgvendor.h"
#include "wal_dfx.h"
#include "wal_radio_status_ioctl.h"
#include "wal_tx_flowctrl_ioctl.h"

#if (_PRE_OS_VERSION_LINUX == _PRE_OS_VERSION)
#include <linux/notifier.h>
#include <linux/inetdevice.h>
#include <linux/mutex.h>
#include <net/addrconf.h>
#endif

#ifdef _PRE_PLAT_FEATURE_CUSTOMIZE
#include "oal_net.h"

#endif /* #ifdef _PRE_PLAT_FEATURE_CUSTOMIZE */

#if (_PRE_OS_VERSION_LINUX == _PRE_OS_VERSION)
#include "plat_pm_wlan.h"
#include "plat_firmware.h"
#endif
#ifdef _PRE_WLAN_FEATURE_SNIFFER
#ifdef CONFIG_HW_SNIFFER
#include <hwnet/ipv4/sysctl_sniffer.h>
#endif
#endif
#ifdef _PRE_WLAN_FEATURE_VSP
#include "hmac_vsp_if.h"
#endif
#include "hmac_dfx.h"

#ifdef _PRE_WLAN_EXPORT
#include "wal_linux_ioctl_debug.h"
#endif

#include "hmac_ht_self_cure.h"

#undef THIS_FILE_ID
#define THIS_FILE_ID OAM_FILE_ID_WAL_LINUX_NETDEV_OPS_C

#ifdef _PRE_WLAN_FEATURE_SNIFFER
#define CONTINUOUS_DISABLE_ENABLE_WIFI_MAX_NUM 5
static int32_t g_continuous_disable_enable_wifi = 0; // sniffer保护计数:如果sniffer异常退出状态位未清空，开启wifi次数限制
#endif

#ifdef _PRE_WLAN_EXPORT
uint8_t g_hitalk_set = 0;
#endif

#ifdef _PRE_WLAN_FEATURE_SNIFFER
void wal_clear_continuous_disable_enable_wifi_cnt(void)
{
    g_continuous_disable_enable_wifi = 0;
}
#endif

/*
 * 函 数 名  : wal_dfr_recovery_check
 * 功能描述  : 检查是否需要进行dfr异常恢复操作
 * 1.日    期  : 2016年7月8日
 *   作    者  : wifi
 *   修改内容  : 新生成函数
 */
#ifdef _PRE_WLAN_FEATURE_DFR
OAL_STATIC oal_bool_enum_uint8 wal_dfr_recovery_check(oal_net_device_stru *pst_net_dev)
{
    oal_wireless_dev_stru *pst_wdev;
    mac_wiphy_priv_stru *pst_wiphy_priv;
    mac_device_stru *pst_mac_device;

    if (pst_net_dev == NULL) {
        oam_warning_log0(0, OAM_SF_ANY, "{wal_dfr_recovery_check::pst_net_dev is null!}");
        return OAL_FALSE;
    }

    pst_wdev = oal_netdevice_wdev(pst_net_dev);
    if (pst_wdev == NULL) {
        oam_warning_log0(0, OAM_SF_ANY, "{wal_dfr_recovery_check::pst_wdev is null!}");
        return OAL_FALSE;
    }

    pst_wiphy_priv = (mac_wiphy_priv_stru *)oal_wiphy_priv(pst_wdev->wiphy);
    if (pst_wiphy_priv == NULL) {
        oam_warning_log0(0, OAM_SF_ANY, "{wal_dfr_recovery_check::pst_wiphy_priv is null!}");
        return OAL_FALSE;
    }

    pst_mac_device = pst_wiphy_priv->pst_mac_device;
    if (pst_mac_device == NULL) {
        oam_warning_log0(0, OAM_SF_ANY, "{wal_dfr_recovery_check::pst_mac_device is null!}");
        return OAL_FALSE;
    }

    oam_warning_log2(0, OAM_SF_ANY, "{wal_dfr_recovery_check::recovery_flag:%d, uc_vap_num:%d.}",
                     g_st_dfr_info.bit_ready_to_recovery_flag, pst_mac_device->uc_vap_num);

    if ((g_st_dfr_info.bit_ready_to_recovery_flag == OAL_TRUE)
        && (!pst_mac_device->uc_vap_num)) {
        /* DFR恢复,在创建业务VAP前下发校准等参数,只下发一次 */
        return OAL_TRUE;
    }

    return OAL_FALSE;
}
#endif /* #ifdef _PRE_WLAN_FEATURE_DFR */

/*
* 函 数 名  : wal_netdev_open_flush_p2p_random_mac
* 功能描述  : netdev open启动vap流程和p2p随机mac更新（降圈复杂度和行数)
* 修改历史      :
*  1.日    期   : 2019年8月30日
*    修改内容   : 新生成函数
*/
OAL_STATIC void wal_netdev_open_flush_p2p_random_mac(oal_net_device_stru *pst_net_dev)
{
#ifdef _PRE_PLAT_FEATURE_CUSTOMIZE
    /* p2p interface mac地址更新只发生在add virtual流程中,
    不能保证上层下发的随机mac更新到mib,此处刷新mac addr */
    if ((oal_strncmp("p2p-p2p0", pst_net_dev->name, OAL_STRLEN("p2p-p2p0")) == 0) ||
         (oal_strncmp("p2p-wlan0", pst_net_dev->name, OAL_STRLEN("p2p-wlan0")) == 0)) {
        wal_set_random_mac_to_mib(pst_net_dev);
    }
#endif
}
OAL_STATIC int32_t wal_netdev_set_power_on_flag(oal_net_device_stru *net_dev, int32_t pm_ret)
{
    int32_t l_ret;

    if (pm_ret != OAL_ERR_CODE_ALREADY_OPEN) {
#ifdef _PRE_PLAT_FEATURE_CUSTOMIZE
        /* 重新上电时置为FALSE */
        hwifi_config_init_force();
#endif

        /* 上电host device_stru初始化 */
        l_ret = wal_host_dev_init(net_dev);
        if (l_ret != OAL_SUCC) {
            oal_io_print("wal_host_dev_init FAIL %d \r\n", l_ret);
            return -OAL_EFAIL;
        }
    } else {
#ifdef _PRE_WLAN_FEATURE_DFR
    /* dfr 暂未适配wlan1 */
        if (wal_dfr_recovery_check(net_dev)) {
#ifdef _PRE_PLAT_FEATURE_CUSTOMIZE
            g_custom_cali_done = OAL_TRUE;
            wal_dfr_custom_cali();
            hwifi_config_init_force();
#endif
        }
#endif /* #ifdef _PRE_WLAN_FEATURE_DFR */
    }

    return OAL_SUCC;
}
OAL_STATIC int32_t wal_netdev_msg_process(oal_net_device_stru *net_dev)
{
    int32_t l_ret;
    uint32_t err_code;
    wal_msg_write_stru st_write_msg;
    wlan_p2p_mode_enum_uint8 en_p2p_mode;
    wal_msg_stru *pst_rsp_msg = NULL;
    oal_wireless_dev_stru *pst_wdev = NULL;

    if (oal_unlikely(net_dev == NULL)) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_msg_process::net_dev null!}");
        return -OAL_EFAUL;
    }
    /* 抛事件到wal层处理 填写消息 */
    wal_write_msg_hdr_init(&st_write_msg, WLAN_CFGID_START_VAP, sizeof(mac_cfg_start_vap_param_stru));
    ((mac_cfg_start_vap_param_stru *)st_write_msg.auc_value)->pst_net_dev = net_dev;
    pst_wdev = net_dev->ieee80211_ptr;
    en_p2p_mode = wal_wireless_iftype_to_mac_p2p_mode(pst_wdev->iftype);
    if (WLAN_P2P_BUTT == en_p2p_mode) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_open::wal_wireless_iftype_to_mac_p2p_mode return BUFF}");
        chr_exception_report(CHR_PLATFORM_EXCEPTION_EVENTID, CHR_SYSTEM_WIFI,
            CHR_LAYER_DRV, CHR_WIFI_DRV_EVENT_OPEN, CHR_WIFI_DRV_ERROR_POWER_ON);
        return -OAL_EINVAL;
    }
    ((mac_cfg_start_vap_param_stru *)st_write_msg.auc_value)->en_p2p_mode = en_p2p_mode;
    ((mac_cfg_start_vap_param_stru *)st_write_msg.auc_value)->en_mgmt_rate_init_flag = OAL_TRUE;

    ((mac_cfg_start_vap_param_stru *)st_write_msg.auc_value)->l_ifindex = net_dev->ifindex;

    /* 发送消息 */
    l_ret = wal_send_cfg_event(net_dev, WAL_MSG_TYPE_WRITE,
        WAL_MSG_WRITE_MSG_HDR_LENGTH + sizeof(mac_cfg_start_vap_param_stru),
        (uint8_t *)&st_write_msg, OAL_TRUE, &pst_rsp_msg);
    if (oal_unlikely(l_ret != OAL_SUCC)) {
        oam_warning_log1(0, OAM_SF_ANY, "{wal_netdev_open::wal_alloc_cfg_event err %d!}", l_ret);
        return l_ret;
    }

    /* 处理返回消息 */
    err_code = wal_check_and_release_msg_resp(pst_rsp_msg);
    if (err_code != OAL_SUCC) {
        oam_warning_log1(0, OAM_SF_ANY, "{wal_netdev_open::hmac start vap fail,err code[%u]!}", err_code);
        return -OAL_EINVAL;
    }

    return OAL_SUCC;
}

void wal_set_napi_enable(oal_net_device_stru *pst_net_dev)
{
    oal_netdev_priv_stru *pst_netdev_priv = NULL;

    if (oal_unlikely(pst_net_dev == NULL || pst_net_dev->ieee80211_ptr == NULL)) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_set_napi_enable::net_dev or ieee80211_ptr null!}");
        return;
    }

    pst_netdev_priv = (oal_netdev_priv_stru *)oal_net_dev_wireless_priv(pst_net_dev);
    if (pst_netdev_priv->uc_napi_enable && (!pst_netdev_priv->uc_state) &&
        (oal_value_eq_any3(pst_net_dev->ieee80211_ptr->iftype,
            NL80211_IFTYPE_STATION, NL80211_IFTYPE_P2P_DEVICE, NL80211_IFTYPE_P2P_CLIENT))) {
        oal_napi_enable(&pst_netdev_priv->st_napi);
        pst_netdev_priv->uc_state = 1;
    }
}

void wal_netdev_open_get_hw_status(oal_net_device_stru *pst_net_dev)
{
    int32_t fem_pa_status = 0;
    int32_t lna_gain_status = 0;

    if (oal_unlikely(pst_net_dev == NULL || pst_net_dev->ieee80211_ptr == NULL)) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_open_get_hw_status::net_dev or ieee80211_ptr null!}");
        return;
    }

    /* mp12硬件fem、DEVICE唤醒HOST虚焊lna烧毁检测,只在wlan0时打印 */
    if ((pst_net_dev->ieee80211_ptr->iftype == NL80211_IFTYPE_STATION) &&
        (0 == (oal_strcmp("wlan0", pst_net_dev->name)))) {
        wal_atcmsrv_ioctl_get_fem_pa_status(pst_net_dev, &fem_pa_status);
        wal_atcmsrv_ioctl_get_lna_gain_status(pst_net_dev, &lna_gain_status);
    }
}

void wal_netdev_set_feature_customize(oal_net_device_stru *pst_net_dev)
{
#ifdef _PRE_PLAT_FEATURE_CUSTOMIZE

    wal_netdev_open_flush_p2p_random_mac(pst_net_dev);
#endif /* #ifdef _PRE_PLAT_FEATURE_CUSTOMIZE */
}

/*
 * 函 数 名  : wal_netdev_open
 * 功能描述  : 启用VAP
 * 1.日    期  : 2012年12月11日
  *   修改内容  : 新生成函数
 */
OAL_STATIC int32_t _wal_netdev_open(oal_net_device_stru *pst_net_dev, uint8_t uc_entry_flag, int32_t pm_ret)
{
    int32_t l_ret;

    if (oal_unlikely(pst_net_dev == NULL || pst_net_dev->ieee80211_ptr == NULL)) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_open::net_dev or ieee80211_ptr null!}");
        return -OAL_EFAUL;
    }

    oal_io_print("wal_netdev_open,dev_name is:%s\n", pst_net_dev->name);
    oam_warning_log1(0, OAM_SF_ANY, "{wal_netdev_open::iftype:%d.!}", pst_net_dev->ieee80211_ptr->iftype);

    if (wal_netdev_set_power_on_flag(pst_net_dev, pm_ret) != OAL_SUCC) {
        oam_warning_log0(0, OAM_SF_ANY, "{_wal_netdev_open::wal_netdev_set_power_on_flag fail}");
        return -OAL_EFAIL;
    }

    /* 配置vap的创建,函数内通过标志位判断是否需要下发device */
    l_ret = wal_cfg_vap_h2d_event(pst_net_dev);
    if (l_ret != OAL_SUCC) {
        oal_io_print("wal_cfg_vap_h2d_event FAIL %d \r\n", l_ret);
        return -OAL_EFAIL;
    }
    oal_io_print("wal_cfg_vap_h2d_event succ \r\n");

#ifdef _PRE_WLAN_CHBA_MGMT
    if ((g_optimized_feature_switch_bitmap & BIT(CUSTOM_OPTIMIZE_CHBA)) &&
        (oal_strcmp("chba0", pst_net_dev->name) == 0)) {
        oam_warning_log0(0, OAM_SF_ANY, "{wal_netdev_open::wal_setup_chba.!}");
        return wal_setup_chba(pst_net_dev);
    }
#endif

    /* 信道跟随--取消name判断 */
    if (oal_value_eq_any2(pst_net_dev->ieee80211_ptr->iftype, NL80211_IFTYPE_STATION, NL80211_IFTYPE_P2P_DEVICE)) {
        if (wal_init_wlan_vap(pst_net_dev) != OAL_SUCC) {
            chr_exception_report(CHR_PLATFORM_EXCEPTION_EVENTID, CHR_SYSTEM_WIFI,
                CHR_LAYER_DRV, CHR_WIFI_DRV_EVENT_OPEN, CHR_WIFI_DRV_ERROR_POWER_ON);
            return -OAL_EFAIL;
        }
    } else if (NL80211_IFTYPE_AP == pst_net_dev->ieee80211_ptr->iftype) {
        oam_warning_log0(0, OAM_SF_ANY, "{wal_netdev_open::ap mode,no need to start vap.!}");
        oal_net_tx_wake_all_queues(pst_net_dev); /* 启动发送队列 */
        return OAL_SUCC;
    }

    wal_netdev_set_feature_customize(pst_net_dev);

    /* 抛事件到wal层处理 填写消息 */
    if (wal_netdev_msg_process(pst_net_dev) != OAL_SUCC) {
        oam_warning_log0(0, OAM_SF_ANY, "{_wal_netdev_open::wal_netdev_msg_process fail}");
        return -OAL_EFAIL;
    }

    oal_netdevice_flags(pst_net_dev) |= OAL_IFF_RUNNING;

    wal_set_napi_enable(pst_net_dev);

    oal_net_tx_wake_all_queues(pst_net_dev); /* 启动发送队列 */

    wal_netdev_open_get_hw_status(pst_net_dev);

    return OAL_SUCC;
}

int32_t wal_netdev_open_ext(oal_net_device_stru *pst_net_dev)
{
#ifdef  _PRE_CONFIG_HISI_S3S4_POWER_STATE
    if (g_st_recovery_info.device_s3s4_process_flag) {
        oam_warning_log0(0, OAM_SF_CFG, "{wal_netdev_open_ext::s3s4 is processing!}\r\n");
        return -OAL_EFAIL;
    }
#endif
    if (pst_net_dev == NULL) {
        oam_error_log0(0, OAM_SF_CFG, "{wal_netdev_open_ext::netdev open fail!}\r\n");
        return -OAL_EFAIL;
    }
    oam_warning_log0(0, OAM_SF_CFG, "{wal_netdev_open_ext::netdev open!}\r\n");
    return wal_netdev_open(pst_net_dev, OAL_TRUE);
}

#ifdef _PRE_WLAN_FEATURE_SNIFFER
#ifdef CONFIG_HW_SNIFFER
void wal_init_sniffer_cmd_function(void)
{
    static int is_func_set = 0;
    if (!is_func_set) {
        is_func_set = 1;
        proc_set_hipriv_func(wal_hipriv_parse_cmd);
    }
}
#endif
#endif

void wal_netdev_open_en_monitor_limit(oal_net_device_stru *net_dev)
{
    mac_vap_stru *mac_vap = NULL;
    hmac_device_stru *hmac_device = NULL;

    mac_vap = oal_net_dev_priv(net_dev);
    if (mac_vap == NULL) {
        return;
    }
    hmac_device = hmac_res_get_mac_dev(mac_vap->uc_device_id);
    if (hmac_device == NULL) {
        return;
    }
#ifdef _PRE_WLAN_FEATURE_SNIFFER
#ifdef CONFIG_HW_SNIFFER
    // netdev_open配置sniffer下发指令
    wal_init_sniffer_cmd_function();
#endif
#endif

    g_continuous_disable_enable_wifi++;
    if ((hmac_device->en_monitor_mode == OAL_TRUE) &&
        (g_continuous_disable_enable_wifi >= CONTINUOUS_DISABLE_ENABLE_WIFI_MAX_NUM)) {
        oam_warning_log1(0, OAM_SF_ANY, "{wal_netdev_open_en_monitor_limit:: g_continuous_disable_enable_wifi=%d}\n",
                         g_continuous_disable_enable_wifi);
        hmac_device->en_monitor_mode = OAL_FALSE;
        g_continuous_disable_enable_wifi = 0;
    }
}

/* if TV && built-in, open wlan0 is early and WAL_WAIT_CALI_COMPLETE_TIME should be longer */
#if (defined _PRE_SUPPORT_LONG_CALI_COMPLETE_TIME) && (!defined CONFIG_HI110X_KERNEL_MODULES_BUILD_SUPPORT)
#define WAL_WAIT_CALI_COMPLETE_TIME  15000
#else
#define WAL_WAIT_CALI_COMPLETE_TIME  6000
#endif
/*
 * 函 数 名  : wal_wait_host_rf_cali_init_complete
 * 功能描述  : 等待开机上下电校准流程完成，避免在开机校准未完成时上层应用打开wifi
 */
int32_t wal_wait_host_rf_cali_init_complete(void)
{
    int32_t ret;

    /* 等待开机校准流程完成 */
    ret = oal_wait_event_timeout_m(g_wlan_cali_complete_wq, hmac_get_wlan_first_power_on_cali_flag() == OAL_FALSE,
        (uint32_t)oal_msecs_to_jiffies(WAL_WAIT_CALI_COMPLETE_TIME));
    if (ret == 0) {
        oam_error_log1(0, OAM_SF_ANY, "{wal_netdev_open::wait first powon rf cali complete [%d]ms timeout!}",
            WAL_WAIT_CALI_COMPLETE_TIME);
    }
    return ret;
}

#ifdef _PRE_WLAN_EXPORT
void wal_netdev_open_export(oal_net_device_stru *pst_net_dev)
{
    if (g_hitalk_set) {
        wal_hipriv_narrow_bw(pst_net_dev, "1 5m");
    }
}
#endif

int32_t wal_netdev_open(oal_net_device_stru *pst_net_dev, uint8_t uc_entry_flag)
{
    int32_t ret, pm_ret;

    if (oal_netdevice_flags(pst_net_dev) & OAL_IFF_RUNNING) {
        return OAL_SUCC;
    }
    if (wal_wait_host_rf_cali_init_complete() == 0) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_open::wlan first powon not done!}");
        return -OAL_EFAIL;
    }
#if (_PRE_OS_VERSION_LINUX == _PRE_OS_VERSION)
    g_netdev_is_open = OAL_TRUE;
#endif
    pm_ret = wlan_pm_open();
    if (pm_ret == OAL_FAIL) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_open::wlan_pm_open Fail!}");
        chr_exception_report(CHR_PLATFORM_EXCEPTION_EVENTID, CHR_SYSTEM_WIFI,
            CHR_LAYER_DRV, CHR_WIFI_DRV_EVENT_OPEN, CHR_WIFI_DRV_ERROR_POWER_ON);
        return -OAL_EFAIL;
    }
#ifdef _PRE_WLAN_FEATURE_DFR
#if (_PRE_OS_VERSION_LINUX == _PRE_OS_VERSION)
    mutex_lock(&g_st_dfr_info.wifi_excp_mutex);
#endif
#endif

    wal_wake_lock();
    ret = _wal_netdev_open(pst_net_dev, uc_entry_flag, pm_ret);
    wal_wake_unlock();

    wal_netdev_open_en_monitor_limit(pst_net_dev);

#ifdef _PRE_WLAN_FEATURE_DFR
#if (_PRE_OS_VERSION_LINUX == _PRE_OS_VERSION)
    mutex_unlock(&g_st_dfr_info.wifi_excp_mutex);
#endif
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)) && \
    (_PRE_OS_VERSION_LINUX == _PRE_OS_VERSION)
    /* 记录wlan0 开的时间 */
    if (oal_strncmp("wlan0", pst_net_dev->name, OAL_STRLEN("wlan0")) == 0) {
        g_st_wifi_radio_stat.ull_wifi_on_time_stamp = OAL_TIME_JIFFY;
    }
#endif
#ifdef HISI_CONN_NVE_SUPPORT
    wlan_chip_get_nv_dpd_data();
#endif
    wlan_chip_send_20m_all_chn_cali_data(pst_net_dev);
#ifdef _PRE_WLAN_EXPORT
    wal_netdev_open_export(pst_net_dev);
#endif

    return ret;
}
/*
 * 函 数 名  : wal_netdev_get_stats
 * 功能描述  : 获取统计信息
 * 1.日    期  : 2013年6月1日
  *   修改内容  : 新生成函数
 */
oal_net_device_stats_stru *wal_netdev_get_stats(oal_net_device_stru *pst_net_dev)
{
    oal_net_device_stats_stru *pst_stats = NULL;
    mac_vap_stru *mac_vap = NULL;
    hmac_vap_stru *hmac_vap = NULL;
    hmac_vap_stat_stru *hmac_vap_stats = NULL;

    if (pst_net_dev == NULL) {
        return NULL;
    }
    pst_stats = &(pst_net_dev->stats);
    mac_vap = oal_net_dev_priv(pst_net_dev);
    if (mac_vap == NULL) {
        return pst_stats;
    }

    hmac_vap = mac_res_get_hmac_vap(mac_vap->uc_vap_id);
    if (hmac_vap == NULL) {
        return pst_stats;
    }

    hmac_vap_stats = hmac_stat_get_vap_stat(hmac_vap);

    /* 更新统计信息到net_device */
    pst_stats->rx_packets = hmac_vap_stats->rx_packets;
    pst_stats->rx_bytes = hmac_vap_stats->rx_bytes;

    pst_stats->tx_packets = hmac_vap_stats->tx_packets;
    pst_stats->tx_bytes = hmac_vap_stats->tx_bytes;
    return pst_stats;
}

/*
 * 函 数 名  : wal_get_macdev_by_netdev
 * 功能描述  : 通过device id设置netdev的名称device id 0 ->wlan0 p2p0, device id 1->wlan1 p2p1
 * 1.日    期  : 2016年07月23日
  *   修改内容  : 新生成函数
 */
mac_device_stru *wal_get_macdev_by_netdev(oal_net_device_stru *pst_net_dev)
{
    oal_wireless_dev_stru *pst_wdev;
    mac_wiphy_priv_stru *pst_wiphy_priv = NULL;

    pst_wdev = oal_netdevice_wdev(pst_net_dev);
    if (pst_wdev == NULL) {
        oam_warning_log0(0, OAM_SF_ANY, "{wal_get_macdev_by_netdev::pst_wdev is null!}");
        return NULL;
    }
    pst_wiphy_priv = (mac_wiphy_priv_stru *)(oal_wiphy_priv(pst_wdev->wiphy));
    return pst_wiphy_priv->pst_mac_device;
}
OAL_STATIC int32_t wal_netdev_stop_msg_process(oal_net_device_stru *net_dev)
{
    int32_t l_ret;
    uint32_t err_code;
    wal_msg_write_stru st_write_msg;
    oal_wireless_dev_stru *pst_wdev = NULL;
    wal_msg_stru *pst_rsp_msg = NULL;
    wlan_p2p_mode_enum_uint8 en_p2p_mode;

    if (oal_unlikely(net_dev == NULL || net_dev->ieee80211_ptr == NULL)) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_stop_msg_process::net_dev or ieee80211_ptr null!}");
        return -OAL_EFAUL;
    }

    /* 抛事件到wal层处理 */
    wal_write_msg_hdr_init(&st_write_msg, WLAN_CFGID_DOWN_VAP, sizeof(mac_cfg_down_vap_param_stru));
    ((mac_cfg_down_vap_param_stru *)st_write_msg.auc_value)->pst_net_dev = net_dev;
    pst_wdev = net_dev->ieee80211_ptr;
    en_p2p_mode = wal_wireless_iftype_to_mac_p2p_mode(pst_wdev->iftype);
    if (WLAN_P2P_BUTT == en_p2p_mode) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_stop::wal_wireless_iftype_to_mac_p2p_mode return BUFF}");
        return -OAL_EINVAL;
    }
    ((mac_cfg_start_vap_param_stru *)st_write_msg.auc_value)->en_p2p_mode = en_p2p_mode;

    /* 发送消息 */
    l_ret = wal_send_cfg_event(net_dev, WAL_MSG_TYPE_WRITE,
        WAL_MSG_WRITE_MSG_HDR_LENGTH + sizeof(mac_cfg_down_vap_param_stru),
        (uint8_t *)&st_write_msg, OAL_TRUE, &pst_rsp_msg);
    if (oal_unlikely(l_ret != OAL_SUCC)) {
        if (oal_net_dev_priv(net_dev) == NULL) {
            /* 关闭net_device，发现其对应vap 是null，清除flags running标志 */
            oal_netdevice_flags(net_dev) &= (~OAL_IFF_RUNNING);
            oam_warning_log1(0, OAM_SF_ANY,
                "{wal_netdev_stop::netdev's vap null, set flag not running, if_idx:%d}", net_dev->ifindex);
        }
        oam_warning_log1(0, OAM_SF_ANY, "{wal_netdev_stop::wal_alloc_cfg_event err %d!}", l_ret);
        return l_ret;
    }

    /* BEGIN:  mp12 P2P关联成为GO时，打印错误.修改wal_net_dev_stop 为需要等待hmac 响应 */
    /* 处理返回消息 */
    err_code = wal_check_and_release_msg_resp(pst_rsp_msg);
    if (err_code != OAL_SUCC) {
        oam_warning_log1(0, OAM_SF_ANY, "{wal_netdev_stop::hmac stop vap fail!err code [%d]}", err_code);
        return -OAL_EFAIL;
    }
    /* END:  mp12 P2P关联成为GO时，打印错误.修改wal_net_dev_stop 为需要等待hmac 响应 */
    return OAL_SUCC;
}
OAL_STATIC int32_t wal_netdev_stop_get_netdev(oal_net_device_stru *net_dev,
    mac_device_stru *mac_dev, mac_vap_stru *mac_vap)
{
    int32_t l_ret;
    mac_device_stru *pst_mac_device = NULL;
    int8_t netdev_name[MAC_NET_DEVICE_NAME_LENGTH] = { 0 };
    uint8_t dev_id = mac_dev->uc_device_id;

    /* 通过device id获取netdev名字 */
    if (snprintf_s(netdev_name, MAC_NET_DEVICE_NAME_LENGTH, MAC_NET_DEVICE_NAME_LENGTH - 1, "wlan%u", dev_id) < 0) {
        return -OAL_EINVAL;
    }

    if ((NL80211_IFTYPE_STATION == net_dev->ieee80211_ptr->iftype) &&
        ((0 == oal_strcmp((const char *)netdev_name, net_dev->name)) ||
        (0 == oal_strcmp(WLAN1_NETDEV_NAME, net_dev->name)))) {
        l_ret = wal_deinit_wlan_vap(net_dev);
        if (l_ret != OAL_SUCC) {
            return l_ret;
        }
    }

    /* p2p0 down时 删除VAP */
    if (snprintf_s(netdev_name, MAC_NET_DEVICE_NAME_LENGTH, MAC_NET_DEVICE_NAME_LENGTH - 1, "p2p%u", dev_id) < 0) {
        return -OAL_EINVAL;
    }
    // 如果上层删除p2p group(GC or GO)时，调用wal_del_p2p_group删除p2p group的vap调用work queue删除GC or GO netdev
    // 如果上层不下发删除p2p group的命令，则在删除p2p device的时候删除p2p group，不要在删除wlan0的时候删除p2p group
    if ((net_dev->ieee80211_ptr->iftype == NL80211_IFTYPE_STATION) &&
        (oal_strcmp((const char *)netdev_name, net_dev->name) == 0)) {
        /* 用于删除p2p小组 */
        mac_vap = oal_net_dev_priv(net_dev);
        if (mac_vap == NULL) {
            oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_stop::pst_mac_vap is null, netdev released.}");
            return OAL_SUCC;
        }
        pst_mac_device = mac_res_get_dev(mac_vap->uc_device_id);
        /* P2P组网失败后，关wifi时上层没有下发删除GC直接删除P2P DEV产生异常。增加保护优先删除p2p小组 */
        if (pst_mac_device != NULL) {
            wal_del_p2p_group(pst_mac_device);
        }
    }

    if (((net_dev->ieee80211_ptr->iftype == NL80211_IFTYPE_STATION) ||
        (net_dev->ieee80211_ptr->iftype == NL80211_IFTYPE_P2P_DEVICE)) &&
        (oal_strcmp((const char *)netdev_name, net_dev->name) == 0)) {
        l_ret = wal_deinit_wlan_vap(net_dev);
        if (l_ret != OAL_SUCC) {
            return l_ret;
        }
    }

    return OAL_SUCC;
}

void wal_netdev_stop_queue(oal_net_device_stru *pst_net_dev)
{
    /* stop the netdev's queues */
    oal_net_tx_stop_all_queues(pst_net_dev); /* 停止发送队列 */
    wal_force_scan_complete(pst_net_dev, OAL_TRUE);

    oal_io_print("wal_netdev_stop,dev_name is:%s\n", pst_net_dev->name);
    oam_warning_log1(0, OAM_SF_ANY, "{wal_netdev_stop::iftype:%d.!}", pst_net_dev->ieee80211_ptr->iftype);
}

void wal_set_napi_disable(oal_net_device_stru *pst_net_dev)
{
    oal_netdev_priv_stru *pst_netdev_priv = NULL;
    if (oal_unlikely(pst_net_dev == NULL || pst_net_dev->ieee80211_ptr == NULL)) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_set_napi_disable::pst_net_dev or ieee80211_ptr null!}");
        return;
    }

    pst_netdev_priv = (oal_netdev_priv_stru *)oal_net_dev_wireless_priv(pst_net_dev);
    if (pst_netdev_priv->uc_napi_enable && ((NL80211_IFTYPE_STATION == pst_net_dev->ieee80211_ptr->iftype) ||
        (NL80211_IFTYPE_P2P_DEVICE == pst_net_dev->ieee80211_ptr->iftype) ||
        (NL80211_IFTYPE_P2P_CLIENT == pst_net_dev->ieee80211_ptr->iftype))) {
#ifndef WIN32
        oal_netbuf_list_purge(&pst_netdev_priv->st_rx_netbuf_queue);
#endif
        oal_napi_disable(&pst_netdev_priv->st_napi);
        pst_netdev_priv->uc_state = 0;
    }
}

/*
 * 函 数 名  : wal_netdev_stop
 * 功能描述  : 停用vap
 * 1.日    期  : 2013年5月13日
 *   作    者  : wifi
 *   修改内容  : 新生成函数
 */
OAL_STATIC int32_t _wal_netdev_stop(oal_net_device_stru *pst_net_dev)
{
    int32_t l_ret;
    mac_vap_stru *pst_mac_vap = NULL;
    mac_device_stru *pst_mac_dev = NULL;

    if (oal_unlikely(pst_net_dev == NULL || pst_net_dev->ieee80211_ptr == NULL)) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_stop::pst_net_dev null!}");
        return -OAL_EFAUL;
    }

    wal_netdev_stop_queue(pst_net_dev);

    /* AP模式下,在模式切换时down和删除 vap */
    if (pst_net_dev->ieee80211_ptr->iftype == NL80211_IFTYPE_AP) {
        l_ret = wal_netdev_stop_ap(pst_net_dev);
        return l_ret;
    }

    /* 如果netdev不是running状态，则直接返回成功 */
    if ((oal_netdevice_flags(pst_net_dev) & OAL_IFF_RUNNING) == 0) {
        oam_warning_log0(0, OAM_SF_ANY, "{_wal_netdev_stop::vap is already down!}");
        return OAL_SUCC;
    }

    /* 如果netdev下mac vap已经释放，则直接返回成功 */
    pst_mac_vap = oal_net_dev_priv(pst_net_dev);
    if (oal_unlikely(pst_mac_vap == NULL)) {
        oam_warning_log1(0, OAM_SF_CFG, "{_wal_netdev_stop:: mac vap of netdevice is down!iftype:[%d]}",
            pst_net_dev->ieee80211_ptr->iftype);
        return OAL_SUCC;
    }

    if (pst_mac_vap->en_vap_state == MAC_VAP_STATE_INIT) {
        oam_warning_log3(0, OAM_SF_ANY, "{_wal_netdev_stop::vap is already down!iftype:[%d] vap mode[%d] p2p mode[%d]}",
                         pst_net_dev->ieee80211_ptr->iftype, pst_mac_vap->en_vap_mode, pst_mac_vap->en_p2p_mode);
        if (WLAN_P2P_DEV_MODE != pst_mac_vap->en_p2p_mode) {
            /*  p2p client设备down掉成为p2p device，需要继续执行deinit操作，不能直接返回 */
            return OAL_SUCC;
        }
    }

    /* 抛事件到wal层处理 */
    l_ret = wal_netdev_stop_msg_process(pst_net_dev);
    if (l_ret != OAL_SUCC) {
        oam_warning_log0(0, OAM_SF_ANY, "{_wal_netdev_stop::wal_netdev_stop_msg_process fail}");
        return l_ret;
    }

    pst_mac_dev = wal_get_macdev_by_netdev(pst_net_dev);
    if (pst_mac_dev == NULL) {
        oal_io_print("wal_deinit_wlan_vap::wal_get_macdev_by_netdev FAIL");
        return -OAL_EFAIL;
    }
    /* 通过device id获取netdev名字 */
    l_ret = wal_netdev_stop_get_netdev(pst_net_dev, pst_mac_dev, pst_mac_vap);
    if (l_ret != OAL_SUCC) {
        oam_warning_log0(0, OAM_SF_ANY, "{_wal_netdev_stop::wal_netdev_stop_get_netdev fail}");
        return l_ret;
    }

    wal_set_napi_disable(pst_net_dev);

    if (pst_net_dev->ieee80211_ptr->iftype == NL80211_IFTYPE_STATION) {
        hmac_ht_self_cure_clear_blacklist();
    }

    return OAL_SUCC;
}

/*
 * 函 数 名  : wal_netdev_stop_ap
 * 功能描述  : netdev_stop流程中针对ap的流程
 * 1.日    期  : 2017年7月19日
 *   作    者  : wifi
 *   修改内容  : 新生成函数
 */
int32_t wal_netdev_stop_ap(oal_net_device_stru *pst_net_dev)
{
    int32_t l_ret;

    if (oal_unlikely(pst_net_dev == NULL || pst_net_dev->ieee80211_ptr == NULL)) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_stop_ap::pst_net_dev null!}");
        return -OAL_EFAUL;
    }

    if (NL80211_IFTYPE_AP != pst_net_dev->ieee80211_ptr->iftype) {
#ifdef _PRE_WLAN_CHBA_MGMT
        if (oal_strcmp("chba0", pst_net_dev->name) != 0) {
            return OAL_SUCC;
        }
#else
        return OAL_SUCC;
#endif
    }

    /* 结束扫描,以防在20/40M扫描过程中关闭AP */
    wal_force_scan_complete(pst_net_dev, OAL_TRUE);

    /* AP关闭切换到STA模式,删除相关vap */
    /*  解决vap状态与netdevice状态不一致，无法删除vap的问题，VAP删除后，上报成功 */
    if (OAL_SUCC != wal_stop_vap(pst_net_dev)) {
        oam_warning_log0(0, OAM_SF_CFG, "{wal_netdev_stop_ap::wal_stop_vap enter a error.}");
    }

    /* 设置aput模式允许下电标志 */
    wal_set_power_mgmt_on(OAL_TRUE);

    l_ret = wal_deinit_wlan_vap(pst_net_dev);
    if (l_ret != OAL_SUCC) {
        oam_warning_log0(0, OAM_SF_CFG, "{wal_netdev_stop_ap::wal_deinit_wlan_vap enter a error.}");
        return l_ret;
    }

    /* Del aput后需要切换netdev iftype状态到station */
    pst_net_dev->ieee80211_ptr->iftype = NL80211_IFTYPE_STATION;

    /* APUT下电同样根据驱动当前vap_num，del_vap后调用wal_del_vap_try_wlan_pm_close下电 */
    return OAL_SUCC;
}

int32_t wal_netdev_stop(oal_net_device_stru *pst_net_dev)
{
    int32_t ret;

    if (oal_unlikely(pst_net_dev == NULL)) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_stop::pst_net_dev null!}");
        return -OAL_EFAUL;
    }

#ifdef _PRE_WLAN_CHBA_MGMT
    if (oal_strcmp("chba0", pst_net_dev->name) == 0) {
        oam_warning_log0(0, 0, "{wal_netdev_stop: chba enter wal_netdev_stop}");
    }
#endif

#ifdef _PRE_WLAN_FEATURE_DFR
#if (_PRE_OS_VERSION_LINUX == _PRE_OS_VERSION)
    mutex_lock(&g_st_dfr_info.wifi_excp_mutex);
#endif
#endif

    wal_wake_lock();
    ret = _wal_netdev_stop(pst_net_dev);
    wal_wake_unlock();

    if (ret == OAL_SUCC) {
        oal_netdevice_flags(pst_net_dev) &= ~OAL_IFF_RUNNING;
    }

#ifdef _PRE_WLAN_FEATURE_DFR
#if (_PRE_OS_VERSION_LINUX == _PRE_OS_VERSION)
    mutex_unlock(&g_st_dfr_info.wifi_excp_mutex);
#endif
#endif
    wal_del_vap_try_wlan_pm_close();
    return ret;
}

/*
 * 函 数 名  : wal_netdev_set_mac_addr
 * 功能描述  : 设置mac地址
 * 1.日    期  : 2012年12月24日
  *   修改内容  : 新生成函数
 */
OAL_STATIC int32_t _wal_netdev_set_mac_addr(oal_net_device_stru *net_dev, oal_sockaddr_stru *mac_addr)
{
    oal_wireless_dev_stru  *wdev = NULL; /* 对于P2P 场景，p2p0 和 p2p-p2p0 MAC 地址从wlan0获取 */
#if defined(_PRE_WLAN_FEATURE_SMP_SUPPORT)
    mac_vap_stru *pst_mac_vap = NULL;
#endif

    if (oal_unlikely(oal_any_null_ptr2(net_dev, mac_addr))) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_netdev_set_mac_addr::net_dev or p_addr null ptr}");
        return -OAL_EFAUL;
    }

    if (oal_netif_running(net_dev)) {
    oam_warning_log0(0, OAM_SF_ANY, "{wal_netdev_set_mac_addr::cannot set address; device running!}\r\n");
    return -OAL_EBUSY;
    }

    if (ether_is_multicast(mac_addr->sa_data)) {
        oam_warning_log0(0, OAM_SF_ANY, "{wal_netdev_set_mac_addr::can not set group/broadcast addr!}\r\n");
        return -OAL_EINVAL;
    }

    oal_set_mac_addr ((uint8_t *)(net_dev->dev_addr), (uint8_t *)(mac_addr->sa_data));

    wdev = (oal_wireless_dev_stru *)net_dev->ieee80211_ptr;
    if (wdev == NULL) {
        return -OAL_EFAUL;
    }

    oam_warning_log4(0, OAM_SF_ANY,
                     "{wal_netdev_set_mac_addr::iftype [%d], mac_addr[%.2x:xx:xx:xx:%.2x:%.2x]..}\r\n",
                     wdev->iftype, net_dev->dev_addr[MAC_ADDR_0],
                     net_dev->dev_addr[MAC_ADDR_4], net_dev->dev_addr[MAC_ADDR_5]);

#ifdef _PRE_WLAN_EXPORT
    oal_set_mac_addr(g_wlan_device_mac_addr, (uint8_t *)(net_dev->dev_addr));
#endif

    return OAL_SUCC;
}

int32_t wal_netdev_set_mac_addr(oal_net_device_stru *pst_net_dev, void *p_addr)
{
    int32_t ret;
    oal_sockaddr_stru *mac_addr = NULL;

    mac_addr = (oal_sockaddr_stru *)p_addr;
    wal_wake_lock();
    ret = _wal_netdev_set_mac_addr(pst_net_dev, mac_addr);
    wal_wake_unlock();

    if (ret != OAL_SUCC) {
        chr_exception_report(CHR_PLATFORM_EXCEPTION_EVENTID, CHR_SYSTEM_WIFI,
            CHR_LAYER_DRV, CHR_WIFI_DRV_EVENT_OPEN, CHR_WIFI_DRV_ERROR_POWER_ON_SET_MAC_ADDR);
    }

    return ret;
}

#ifdef _PRE_WLAN_FEATURE_VSP
static int32_t wal_vsp_priv_cmd_proc(hmac_vap_stru *hmac_vap, hmac_user_stru *hmac_user, hmac_vsp_cmd *vsp_cmd)
{
    uint32_t ret = OAL_FAIL;

    switch (vsp_cmd->cmd) {
        case VSP_CMD_CFG:
            ret = hmac_vsp_cfg(hmac_vap, hmac_user, vsp_cmd);
            break;
        case VSP_CMD_START:
            ret = hmac_vsp_start(hmac_user);
            break;
        case VSP_CMD_STOP:
            ret = hmac_vsp_stop(hmac_user);
            break;
        default:
            break;
    }

    if (ret != OAL_SUCC) {
        oam_error_log2(0, 0, "{wal_vsp_priv_cmd_proc::cmd[%d] proc failed! ret[%d]}", vsp_cmd->cmd, ret);
    }

    return ret == OAL_SUCC ? EOK : -OAL_EINVAL;
}

static int32_t wal_vsp_priv_cmd(oal_net_device_stru *net_dev, oal_ifreq_stru *ifr)
{
    hmac_vsp_cmd vsp_cmd = { 0 };
    mac_vap_stru *mac_vap = NULL;
    hmac_vap_stru *hmac_vap = NULL;
    hmac_user_stru *hmac_user = NULL;
    uint16_t user_idx;

    if (oal_any_null_ptr2(ifr->ifr_data, net_dev)) {
        return -OAL_EINVAL;
    }

    /* 将用户态数据拷贝到内核态 */
    if (oal_copy_from_user(&vsp_cmd, ifr->ifr_data, sizeof(hmac_vsp_cmd))) {
        return -OAL_EINVAL;
    }

    mac_vap = (mac_vap_stru *)oal_net_dev_priv(net_dev);
    if (!mac_vap) {
        return -OAL_EINVAL;
    }

    hmac_vap = (hmac_vap_stru *)mac_res_get_hmac_vap(mac_vap->uc_vap_id);
    if (!hmac_vap) {
        return -OAL_EINVAL;
    }

    if (mac_vap_find_user_by_macaddr(mac_vap, vsp_cmd.ra, &user_idx) != OAL_SUCC) {
        return -OAL_EINVAL;
    }

    hmac_user = mac_res_get_hmac_user(user_idx);
    if (!hmac_user) {
        return -OAL_EINVAL;
    }

    return wal_vsp_priv_cmd_proc(hmac_vap, hmac_user, &vsp_cmd);
}
#endif


/****************************** 私有无线状态扩展开始 ******************************/

/*
 * 说明：
 * 本区域为私有无线状态查询接口，非原SDK逻辑。
 * 修改或升级原SDK时，应整体保留本区域代码及对应调用入口。
 */

/****************************** 类型定义 ******************************/


/****************************** 缓存状态 ******************************/

typedef struct
{
    oal_bool_enum_uint8  valid;        /* 缓存是否有效 */
    uint8_t              reserved[3];  /* 保留字段 */
    uint32_t             timestamp_ms; /* 缓存更新时间，单位毫秒 */
    uint32_t             generation;   /* 缓存生命周期代数 */
    wal_radio_status_stru status;      /* 无线状态快照 */
} wal_radio_status_cache_stru;

/****************************** ABI请求 ******************************/

typedef struct
{
    uint16_t version;     /* 用户态请求的 ABI 版本 */
    uint16_t struct_size; /* 用户态状态缓冲区大小 */
} wal_radio_status_request_stru;


/****************************** 全局状态 ******************************/

static DEFINE_MUTEX(g_wal_radio_status_lock);
static wal_radio_status_cache_stru g_wal_radio_status_cache[WLAN_VAP_SUPPORT_MAX_NUM_LIMIT];

/****************************** 缓存管理 ******************************/

/*
 * 函 数 名  : wal_radio_status_cache_clear
 * 功能描述  : 清除指定VAP的无线状态缓存
 * 1.日    期  : 2026年7月24日
 *   作    者  : dawn
 *   修改内容  : 新生成函数
 */
void wal_radio_status_cache_clear(uint8_t vap_id)
{
    wal_radio_status_cache_stru *cache;

    if (vap_id >= WLAN_VAP_SUPPORT_MAX_NUM_LIMIT) {
        return;
    }

    mutex_lock(&g_wal_radio_status_lock);

    cache = &g_wal_radio_status_cache[vap_id];
    cache->valid        = OAL_FALSE;
    cache->timestamp_ms = 0;
    cache->generation++;

    (void)memset_s(&cache->status, sizeof(cache->status), 0, sizeof(cache->status));

    mutex_unlock(&g_wal_radio_status_lock);
}

/****************************** 基础状态转换 ******************************/

/* 获取当前工作带宽，单位MHz。 */
OAL_STATIC uint16_t wal_radio_status_bandwidth(const mac_vap_stru *mac_vap)
{
    wlan_channel_bandwidth_enum_uint8 bandwidth = mac_vap->st_channel.en_bandwidth;

#ifdef _PRE_WLAN_NARROW_BAND
    if (mac_vap->st_nb.en_open == OAL_TRUE) {
        if (mac_vap->st_nb.en_bw == NARROW_BW_1M) {
            return 1U;
        }

        if (mac_vap->st_nb.en_bw == NARROW_BW_5M) {
            return 5U;
        }

        if (mac_vap->st_nb.en_bw == NARROW_BW_10M) {
            return 10U;
        }
    }
#endif

    if ((bandwidth == WLAN_BAND_WIDTH_40PLUS) ||
        (bandwidth == WLAN_BAND_WIDTH_40MINUS)) {
        return 40U;
    }

    if ((bandwidth >= WLAN_BAND_WIDTH_80PLUSPLUS) &&
        (bandwidth <= WLAN_BAND_WIDTH_80MINUSMINUS)) {
        return 80U;
    }

    return 20U;
}

/* 将当前信道转换为中心频率，单位MHz。 */
OAL_STATIC uint32_t wal_radio_status_frequency(mac_vap_stru *mac_vap)
{
    uint8_t channel = mac_vap->st_channel.uc_chan_number;
    uint8_t band;

    if (channel == 0U) {
        return 0U;
    }

    band = hmac_get_80211_band_type(&mac_vap->st_channel);

    return (uint32_t)oal_ieee80211_channel_to_frequency(channel, band);
}

/* 获取当前VAP工作角色。 */
OAL_STATIC uint8_t wal_radio_status_role(const mac_vap_stru *mac_vap)
{
    if (mac_vap->en_vap_mode == WLAN_VAP_MODE_BSS_AP) {
        return WAL_RADIO_ROLE_AP;
    }

    if (mac_vap->en_vap_mode == WLAN_VAP_MODE_BSS_STA) {
        return WAL_RADIO_ROLE_STA;
    }

    return WAL_RADIO_ROLE_UNKNOWN;
}

/* 获取当前无线接口运行状态。 */
OAL_STATIC uint8_t wal_radio_status_state(const oal_net_device_stru *net_dev,
    const mac_vap_stru *mac_vap)
{
    if (!oal_netif_running(net_dev)) {
        return WAL_RADIO_STATE_DOWN;
    }

    return (mac_vap->us_user_nums != 0U) ?
        WAL_RADIO_STATE_CONNECTED : WAL_RADIO_STATE_READY;
}

/****************************** 对端状态填充 ******************************/

/* 填充单个关联对端的无线状态。 */
OAL_STATIC void wal_radio_status_fill_peer(wal_radio_peer_status_stru *peer, const uint8_t *mac,
    const oal_station_info_stru *sta_info, const hmac_user_stru *hmac_user, int32_t query_ret)
{
    uint32_t current_time = (uint32_t)oal_time_get_stamp_ms();

    peer->rssi_dbm = -127;

    if (memcpy_s(peer->mac, sizeof(peer->mac), mac, WAL_RADIO_STATUS_MAC_LENGTH) != EOK) {
        peer->flags = WAL_RADIO_PEER_FLAG_QUERY_FAILED;
        return;
    }

    peer->flags = WAL_RADIO_PEER_FLAG_VALID;

    if ((hmac_user != NULL) && (hmac_user->first_add_time != 0U)) {
        peer->connected_time_s =
            (uint32_t)oal_time_get_runtime(hmac_user->first_add_time, current_time) / HMAC_S_TO_MS;
    }

    if (query_ret != OAL_SUCC) {
        peer->flags |= WAL_RADIO_PEER_FLAG_QUERY_FAILED;
        return;
    }

    peer->rssi_dbm           = sta_info->signal;
    peer->inactive_ms        = sta_info->inactive_time;
    peer->tx_rate_kbps       = (uint32_t)sta_info->txrate.legacy * 100U;
    peer->rx_rate_kbps       = (uint32_t)sta_info->rxrate.legacy * 100U;
    peer->driver_tx_bytes    = sta_info->tx_bytes;
    peer->driver_rx_bytes    = sta_info->rx_bytes;
    peer->driver_tx_packets  = sta_info->tx_packets;
    peer->driver_rx_packets  = sta_info->rx_packets;
    peer->driver_tx_failed   = sta_info->tx_failed;
}

/****************************** 无线状态构建 ******************************/

/*
 * 函 数 名  : wal_radio_status_build
 * 功能描述  : 构建当前VAP及其关联对端的无线状态快照
 * 1.日    期  : 2026年7月24日
 *   作    者  : dawn
 *   修改内容  : 新生成函数
 */
OAL_STATIC int32_t wal_radio_status_build(oal_net_device_stru *net_dev, mac_vap_stru *mac_vap,
    hmac_vap_stru *hmac_vap, wal_radio_status_stru *status)
{
    oal_wireless_dev_stru       *wdev;
    oal_station_info_stru        sta_info;
    hmac_user_stru              *hmac_user;
    wal_radio_peer_status_stru  *peer;
    uint8_t                      mac[WLAN_MAC_ADDR_LEN];
    uint32_t                     index;
    int32_t                      query_ret;

    if (oal_any_null_ptr4(net_dev, mac_vap, hmac_vap, status)) {
        return -OAL_EINVAL;
    }

    wdev = oal_netdevice_wdev(net_dev);
    if ((wdev == NULL) || (wdev->wiphy == NULL)) {
        return -OAL_EINVAL;
    }

    if (memset_s(status, sizeof(*status), 0, sizeof(*status)) != EOK) {
        return -OAL_EFAIL;
    }

    status->version       = WAL_RADIO_STATUS_ABI_VERSION;
    status->struct_size   = (uint16_t)sizeof(*status);
    status->role          = wal_radio_status_role(mac_vap);
    status->state         = wal_radio_status_state(net_dev, mac_vap);
    status->channel       = mac_vap->st_channel.uc_chan_number;
    status->frequency_mhz = wal_radio_status_frequency(mac_vap);
    status->bandwidth_mhz = wal_radio_status_bandwidth(mac_vap);

    if (memcpy_s(status->local_mac, sizeof(status->local_mac), net_dev->dev_addr,
        WAL_RADIO_STATUS_MAC_LENGTH) != EOK) {
        return -OAL_EFAIL;
    }

    if (mac_vap->us_user_nums > WAL_RADIO_STATUS_MAX_PEERS) {
        status->flags |= WAL_RADIO_FLAG_PEERS_TRUNCATED;
    }

    for (index = 0U; index < WAL_RADIO_STATUS_MAX_PEERS; index++) {
        if (memset_s(&sta_info, sizeof(sta_info), 0, sizeof(sta_info)) != EOK) {
            return -OAL_EFAIL;
        }

        if (memset_s(mac, sizeof(mac), 0, sizeof(mac)) != EOK) {
            return -OAL_EFAIL;
        }

        query_ret = wal_cfg80211_dump_station(
            wdev->wiphy, net_dev, (int32_t)index, mac, &sta_info);

        if (query_ret == -OAL_ENOENT) {
            break;
        }

        if (mac_addr_is_zero(mac) == OAL_TRUE) {
            status->flags |= WAL_RADIO_FLAG_PARTIAL;
            continue;
        }

        hmac_user = mac_vap_get_hmac_user_by_addr(mac_vap, mac, WLAN_MAC_ADDR_LEN);
        peer      = &status->peers[status->peer_count];

        wal_radio_status_fill_peer(peer, mac, &sta_info, hmac_user, query_ret);
        status->peer_count++;

        if (query_ret != OAL_SUCC) {
            status->flags |= WAL_RADIO_FLAG_PARTIAL;
        }
    }

    status->noise_dbm = hmac_vap->s_free_power;

    if ((status->noise_dbm < 0) && (status->noise_dbm >= -120)) {
        status->flags |= WAL_RADIO_FLAG_NOISE_VALID;
    }

    return OAL_SUCC;
}

/*
 * 函 数 名  : wal_radio_status_ioctl
 * 功能描述  : 处理无线状态私有ioctl查询请求
 * 1.日    期  : 2026年7月24日
 *   作    者  : dawn
 *   修改内容  : 新生成函数
 */
OAL_STATIC int32_t wal_radio_status_ioctl(oal_net_device_stru *net_dev, oal_ifreq_stru *ifr)
{
    mac_vap_stru                   *mac_vap;
    hmac_vap_stru                  *hmac_vap;
    wal_radio_status_cache_stru    *cache;
    wal_radio_status_request_stru   request;
    wal_radio_status_stru           status;
    oal_bool_enum_uint8             cache_hit = OAL_FALSE;
    uint32_t                        now;
    uint32_t                        age;
    uint32_t                        generation;
    uint8_t                         expected_peer_count;
    int32_t                         ret;

    mac_vap = (mac_vap_stru *)oal_net_dev_priv(net_dev);

    /* 读取并校验用户态请求头。 */
    if (oal_copy_from_user(&request, ifr->ifr_data, sizeof(request)) != 0) {
        return -OAL_EFAUL;
    }

    if ((request.version != WAL_RADIO_STATUS_ABI_VERSION) ||
        (request.struct_size != sizeof(wal_radio_status_stru))) {
        return -OAL_EINVAL;
    }

    if ((mac_vap == NULL) ||
        (mac_vap->uc_vap_id >= WLAN_VAP_SUPPORT_MAX_NUM_LIMIT)) {
        return -OAL_EINVAL;
    }

    hmac_vap = (hmac_vap_stru *)mac_res_get_hmac_vap(mac_vap->uc_vap_id);
    if (hmac_vap == NULL) {
        return -OAL_EINVAL;
    }

    expected_peer_count = (uint8_t)oal_min(mac_vap->us_user_nums, WAL_RADIO_STATUS_MAX_PEERS);

    /* 检查当前VAP缓存是否仍然有效。 */
    now = (uint32_t)oal_time_get_stamp_ms();

    mutex_lock(&g_wal_radio_status_lock);

    cache      = &g_wal_radio_status_cache[mac_vap->uc_vap_id];
    generation = cache->generation;
    age        = (uint32_t)oal_time_get_runtime(cache->timestamp_ms, now);

    if ((cache->valid == OAL_TRUE) &&
        (age < WAL_RADIO_STATUS_REFRESH_MS) &&
        (cache->status.role == wal_radio_status_role(mac_vap)) &&
        (cache->status.state == wal_radio_status_state(net_dev, mac_vap)) &&
        (cache->status.peer_count == expected_peer_count) &&
        (cache->status.channel == mac_vap->st_channel.uc_chan_number) &&
        (cache->status.bandwidth_mhz == wal_radio_status_bandwidth(mac_vap))) {
        status    = cache->status;
        cache_hit = OAL_TRUE;
    }

    mutex_unlock(&g_wal_radio_status_lock);

    /* 缓存无效时重新构建无线状态快照。 */
    if (cache_hit != OAL_TRUE) {
        ret = wal_radio_status_build(net_dev, mac_vap, hmac_vap, &status);
        if (ret != OAL_SUCC) {
            return ret;
        }

        now = (uint32_t)oal_time_get_stamp_ms();

        mutex_lock(&g_wal_radio_status_lock);

        cache = &g_wal_radio_status_cache[mac_vap->uc_vap_id];

        /*
         * 构建期间若缓存被主动清除，则generation会发生变化，
         * 此时不再写回本次可能已经失效的状态快照。
         */
        if (cache->generation == generation) {
            cache->status       = status;
            cache->timestamp_ms = now;
            cache->valid        = OAL_TRUE;
        }

        mutex_unlock(&g_wal_radio_status_lock);
    }

    /* 将完整无线状态返回用户态。 */
    if (oal_copy_to_user(ifr->ifr_data, &status, sizeof(status)) != 0) {
        return -OAL_EFAUL;
    }

    return OAL_SUCC;
}

/****************************** 发送流控状态扩展 ******************************/

/**
 * @brief 获取Host HCC发送队列及Device发送流控状态。
 */
OAL_STATIC int32_t wal_tx_flowctrl_ioctl(oal_ifreq_stru *ifr)
{
    struct hcc_handler          *hcc;
    hcc_tx_flow_ctrl_info       *flow_ctrl;
    wal_tx_flowctrl_status_stru  request = {0};
    wal_tx_flowctrl_status_stru  status = {0};
    uint32_t                     flowctrl_flag;

    if (ifr == NULL || ifr->ifr_data == NULL)
    {
        return -OAL_EFAUL;
    }

    if (oal_copy_from_user(&request, ifr->ifr_data, sizeof(request)) != 0)
    {
        return -OAL_EFAUL;
    }

    if (request.version != WAL_TX_FLOWCTRL_ABI_VERSION ||
        request.struct_size != sizeof(request))
    {
        return -OAL_EINVAL;
    }

    hcc = hcc_get_handler(HCC_EP_WIFI_DEV);
    if (hcc == NULL)
    {
        return -OAL_ENODEV;
    }

    flow_ctrl = &hcc->hcc_transer_info.tx_flow_ctrl;

    status.version     = WAL_TX_FLOWCTRL_ABI_VERSION;
    status.struct_size = sizeof(status);

    status.vi_queue_length = oal_netbuf_list_len( &hcc->hcc_transer_info.hcc_queues[HCC_TX].queues[DATA_UDP_VI_QUEUE].data_queue);
    status.vo_queue_length = oal_netbuf_list_len( &hcc->hcc_transer_info.hcc_queues[HCC_TX].queues[DATA_UDP_VO_QUEUE].data_queue);
    status.be_queue_length = oal_netbuf_list_len( &hcc->hcc_transer_info.hcc_queues[HCC_TX].queues[DATA_UDP_BE_QUEUE].data_queue);

    status.flowctrl_off_count = oal_read_once(flow_ctrl->flowctrl_off_count);
    flowctrl_flag             = oal_read_once(flow_ctrl->flowctrl_flag);

    status.tx_allowed = (oal_atomic_read(&hcc->state) == HCC_ON && flowctrl_flag == D2H_MSG_FLOWCTRL_ON) ? 1U : 0U;

    if (oal_copy_to_user(ifr->ifr_data, &status, sizeof(status)) != 0)
    {
        return -OAL_EFAUL;
    }

    return OAL_SUCC;
}


/****************************** 私有无线状态扩展结束 ******************************/


int32_t wal_net_device_ioctl(oal_net_device_stru *net_dev, oal_ifreq_stru *ifr, int32_t cmd)
{
    int32_t ret = 0;

#if (_PRE_OS_VERSION_LINUX == _PRE_OS_VERSION)
    if (!capable(CAP_NET_ADMIN)) {
        return -EPERM;
    }
#endif

#ifdef _PRE_WLAN_FEATURE_DFR
    if (g_st_dfr_info.bit_device_reset_process_flag)
    {
        oam_warning_log1(0, OAM_SF_ANY,
                         "{dfr_process_status[%d]!}",
                         g_st_dfr_info.bit_device_reset_process_flag);

        if (cmd == WAL_TX_FLOWCTRL_IOCTL)
        {
            return -OAL_EAGAIN;
        }

        return OAL_SUCC;
    }
#endif

    if (oal_any_null_ptr3(net_dev, ifr, ifr->ifr_data)) {
        oam_error_log0(0, OAM_SF_ANY, "{wal_net_device_ioctl::pst_dev or pst_ifr null!}");
        return -OAL_EFAUL;
    }

    /* mp12 wpa_supplicant 通过ioctl 下发命令 */
    if (cmd == (WAL_SIOCDEVPRIVATE + 1)) {
        return wal_vendor_priv_cmd(net_dev, ifr, cmd);
    }
#if (_PRE_OS_VERSION_WIN32 != _PRE_OS_VERSION)
    /* atcmdsrv 通过ioctl下发命令，上层适配+2 */
    if (cmd == (WAL_SIOCDEVPRIVATE + 2)) {
#ifdef PLATFORM_DEBUG_ENABLE
        if (mpxx_get_os_build_variant() == MPXX_OS_BUILD_VARIANT_ROOT) {
            wal_wake_lock();
            ret = wal_atcmdsrv_wifi_priv_cmd(net_dev, ifr, cmd);
            wal_wake_unlock();
        }
#endif
        return ret;
    }
#ifdef _PRE_WLAN_RR_PERFORMANCE
    /* PC APP 通过ioctl下发的命令，上层适配+3 */
    if (cmd == (WAL_SIOCDEVPRIVATE + 3)) {
        return wal_ext_priv_cmd(net_dev, ifr);
    }
#endif /* _PRE_WLAN_RR_PERFORMANCE */
#endif

#ifdef _PRE_WLAN_FEATURE_VSP
    /* VSP 通过ioctl下发的命令，上层适配+4 */
    if (cmd == (WAL_SIOCDEVPRIVATE + 4)) {
        return wal_vsp_priv_cmd(net_dev, ifr);
    }
#endif

    if (cmd == WAL_RADIO_STATUS_IOCTL) {
        return wal_radio_status_ioctl(net_dev, ifr);
    }

    if (cmd == WAL_TX_FLOWCTRL_IOCTL)
    {
        return wal_tx_flowctrl_ioctl(ifr);
    }

    oam_warning_log1(0, OAM_SF_ANY, "{wal_net_device_ioctl::unrecognised cmd[0x%x]!}", cmd);
    return OAL_SUCC;
}







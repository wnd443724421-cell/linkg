/**
 * @file wpa_ctrl.h
 * @brief wpa_supplicant与hostapd控制接口定义
 * @author Jouni Malinen
 * @note 本文件遵循BSD许可证，详细信息请参阅README。
 * @copyright Copyright (c) 2004-2017, Jouni Malinen <j@w1.fi>
 */

#ifndef WPA_CTRL_H
#define WPA_CTRL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 控制请求前缀 ******************************/

#define WPA_CTRL_REQ                              "CTRL-REQ-"                                   // 身份、密码或PIN交互请求前缀
#define WPA_CTRL_RSP                              "CTRL-RSP-"                                   // 身份、密码或PIN交互响应前缀

/****************************** wpa_supplicant连接事件 ******************************/

#define WPA_EVENT_CONNECTED                       "CTRL-EVENT-CONNECTED "                       // 认证完成并建立数据连接
#define WPA_EVENT_DISCONNECTED                    "CTRL-EVENT-DISCONNECTED "                    // 连接断开且数据连接不可用
#define WPA_EVENT_ASSOC_REJECT                    "CTRL-EVENT-ASSOC-REJECT "                    // 关联请求被拒绝
#define WPA_EVENT_AUTH_REJECT                     "CTRL-EVENT-AUTH-REJECT "                     // 认证请求被拒绝
#define WPA_EVENT_TERMINATING                     "CTRL-EVENT-TERMINATING "                     // wpa_supplicant正在退出
#define WPA_EVENT_PASSWORD_CHANGED                "CTRL-EVENT-PASSWORD-CHANGED "                // 密码修改成功

/****************************** EAP事件 ******************************/

#define WPA_EVENT_EAP_NOTIFICATION                "CTRL-EVENT-EAP-NOTIFICATION "                // 收到EAP通知请求
#define WPA_EVENT_EAP_STARTED                     "CTRL-EVENT-EAP-STARTED "                     // EAP认证开始
#define WPA_EVENT_EAP_PROPOSED_METHOD             "CTRL-EVENT-EAP-PROPOSED-METHOD "             // 服务端提出EAP认证方法
#define WPA_EVENT_EAP_METHOD                      "CTRL-EVENT-EAP-METHOD "                      // 已选择EAP认证方法
#define WPA_EVENT_EAP_PEER_CERT                   "CTRL-EVENT-EAP-PEER-CERT "                   // 收到TLS对端证书
#define WPA_EVENT_EAP_PEER_ALT                    "CTRL-EVENT-EAP-PEER-ALT "                    // 收到TLS对端证书备用主题名称
#define WPA_EVENT_EAP_TLS_CERT_ERROR              "CTRL-EVENT-EAP-TLS-CERT-ERROR "              // TLS证书链校验失败
#define WPA_EVENT_EAP_STATUS                      "CTRL-EVENT-EAP-STATUS "                      // EAP认证状态更新
#define WPA_EVENT_EAP_RETRANSMIT                  "CTRL-EVENT-EAP-RETRANSMIT "                  // 重传上一次EAP请求
#define WPA_EVENT_EAP_RETRANSMIT2                 "CTRL-EVENT-EAP-RETRANSMIT2 "                 // 重传上一次EAP请求扩展事件
#define WPA_EVENT_EAP_SUCCESS                     "CTRL-EVENT-EAP-SUCCESS "                     // EAP认证成功
#define WPA_EVENT_EAP_SUCCESS2                    "CTRL-EVENT-EAP-SUCCESS2 "                    // EAP认证成功扩展事件
#define WPA_EVENT_EAP_FAILURE                     "CTRL-EVENT-EAP-FAILURE "                     // EAP认证失败
#define WPA_EVENT_EAP_FAILURE2                    "CTRL-EVENT-EAP-FAILURE2 "                    // EAP认证失败扩展事件
#define WPA_EVENT_EAP_TIMEOUT_FAILURE             "CTRL-EVENT-EAP-TIMEOUT-FAILURE "             // EAP认证因无响应超时失败
#define WPA_EVENT_EAP_TIMEOUT_FAILURE2            "CTRL-EVENT-EAP-TIMEOUT-FAILURE2 "            // EAP认证超时失败扩展事件
#define WPA_EVENT_EAP_ERROR_CODE                  "EAP-ERROR-CODE "                             // EAP错误码事件

/****************************** 网络与扫描事件 ******************************/

#define WPA_EVENT_TEMP_DISABLED                   "CTRL-EVENT-SSID-TEMP-DISABLED "              // 网络配置因认证失败等原因被临时禁用
#define WPA_EVENT_REENABLED                       "CTRL-EVENT-SSID-REENABLED "                  // 临时禁用的网络配置已重新启用
#define WPA_EVENT_SCAN_STARTED                    "CTRL-EVENT-SCAN-STARTED "                    // 无线扫描开始
#define WPA_EVENT_SCAN_RESULTS                    "CTRL-EVENT-SCAN-RESULTS "                    // 无线扫描结果可用
#define WPA_EVENT_SCAN_FAILED                     "CTRL-EVENT-SCAN-FAILED "                     // 无线扫描失败
#define WPA_EVENT_STATE_CHANGE                    "CTRL-EVENT-STATE-CHANGE "                    // wpa_supplicant状态发生变化
#define WPA_EVENT_BSS_ADDED                       "CTRL-EVENT-BSS-ADDED "                       // 新增BSS扫描条目
#define WPA_EVENT_BSS_REMOVED                     "CTRL-EVENT-BSS-REMOVED "                     // 删除BSS扫描条目
#define WPA_EVENT_NETWORK_NOT_FOUND               "CTRL-EVENT-NETWORK-NOT-FOUND "               // 未找到可用网络
#define WPA_EVENT_SIGNAL_CHANGE                   "CTRL-EVENT-SIGNAL-CHANGE "                   // 驱动上报信号强度变化
#define WPA_EVENT_BEACON_LOSS                     "CTRL-EVENT-BEACON-LOSS "                     // 驱动上报信标丢失
#define WPA_EVENT_REGDOM_CHANGE                   "CTRL-EVENT-REGDOM-CHANGE "                   // 监管域发生变化
#define WPA_EVENT_CHANNEL_SWITCH_STARTED          "CTRL-EVENT-STARTED-CHANNEL-SWITCH "          // 信道切换开始
#define WPA_EVENT_CHANNEL_SWITCH                  "CTRL-EVENT-CHANNEL-SWITCH "                  // 信道切换完成
#define WPA_EVENT_SAE_UNKNOWN_PASSWORD_IDENTIFIER "CTRL-EVENT-SAE-UNKNOWN-PASSWORD-IDENTIFIER " // SAE认证因密码标识符未知而失败
#define WPA_EVENT_UNPROT_BEACON                   "CTRL-EVENT-UNPROT-BEACON "                   // 未受保护的信标帧被丢弃
#define WPA_EVENT_SUBNET_STATUS_UPDATE            "CTRL-EVENT-SUBNET-STATUS-UPDATE "            // IP子网状态更新
#define IBSS_RSN_COMPLETED                        "IBSS-RSN-COMPLETED "                         // 指定IBSS对端RSN四次握手完成
#define WPA_EVENT_FREQ_CONFLICT                   "CTRL-EVENT-FREQ-CONFLICT "                   // 并发操作导致频率冲突
#define WPA_EVENT_AVOID_FREQ                      "CTRL-EVENT-AVOID-FREQ "                      // 驱动建议避开的频率范围

/****************************** WPS事件 ******************************/

#define WPS_EVENT_OVERLAP                         "WPS-OVERLAP-DETECTED "                       // PBC模式检测到WPS重叠
#define WPS_EVENT_AP_AVAILABLE_PBC                "WPS-AP-AVAILABLE-PBC "                       // 发现启用PBC的WPS接入点
#define WPS_EVENT_AP_AVAILABLE_AUTH               "WPS-AP-AVAILABLE-AUTH "                      // 发现已授权本机地址的WPS接入点
#define WPS_EVENT_AP_AVAILABLE_PIN                "WPS-AP-AVAILABLE-PIN "                       // 发现已选择PIN注册器的WPS接入点
#define WPS_EVENT_AP_AVAILABLE                    "WPS-AP-AVAILABLE "                           // 发现可用WPS接入点
#define WPS_EVENT_CRED_RECEIVED                   "WPS-CRED-RECEIVED "                          // 收到新的WPS凭据
#define WPS_EVENT_M2D                             "WPS-M2D "                                    // 收到WPS M2D消息
#define WPS_EVENT_FAIL                            "WPS-FAIL "                                   // WPS注册失败
#define WPS_EVENT_SUCCESS                         "WPS-SUCCESS "                                // WPS注册成功
#define WPS_EVENT_TIMEOUT                         "WPS-TIMEOUT "                                // WPS注册超时
#define WPS_EVENT_ACTIVE                          "WPS-PBC-ACTIVE "                             // WPS PBC模式已启用
#define WPS_EVENT_DISABLE                         "WPS-PBC-DISABLE "                            // WPS PBC模式已禁用
#define WPS_EVENT_ENROLLEE_SEEN                   "WPS-ENROLLEE-SEEN "                          // 发现WPS注册终端
#define WPS_EVENT_OPEN_NETWORK                    "WPS-OPEN-NETWORK "                           // 检测到WPS开放网络

/****************************** WPS外部注册器事件 ******************************/

#define WPS_EVENT_ER_AP_ADD                       "WPS-ER-AP-ADD "                              // 外部注册器新增接入点
#define WPS_EVENT_ER_AP_REMOVE                    "WPS-ER-AP-REMOVE "                           // 外部注册器移除接入点
#define WPS_EVENT_ER_ENROLLEE_ADD                 "WPS-ER-ENROLLEE-ADD "                        // 外部注册器新增注册终端
#define WPS_EVENT_ER_ENROLLEE_REMOVE              "WPS-ER-ENROLLEE-REMOVE "                     // 外部注册器移除注册终端
#define WPS_EVENT_ER_AP_SETTINGS                  "WPS-ER-AP-SETTINGS "                         // 外部注册器收到接入点配置
#define WPS_EVENT_ER_SET_SEL_REG                  "WPS-ER-AP-SET-SEL-REG "                      // 外部注册器设置选定注册器

/****************************** DPP事件 ******************************/

#define DPP_EVENT_AUTH_SUCCESS                    "DPP-AUTH-SUCCESS "                           // DPP认证成功
#define DPP_EVENT_AUTH_INIT_FAILED                "DPP-AUTH-INIT-FAILED "                       // DPP认证初始化失败
#define DPP_EVENT_NOT_COMPATIBLE                  "DPP-NOT-COMPATIBLE "                         // DPP能力不兼容
#define DPP_EVENT_RESPONSE_PENDING                "DPP-RESPONSE-PENDING "                       // DPP响应等待中
#define DPP_EVENT_SCAN_PEER_QR_CODE               "DPP-SCAN-PEER-QR-CODE "                      // 扫描对端DPP二维码
#define DPP_EVENT_AUTH_DIRECTION                  "DPP-AUTH-DIRECTION "                         // DPP认证方向信息
#define DPP_EVENT_CONF_RECEIVED                   "DPP-CONF-RECEIVED "                          // 收到DPP配置
#define DPP_EVENT_CONF_SENT                       "DPP-CONF-SENT "                              // DPP配置发送完成
#define DPP_EVENT_CONF_FAILED                     "DPP-CONF-FAILED "                            // DPP配置失败
#define DPP_EVENT_CONN_STATUS_RESULT              "DPP-CONN-STATUS-RESULT "                     // DPP连接状态结果
#define DPP_EVENT_CONFOBJ_AKM                     "DPP-CONFOBJ-AKM "                            // DPP配置对象AKM信息
#define DPP_EVENT_CONFOBJ_SSID                    "DPP-CONFOBJ-SSID "                           // DPP配置对象SSID
#define DPP_EVENT_CONFOBJ_SSID_CHARSET            "DPP-CONFOBJ-SSID-CHARSET "                   // DPP配置对象SSID字符集
#define DPP_EVENT_CONFOBJ_PASS                    "DPP-CONFOBJ-PASS "                           // DPP配置对象密码
#define DPP_EVENT_CONFOBJ_PSK                     "DPP-CONFOBJ-PSK "                            // DPP配置对象PSK
#define DPP_EVENT_CONNECTOR                       "DPP-CONNECTOR "                              // DPP连接器
#define DPP_EVENT_C_SIGN_KEY                      "DPP-C-SIGN-KEY "                             // DPP连接器签名密钥
#define DPP_EVENT_NET_ACCESS_KEY                  "DPP-NET-ACCESS-KEY "                         // DPP网络访问密钥
#define DPP_EVENT_MISSING_CONNECTOR               "DPP-MISSING-CONNECTOR "                      // 缺少DPP连接器
#define DPP_EVENT_NETWORK_ID                      "DPP-NETWORK-ID "                             // DPP网络标识
#define DPP_EVENT_CONFIGURATOR_ID                 "DPP-CONFIGURATOR-ID "                        // DPP配置器标识
#define DPP_EVENT_RX                              "DPP-RX "                                     // 收到DPP帧
#define DPP_EVENT_TX                              "DPP-TX "                                     // 发送DPP帧
#define DPP_EVENT_TX_STATUS                       "DPP-TX-STATUS "                              // DPP帧发送状态
#define DPP_EVENT_FAIL                            "DPP-FAIL "                                   // DPP操作失败
#define DPP_EVENT_PKEX_T_LIMIT                    "DPP-PKEX-T-LIMIT "                           // DPP PKEX尝试次数达到上限
#define DPP_EVENT_INTRO                           "DPP-INTRO "                                  // DPP对端引介结果
#define DPP_EVENT_CONF_REQ_RX                     "DPP-CONF-REQ-RX "                            // 收到DPP配置请求
#define DPP_EVENT_CHIRP_STOPPED                   "DPP-CHIRP-STOPPED "                          // DPP Chirp已停止
#define DPP_EVENT_MUD_URL                         "DPP-MUD-URL "                                // DPP MUD地址
#define DPP_EVENT_BAND_SUPPORT                    "DPP-BAND-SUPPORT "                           // DPP频段支持信息

/****************************** Mesh事件 ******************************/

#define MESH_GROUP_STARTED                        "MESH-GROUP-STARTED "                         // Mesh网络组已启动
#define MESH_GROUP_REMOVED                        "MESH-GROUP-REMOVED "                         // Mesh网络组已移除
#define MESH_PEER_CONNECTED                       "MESH-PEER-CONNECTED "                        // Mesh对端已连接
#define MESH_PEER_DISCONNECTED                    "MESH-PEER-DISCONNECTED "                     // Mesh对端已断开
#define MESH_SAE_AUTH_FAILURE                     "MESH-SAE-AUTH-FAILURE "                      // Mesh SAE认证失败，可能密码错误
#define MESH_SAE_AUTH_BLOCKED                     "MESH-SAE-AUTH-BLOCKED "                      // Mesh SAE认证被阻止

/****************************** WMM接入类别事件 ******************************/

#define WMM_AC_EVENT_TSPEC_ADDED                  "TSPEC-ADDED "                                // WMM TSPEC已添加
#define WMM_AC_EVENT_TSPEC_REMOVED                "TSPEC-REMOVED "                              // WMM TSPEC已移除
#define WMM_AC_EVENT_TSPEC_REQ_FAILED             "TSPEC-REQ-FAILED "                           // WMM TSPEC请求失败

/****************************** P2P事件 ******************************/

#define P2P_EVENT_DEVICE_FOUND                    "P2P-DEVICE-FOUND "                           // 发现P2P设备
#define P2P_EVENT_DEVICE_LOST                     "P2P-DEVICE-LOST "                            // P2P设备丢失
#define P2P_EVENT_GO_NEG_REQUEST                  "P2P-GO-NEG-REQUEST "                         // 收到P2P GO协商请求
#define P2P_EVENT_GO_NEG_SUCCESS                  "P2P-GO-NEG-SUCCESS "                         // P2P GO协商成功
#define P2P_EVENT_GO_NEG_FAILURE                  "P2P-GO-NEG-FAILURE "                         // P2P GO协商失败
#define P2P_EVENT_GROUP_FORMATION_SUCCESS         "P2P-GROUP-FORMATION-SUCCESS "                // P2P组建成功
#define P2P_EVENT_GROUP_FORMATION_FAILURE         "P2P-GROUP-FORMATION-FAILURE "                // P2P组建失败
#define P2P_EVENT_GROUP_STARTED                   "P2P-GROUP-STARTED "                          // P2P组已启动
#define P2P_EVENT_GROUP_REMOVED                   "P2P-GROUP-REMOVED "                          // P2P组已移除
#define P2P_EVENT_CROSS_CONNECT_ENABLE            "P2P-CROSS-CONNECT-ENABLE "                   // P2P跨连接已启用
#define P2P_EVENT_CROSS_CONNECT_DISABLE           "P2P-CROSS-CONNECT-DISABLE "                  // P2P跨连接已禁用
#define P2P_EVENT_PROV_DISC_SHOW_PIN              "P2P-PROV-DISC-SHOW-PIN "                     // P2P配置发现显示PIN
#define P2P_EVENT_PROV_DISC_ENTER_PIN             "P2P-PROV-DISC-ENTER-PIN "                    // P2P配置发现请求输入PIN
#define P2P_EVENT_PROV_DISC_PBC_REQ               "P2P-PROV-DISC-PBC-REQ "                      // 收到P2P PBC配置发现请求
#define P2P_EVENT_PROV_DISC_PBC_RESP              "P2P-PROV-DISC-PBC-RESP "                     // 收到P2P PBC配置发现响应
#define P2P_EVENT_PROV_DISC_FAILURE               "P2P-PROV-DISC-FAILURE"                       // P2P配置发现失败
#define P2P_EVENT_SERV_DISC_REQ                   "P2P-SERV-DISC-REQ "                          // 收到P2P服务发现请求
#define P2P_EVENT_SERV_DISC_RESP                  "P2P-SERV-DISC-RESP "                         // 收到P2P服务发现响应
#define P2P_EVENT_SERV_ASP_RESP                   "P2P-SERV-ASP-RESP "                          // 收到P2P ASP服务响应
#define P2P_EVENT_INVITATION_RECEIVED             "P2P-INVITATION-RECEIVED "                    // 收到P2P邀请
#define P2P_EVENT_INVITATION_RESULT               "P2P-INVITATION-RESULT "                      // P2P邀请结果
#define P2P_EVENT_INVITATION_ACCEPTED             "P2P-INVITATION-ACCEPTED "                    // P2P邀请已接受
#define P2P_EVENT_FIND_STOPPED                    "P2P-FIND-STOPPED "                           // P2P设备发现已停止
#define P2P_EVENT_PERSISTENT_PSK_FAIL             "P2P-PERSISTENT-PSK-FAIL id="                 // P2P持久组PSK失败，后接网络ID
#define P2P_EVENT_PRESENCE_RESPONSE               "P2P-PRESENCE-RESPONSE "                      // 收到P2P存在请求响应
#define P2P_EVENT_NFC_BOTH_GO                     "P2P-NFC-BOTH-GO "                            // NFC交互中双方均为GO
#define P2P_EVENT_NFC_PEER_CLIENT                 "P2P-NFC-PEER-CLIENT "                        // NFC对端为P2P客户端
#define P2P_EVENT_NFC_WHILE_CLIENT                "P2P-NFC-WHILE-CLIENT "                       // 本机作为客户端时发生NFC交互
#define P2P_EVENT_FALLBACK_TO_GO_NEG              "P2P-FALLBACK-TO-GO-NEG "                     // 回退到P2P GO协商
#define P2P_EVENT_FALLBACK_TO_GO_NEG_ENABLED      "P2P-FALLBACK-TO-GO-NEG-ENABLED "             // 已启用回退到P2P GO协商
#define ESS_DISASSOC_IMMINENT                     "ESS-DISASSOC-IMMINENT "                      // ESS即将解除关联
#define P2P_EVENT_REMOVE_AND_REFORM_GROUP         "P2P-REMOVE-AND-REFORM-GROUP "                // 移除并重新组建P2P组
#define P2P_EVENT_P2PS_PROVISION_START            "P2PS-PROV-START "                            // P2PS配置开始
#define P2P_EVENT_P2PS_PROVISION_DONE             "P2PS-PROV-DONE "                             // P2PS配置完成

/****************************** 互联与凭据事件 ******************************/

#define INTERWORKING_AP                           "INTERWORKING-AP "                            // 发现互联接入点
#define INTERWORKING_BLACKLISTED                  "INTERWORKING-BLACKLISTED "                   // 互联网络已加入黑名单
#define INTERWORKING_NO_MATCH                     "INTERWORKING-NO-MATCH "                      // 未找到匹配的互联网络
#define INTERWORKING_ALREADY_CONNECTED            "INTERWORKING-ALREADY-CONNECTED "             // 已连接目标互联网络
#define INTERWORKING_SELECTED                     "INTERWORKING-SELECTED "                      // 已选择互联网络
#define CRED_ADDED                                "CRED-ADDED "                                 // 凭据块已添加，后接凭据ID
#define CRED_MODIFIED                             "CRED-MODIFIED "                              // 凭据块已修改，后接凭据ID和字段
#define CRED_REMOVED                              "CRED-REMOVED "                               // 凭据块已移除，后接凭据ID

/****************************** GAS与ANQP事件 ******************************/

#define GAS_RESPONSE_INFO                         "GAS-RESPONSE-INFO "                          // GAS响应信息
#define GAS_QUERY_START                           "GAS-QUERY-START "                            // GAS查询开始
#define GAS_QUERY_DONE                            "GAS-QUERY-DONE "                             // GAS查询完成
#define ANQP_QUERY_DONE                           "ANQP-QUERY-DONE "                            // ANQP查询完成
#define RX_ANQP                                   "RX-ANQP "                                    // 收到ANQP数据
#define RX_HS20_ANQP                              "RX-HS20-ANQP "                               // 收到Hotspot 2.0 ANQP数据
#define RX_HS20_ANQP_ICON                         "RX-HS20-ANQP-ICON "                          // 收到Hotspot 2.0 ANQP图标信息
#define RX_HS20_ICON                              "RX-HS20-ICON "                               // 收到Hotspot 2.0图标
#define RX_MBO_ANQP                               "RX-MBO-ANQP "                                // 收到MBO ANQP数据
#define RX_VENUE_URL                              "RX-VENUE-URL "                               // 收到场所URL

/****************************** Hotspot 2.0与射频事件 ******************************/

#define HS20_SUBSCRIPTION_REMEDIATION             "HS20-SUBSCRIPTION-REMEDIATION "              // Hotspot 2.0订阅修复通知
#define HS20_DEAUTH_IMMINENT_NOTICE               "HS20-DEAUTH-IMMINENT-NOTICE "                // Hotspot 2.0即将解除认证通知
#define HS20_T_C_ACCEPTANCE                       "HS20-T-C-ACCEPTANCE "                        // Hotspot 2.0条款接受通知
#define EXT_RADIO_WORK_START                      "EXT-RADIO-WORK-START "                       // 外部射频任务开始
#define EXT_RADIO_WORK_TIMEOUT                    "EXT-RADIO-WORK-TIMEOUT "                     // 外部射频任务超时
#define RRM_EVENT_NEIGHBOR_REP_RXED               "RRM-NEIGHBOR-REP-RECEIVED "                  // 收到RRM邻居报告
#define RRM_EVENT_NEIGHBOR_REP_FAILED             "RRM-NEIGHBOR-REP-REQUEST-FAILED "            // RRM邻居报告请求失败

/****************************** hostapd WPS与STA事件 ******************************/

#define WPS_EVENT_PIN_NEEDED                      "WPS-PIN-NEEDED "                             // 需要WPS PIN
#define WPS_EVENT_NEW_AP_SETTINGS                 "WPS-NEW-AP-SETTINGS "                        // 收到新的WPS接入点配置
#define WPS_EVENT_REG_SUCCESS                     "WPS-REG-SUCCESS "                            // WPS注册成功
#define WPS_EVENT_AP_SETUP_LOCKED                 "WPS-AP-SETUP-LOCKED "                        // 接入点WPS配置已锁定
#define WPS_EVENT_AP_SETUP_UNLOCKED               "WPS-AP-SETUP-UNLOCKED "                      // 接入点WPS配置已解锁
#define WPS_EVENT_AP_PIN_ENABLED                  "WPS-AP-PIN-ENABLED "                         // 接入点WPS PIN已启用
#define WPS_EVENT_AP_PIN_DISABLED                 "WPS-AP-PIN-DISABLED "                        // 接入点WPS PIN已禁用
#define WPS_EVENT_PIN_ACTIVE                      "WPS-PIN-ACTIVE "                             // WPS PIN处于活动状态
#define WPS_EVENT_CANCEL                          "WPS-CANCEL "                                 // WPS操作已取消
#define AP_STA_CONNECTED                          "AP-STA-CONNECTED "                           // STA已连接接入点
#define AP_STA_DISCONNECTED                       "AP-STA-DISCONNECTED "                        // STA已断开接入点
#define AP_STA_POSSIBLE_PSK_MISMATCH              "AP-STA-POSSIBLE-PSK-MISMATCH "               // STA可能存在PSK不匹配
#define AP_STA_POLL_OK                            "AP-STA-POLL-OK "                             // STA轮询响应正常
#define AP_REJECTED_MAX_STA                       "AP-REJECTED-MAX-STA "                        // 因达到最大STA数量拒绝连接
#define AP_REJECTED_BLOCKED_STA                   "AP-REJECTED-BLOCKED-STA "                    // 因STA被阻止而拒绝连接

/****************************** hostapd接口与信道事件 ******************************/

#define HS20_T_C_FILTERING_ADD                    "HS20-T-C-FILTERING-ADD "                     // 添加Hotspot 2.0条款过滤
#define HS20_T_C_FILTERING_REMOVE                 "HS20-T-C-FILTERING-REMOVE "                  // 移除Hotspot 2.0条款过滤
#define AP_EVENT_ENABLED                          "AP-ENABLED "                                 // 接入点已启用
#define AP_EVENT_DISABLED                         "AP-DISABLED "                                // 接入点已禁用
#define INTERFACE_ENABLED                         "INTERFACE-ENABLED "                          // 无线接口已启用
#define INTERFACE_DISABLED                        "INTERFACE-DISABLED "                         // 无线接口已禁用
#define ACS_EVENT_STARTED                         "ACS-STARTED "                                // 自动信道选择开始
#define ACS_EVENT_COMPLETED                       "ACS-COMPLETED "                              // 自动信道选择完成
#define ACS_EVENT_FAILED                          "ACS-FAILED "                                 // 自动信道选择失败
#define DFS_EVENT_RADAR_DETECTED                  "DFS-RADAR-DETECTED "                         // DFS检测到雷达
#define DFS_EVENT_NEW_CHANNEL                     "DFS-NEW-CHANNEL "                            // DFS选择新信道
#define DFS_EVENT_CAC_START                       "DFS-CAC-START "                              // DFS信道可用性检查开始
#define DFS_EVENT_CAC_COMPLETED                   "DFS-CAC-COMPLETED "                          // DFS信道可用性检查完成
#define DFS_EVENT_NOP_FINISHED                    "DFS-NOP-FINISHED "                           // DFS非占用周期结束
#define DFS_EVENT_PRE_CAC_EXPIRED                 "DFS-PRE-CAC-EXPIRED "                        // DFS预CAC结果已过期
#define AP_CSA_FINISHED                           "AP-CSA-FINISHED "                            // 接入点信道切换公告完成
#define P2P_EVENT_LISTEN_OFFLOAD_STOP             "P2P-LISTEN-OFFLOAD-STOPPED "                 // P2P监听卸载已停止
#define P2P_LISTEN_OFFLOAD_STOP_REASON            "P2P-LISTEN-OFFLOAD-STOP-REASON "             // P2P监听卸载停止原因

/****************************** 管理与测量事件 ******************************/

#define BSS_TM_RESP                               "BSS-TM-RESP "                                // 收到BSS过渡管理响应
#define COLOC_INTF_REQ                            "COLOC-INTF-REQ "                             // 收到同址干扰请求
#define COLOC_INTF_REPORT                         "COLOC-INTF-REPORT "                          // 收到同址干扰报告
#define MBO_CELL_PREFERENCE                       "MBO-CELL-PREFERENCE "                        // 收到包含蜂窝数据偏好的MBO信息元素
#define MBO_TRANSITION_REASON                     "MBO-TRANSITION-REASON "                      // 收到包含MBO切换原因的BSS过渡请求
#define BEACON_REQ_TX_STATUS                      "BEACON-REQ-TX-STATUS "                       // 信标测量请求发送状态
#define BEACON_RESP_RX                            "BEACON-RESP-RX "                             // 收到信标测量响应
#define PMKSA_CACHE_ADDED                         "PMKSA-CACHE-ADDED "                          // PMKSA缓存条目已添加
#define PMKSA_CACHE_REMOVED                       "PMKSA-CACHE-REMOVED "                        // PMKSA缓存条目已移除
#define FILS_HLP_RX                               "FILS-HLP-RX "                                // 收到FILS HLP容器
#define RX_PROBE_REQUEST                          "RX-PROBE-REQUEST "                           // 收到Probe Request帧
#define STA_OPMODE_MAX_BW_CHANGED                 "STA-OPMODE-MAX-BW-CHANGED "                  // STA最大工作带宽发生变化
#define STA_OPMODE_SMPS_MODE_CHANGED              "STA-OPMODE-SMPS-MODE-CHANGED "               // STA SMPS模式发生变化
#define STA_OPMODE_N_SS_CHANGED                   "STA-OPMODE-N_SS-CHANGED "                    // STA空间流数量发生变化
#define WDS_STA_INTERFACE_ADDED                   "WDS-STA-INTERFACE-ADDED "                    // 新增4地址WDS STA接口
#define WDS_STA_INTERFACE_REMOVED                 "WDS-STA-INTERFACE-REMOVED "                  // 移除4地址WDS STA接口
#define TRANSITION_DISABLE                        "TRANSITION-DISABLE "                         // 过渡模式禁用指示
#define OCV_FAILURE                               "OCV-FAILURE "                                // OCV校验失败
#define AP_MGMT_FRAME_RECEIVED                    "AP-MGMT-FRAME-RECEIVED "                     // 接入点收到管理帧

/****************************** 位操作 ******************************/

#ifndef BIT
#define BIT(x)                                    (1U << (x))                                   // 生成指定位的无符号掩码
#endif

/****************************** BSS信息掩码 ******************************/

#define WPA_BSS_MASK_ALL                          0xFFFDFFFF                                    // 返回全部BSS信息
#define WPA_BSS_MASK_ID                           BIT(0)                                        // 返回BSS条目标识
#define WPA_BSS_MASK_BSSID                        BIT(1)                                        // 返回BSSID
#define WPA_BSS_MASK_FREQ                         BIT(2)                                        // 返回工作频率
#define WPA_BSS_MASK_BEACON_INT                   BIT(3)                                        // 返回信标间隔
#define WPA_BSS_MASK_CAPABILITIES                 BIT(4)                                        // 返回能力字段
#define WPA_BSS_MASK_QUAL                         BIT(5)                                        // 返回链路质量
#define WPA_BSS_MASK_NOISE                        BIT(6)                                        // 返回噪声强度
#define WPA_BSS_MASK_LEVEL                        BIT(7)                                        // 返回信号强度
#define WPA_BSS_MASK_TSF                          BIT(8)                                        // 返回TSF时间
#define WPA_BSS_MASK_AGE                          BIT(9)                                        // 返回扫描条目年龄
#define WPA_BSS_MASK_IE                           BIT(10)                                       // 返回信息元素
#define WPA_BSS_MASK_FLAGS                        BIT(11)                                       // 返回状态标志
#define WPA_BSS_MASK_SSID                         BIT(12)                                       // 返回SSID
#define WPA_BSS_MASK_WPS_SCAN                     BIT(13)                                       // 返回WPS扫描信息
#define WPA_BSS_MASK_P2P_SCAN                     BIT(14)                                       // 返回P2P扫描信息
#define WPA_BSS_MASK_INTERNETW                    BIT(15)                                       // 返回互联网络信息
#define WPA_BSS_MASK_WIFI_DISPLAY                 BIT(16)                                       // 返回Wi-Fi Display信息
#define WPA_BSS_MASK_DELIM                        BIT(17)                                       // 返回条目分隔符
#define WPA_BSS_MASK_MESH_SCAN                    BIT(18)                                       // 返回Mesh扫描信息
#define WPA_BSS_MASK_SNR                          BIT(19)                                       // 返回信噪比
#define WPA_BSS_MASK_EST_THROUGHPUT               BIT(20)                                       // 返回估算吞吐量
#define WPA_BSS_MASK_FST                          BIT(21)                                       // 返回快速会话迁移信息
#define WPA_BSS_MASK_UPDATE_IDX                   BIT(22)                                       // 返回更新索引
#define WPA_BSS_MASK_BEACON_IE                    BIT(23)                                       // 返回信标信息元素
#define WPA_BSS_MASK_FILS_INDICATION              BIT(24)                                       // 返回FILS指示信息

/****************************** 厂商元素帧类型 ******************************/

enum wpa_vendor_elem_frame
{
    VENDOR_ELEM_PROBE_REQ_P2P     = 0,  // P2P Probe Request帧
    VENDOR_ELEM_PROBE_RESP_P2P    = 1,  // P2P Probe Response帧
    VENDOR_ELEM_PROBE_RESP_P2P_GO = 2,  // P2P GO Probe Response帧
    VENDOR_ELEM_BEACON_P2P_GO     = 3,  // P2P GO Beacon帧
    VENDOR_ELEM_P2P_PD_REQ        = 4,  // P2P配置发现请求帧
    VENDOR_ELEM_P2P_PD_RESP       = 5,  // P2P配置发现响应帧
    VENDOR_ELEM_P2P_GO_NEG_REQ    = 6,  // P2P GO协商请求帧
    VENDOR_ELEM_P2P_GO_NEG_RESP   = 7,  // P2P GO协商响应帧
    VENDOR_ELEM_P2P_GO_NEG_CONF   = 8,  // P2P GO协商确认帧
    VENDOR_ELEM_P2P_INV_REQ       = 9,  // P2P邀请请求帧
    VENDOR_ELEM_P2P_INV_RESP      = 10, // P2P邀请响应帧
    VENDOR_ELEM_P2P_ASSOC_REQ     = 11, // P2P关联请求帧
    VENDOR_ELEM_P2P_ASSOC_RESP    = 12, // P2P关联响应帧
    VENDOR_ELEM_ASSOC_REQ         = 13, // 普通关联请求帧
    VENDOR_ELEM_PROBE_REQ         = 14, // 普通Probe Request帧
    NUM_VENDOR_ELEM_FRAMES              // 厂商元素帧类型数量
};

/****************************** 控制接口 ******************************/

struct wpa_ctrl;

struct wpa_ctrl *wpa_ctrl_open(const char *ctrl_path);

struct wpa_ctrl *wpa_ctrl_open2(const char *ctrl_path, const char *cli_path);

void             wpa_ctrl_close(struct wpa_ctrl *ctrl);

int              wpa_ctrl_request(struct wpa_ctrl *ctrl, const char *cmd, size_t cmd_len, char *reply, size_t *reply_len, void (*msg_cb)(char *msg, size_t len));

int              wpa_ctrl_attach(struct wpa_ctrl *ctrl);

int              wpa_ctrl_detach(struct wpa_ctrl *ctrl);

int              wpa_ctrl_recv(struct wpa_ctrl *ctrl, char *reply, size_t *reply_len);

int              wpa_ctrl_pending(struct wpa_ctrl *ctrl);

int              wpa_ctrl_get_fd(struct wpa_ctrl *ctrl);

#ifdef ANDROID
void             wpa_ctrl_cleanup(void);
#endif

#ifdef CONFIG_CTRL_IFACE_UDP

/****************************** UDP控制接口 ******************************/

#define WPA_CTRL_IFACE_PORT                       9877                                          // wpa_supplicant控制接口起始端口
#define WPA_CTRL_IFACE_PORT_LIMIT                 50                                            // wpa_supplicant控制接口端口数量上限
#define WPA_GLOBAL_CTRL_IFACE_PORT                9878                                          // 全局控制接口起始端口
#define WPA_GLOBAL_CTRL_IFACE_PORT_LIMIT          20                                            // 全局控制接口端口数量上限

char *wpa_ctrl_get_remote_ifname(struct wpa_ctrl *ctrl);

#endif

#ifdef __cplusplus
}
#endif

#endif

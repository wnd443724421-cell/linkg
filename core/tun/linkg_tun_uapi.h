/**
 * @file linkg_tun_uapi.h
 * @brief LinkG TUN内核与用户空间私有ABI
 */

#ifndef LINKG_TUN_UAPI_H
#define LINKG_TUN_UAPI_H

#include <linux/types.h>
#include <sys/ioctl.h>

/****************************** 批量配置 ******************************/

#define LQ_TUN_BATCH_MAX                      1024U                                         // 单次批量操作最大报文数量
#define LQ_TUN_BATCH_TIMEOUT_MAX_US           1000U                                        // 批量读取最大聚合等待时间，单位微秒

/****************************** 业务分类 ******************************/

#define LQ_TUN_TRAFFIC_CLASS_REALTIME         0U                                            // 实时业务分类
#define LQ_TUN_TRAFFIC_CLASS_VIDEO            1U                                            // 视频业务分类
#define LQ_TUN_TRAFFIC_CLASS_DATA             2U                                            // 普通数据业务分类
#define LQ_TUN_TRAFFIC_CLASS_COUNT            3U                                            // 业务分类总数量

#define LQ_TUN_REALTIME_QUEUE_SIZE            128U                                          // 实时业务队列容量
#define LQ_TUN_VIDEO_QUEUE_SIZE               256U                                          // 视频业务队列容量
#define LQ_TUN_DATA_QUEUE_SIZE                512U                                          // 普通数据业务队列容量

/****************************** 动态分类规则 ******************************/

#define LQ_TUN_TRAFFIC_RULE_MAX               16U                                           // 动态业务分类规则最大数量
#define LQ_TUN_PORT_PROTOCOL_TCP              6U                                            // TCP协议号
#define LQ_TUN_PORT_PROTOCOL_UDP              17U                                           // UDP协议号

struct lq_tun_traffic_rule
{
    __u8  traffic_class; // 目标业务分类
    __u8  protocol;      // TCP或UDP协议号
    __u16 start_port;    // 匹配端口范围起始值
    __u16 end_port;      // 匹配端口范围结束值
    __u16 reserved;      // 保留字段，必须为0
};

struct lq_tun_traffic_config
{
    __u32                      count;                           // 有效规则数量
    __u32                      reserved;                        // 保留字段，必须为0
    struct lq_tun_traffic_rule rules[LQ_TUN_TRAFFIC_RULE_MAX]; // 动态分类规则
};

/****************************** 批量报文 ******************************/

struct lq_tun_batch_entry
{
    __aligned_u64 data;     // 用户空间数据缓冲区地址
    __u32         length;   // 实际数据长度
    __u32         capacity; // 缓冲区容量
};

struct lq_tun_batch_read
{
    __aligned_u64 entries;       // 批量条目数组地址
    __u32         traffic_class; // 指定读取的业务分类
    __u32         max_pkts;      // 最大读取包数
    __u32         min_pkts;      // 最小聚合包数
    __u32         timeout_us;    // 最大聚合等待时间
    __u32         read_pkts;     // 实际读取包数
    __u32         read_bytes;    // 实际读取字节数
    __s32         status;        // 内核操作状态
};

struct lq_tun_batch_write
{
    __aligned_u64 entries;       // 批量条目数组地址
    __u32         pkt_count;     // 请求写入包数
    __u32         written_pkts;  // 实际写入包数
    __u32         written_bytes; // 实际写入字节数
    __s32         status;        // 内核操作状态
};

/****************************** 私有 IOCTL ******************************/

#define LQ_TUN_IOC_READ_BATCH                 _IOWR('T', 240, struct lq_tun_batch_read)    // 批量读取指定业务分类
#define LQ_TUN_IOC_WRITE_BATCH                _IOWR('T', 241, struct lq_tun_batch_write)   // 批量写入TUN
#define LQ_TUN_IOC_SET_TRAFFIC_CONFIG         _IOW('T', 242, struct lq_tun_traffic_config) // 更新动态业务分类规则

#endif

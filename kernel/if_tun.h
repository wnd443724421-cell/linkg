/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/**
 * Universal TUN/TAP 设备驱动
 *
 * Copyright (C) 1999-2000 Maxim Krasnyansky <max_mk@yahoo.com>
 *
 * 本程序是自由软件；你可以根据自由软件基金会发布的
 * GNU 通用公共许可证条款重新发布和/或修改本程序，
 * 许可证版本可以选择第 2 版或任何后续版本。
 *
 * 本程序的发布是希望它能够有用，但不提供任何担保；
 * 甚至不提供适销性或特定用途适用性的默示担保。
 * 详细内容请参阅 GNU 通用公共许可证。
 */

#ifndef _UAPI__IF_TUN_H
#define _UAPI__IF_TUN_H

#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/types.h>

/****************************** TUN 基础定义 ******************************/

#define TUN_READQ_SIZE                        500                                                // TUN默认读取队列容量
#define TUN_TUN_DEV                           IFF_TUN                                            // TUN设备类型兼容标志，已废弃
#define TUN_TAP_DEV                           IFF_TAP                                            // TAP设备类型兼容标志，已废弃
#define TUN_TYPE_MASK                         0x000f                                             // TUN/TAP设备类型掩码

/****************************** IOCTL 接口 ******************************/

#define TUNSETNOCSUM                          _IOW('T', 200, int)                                // 设置是否禁用校验和处理
#define TUNSETDEBUG                           _IOW('T', 201, int)                                // 设置TUN调试级别
#define TUNSETIFF                             _IOW('T', 202, int)                                // 创建或连接TUN/TAP接口
#define TUNSETPERSIST                         _IOW('T', 203, int)                                // 设置接口持久化状态
#define TUNSETOWNER                           _IOW('T', 204, int)                                // 设置接口所属用户
#define TUNSETLINK                            _IOW('T', 205, int)                                // 设置接口链路类型
#define TUNSETGROUP                           _IOW('T', 206, int)                                // 设置接口所属用户组
#define TUNGETFEATURES                        _IOR('T', 207, unsigned int)                       // 获取TUN驱动支持的功能标志
#define TUNSETOFFLOAD                         _IOW('T', 208, unsigned int)                       // 设置TUN卸载功能
#define TUNSETTXFILTER                        _IOW('T', 209, unsigned int)                       // 设置TAP发送地址过滤器
#define TUNGETIFF                             _IOR('T', 210, unsigned int)                       // 获取当前接口配置
#define TUNGETSNDBUF                          _IOR('T', 211, int)                                // 获取Socket发送缓冲区大小
#define TUNSETSNDBUF                          _IOW('T', 212, int)                                // 设置Socket发送缓冲区大小
#define TUNATTACHFILTER                       _IOW('T', 213, struct sock_fprog)                  // 挂载Socket过滤器
#define TUNDETACHFILTER                       _IOW('T', 214, struct sock_fprog)                  // 卸载Socket过滤器
#define TUNGETVNETHDRSZ                       _IOR('T', 215, int)                                // 获取Virtio网络头长度
#define TUNSETVNETHDRSZ                       _IOW('T', 216, int)                                // 设置Virtio网络头长度
#define TUNSETQUEUE                           _IOW('T', 217, int)                                // 设置多队列附加或分离状态
#define TUNSETIFINDEX                         _IOW('T', 218, unsigned int)                       // 设置接口索引
#define TUNGETFILTER                          _IOR('T', 219, struct sock_fprog)                  // 获取当前Socket过滤器
#define TUNSETVNETLE                          _IOW('T', 220, int)                                // 设置Virtio网络头小端模式
#define TUNGETVNETLE                          _IOR('T', 221, int)                                // 获取Virtio网络头小端模式
#define TUNSETVNETBE                          _IOW('T', 222, int)                                // 设置Virtio网络头大端模式
#define TUNGETVNETBE                          _IOR('T', 223, int)                                // 获取Virtio网络头大端模式
#define TUNSETSTEERINGEBPF                    _IOR('T', 224, int)                                // 设置多队列流量导向eBPF程序
#define TUNSETFILTEREBPF                      _IOR('T', 225, int)                                // 设置接收过滤eBPF程序
#define TUNSETCARRIER                         _IOW('T', 226, int)                                // 设置接口Carrier状态
#define TUNGETDEVNETNS                        _IO('T', 227)                                      // 获取接口所在网络命名空间

/****************************** LQ 批量配置 ******************************/

#define LQ_TUN_BATCH_MAX                      1024                                               // 单次批量操作允许处理的最大报文数量
#define LQ_TUN_BATCH_TIMEOUT_MAX_US           1000U                                              // 批量读取最大聚合等待时间，单位微秒

/****************************** LQ 业务分类 ******************************/

#define LQ_TUN_TRAFFIC_CLASS_REALTIME         0U                                                 // 实时业务分类
#define LQ_TUN_TRAFFIC_CLASS_VIDEO            1U                                                 // 视频业务分类
#define LQ_TUN_TRAFFIC_CLASS_DATA             2U                                                 // 普通数据业务分类
#define LQ_TUN_TRAFFIC_CLASS_COUNT            3U                                                 // 业务分类总数量

#define LQ_TUN_REALTIME_QUEUE_SIZE            128U                                               // 实时业务固定队列容量
#define LQ_TUN_VIDEO_QUEUE_SIZE               256U                                               // 视频业务固定队列容量
#define LQ_TUN_DATA_QUEUE_SIZE                512U                                               // 普通数据业务固定队列容量

/****************************** LQ 动态分类规则 ******************************/

#define LQ_TUN_TRAFFIC_RULE_MAX               16U                                                // 动态业务分类规则最大数量
#define LQ_TUN_PORT_PROTOCOL_TCP              6U                                                 // TCP对应的IPv4协议号
#define LQ_TUN_PORT_PROTOCOL_UDP              17U                                                // UDP对应的IPv4协议号

struct lq_tun_traffic_rule
{
    __u8  traffic_class; // 规则命中的目标业务分类，使用LQ_TUN_TRAFFIC_CLASS_*
    __u8  protocol;      // IP传输层协议号，当前支持TCP和UDP
    __u16 start_port;    // 匹配端口范围起始值，包含该端口
    __u16 end_port;      // 匹配端口范围结束值，包含该端口
    __u16 reserved;      // 保留字段，设置时必须为0
};

struct lq_tun_traffic_config
{
    __u32                      count;                               // 当前有效业务分类规则数量
    __u32                      reserved;                            // 保留字段，设置时必须为0
    struct lq_tun_traffic_rule rules[LQ_TUN_TRAFFIC_RULE_MAX];     // 动态业务分类规则数组
};

/****************************** LQ 批量报文 ******************************/

/**
 * 单个批量报文描述符。
 *
 * data:
 *   输入，用户空间报文缓冲区地址。
 *
 * length:
 *   批量读取时输出实际读取的报文长度。
 *   批量写入时输入需要写入的报文长度。
 *
 * capacity:
 *   输入，data指向的用户空间缓冲区总容量。
 */
struct lq_tun_batch_entry
{
    __aligned_u64 data;     // 用户空间报文缓冲区地址
    __u32         length;   // 读取时为实际报文长度，写入时为待写入报文长度
    __u32         capacity; // data指向的用户空间缓冲区总容量
};

/**
 * 批量读取参数。
 *
 * 首个报文读取成功后，可以继续等待同一业务分类的后续报文。
 * 达到min_pkts后即可结束聚合，不要求必须达到max_pkts；
 * 等待时间不得超过timeout_us。
 */
struct lq_tun_batch_read
{
    __aligned_u64 entries;       // struct lq_tun_batch_entry[max_pkts]用户空间数组地址
    __u32         traffic_class; // 需要读取的业务分类，使用LQ_TUN_TRAFFIC_CLASS_*
    __u32         max_pkts;      // 本次调用最多读取的报文数量
    __u32         min_pkts;      // 本次批量聚合的最小目标报文数量
    __u32         timeout_us;    // 首包成功后的最大聚合等待时间，单位微秒
    __u32         read_pkts;     // 本次实际成功读取的报文数量
    __u32         read_bytes;    // 本次实际成功读取的报文字节总数
    __s32         status;        // 批量操作状态，0表示成功，负值表示Linux errno
};

/**
 * 批量写入参数。
 *
 * entries指向pkt_count个批量报文描述符。
 * written_pkts和written_bytes返回本次实际完成的写入结果。
 */
struct lq_tun_batch_write
{
    __aligned_u64 entries;       // struct lq_tun_batch_entry[pkt_count]用户空间数组地址
    __u32         pkt_count;     // 本次需要写入的报文数量
    __u32         written_pkts;  // 本次实际成功写入的报文数量
    __u32         written_bytes; // 本次实际成功写入的报文字节总数
    __s32         status;        // 批量操作状态，0表示成功，负值表示Linux errno
};

/****************************** LQ 私有 IOCTL ******************************/

#define LQ_TUN_IOC_READ_BATCH                 _IOWR('T', 240, struct lq_tun_batch_read)          // 从指定业务分类队列批量读取报文
#define LQ_TUN_IOC_WRITE_BATCH                _IOWR('T', 241, struct lq_tun_batch_write)         // 向TUN设备批量写入报文
#define LQ_TUN_IOC_SET_TRAFFIC_CONFIG         _IOW('T', 242, struct lq_tun_traffic_config)       // 动态替换后续新入队报文使用的业务分类规则

/****************************** TUNSETIFF 标志 ******************************/

#define IFF_TUN                                0x0001                                             // 创建三层TUN设备
#define IFF_TAP                                0x0002                                             // 创建二层TAP设备
#define IFF_NAPI                               0x0010                                             // 启用NAPI接收处理
#define IFF_NAPI_FRAGS                         0x0020                                             // 启用NAPI分片接收处理
#define IFF_NO_PI                              0x1000                                             // 不在报文前附加tun_pi协议头
#define IFF_ONE_QUEUE                          0x2000                                             // 单队列兼容标志，当前无实际作用
#define IFF_VNET_HDR                           0x4000                                             // 启用Virtio网络头
#define IFF_TUN_EXCL                           0x8000                                             // 要求TUNSETIFF创建独占设备
#define IFF_MULTI_QUEUE                        0x0100                                             // 启用TUN/TAP多队列
#define IFF_ATTACH_QUEUE                       0x0200                                             // 将文件描述符附加到现有队列
#define IFF_DETACH_QUEUE                       0x0400                                             // 将文件描述符从现有队列分离
#define IFF_PERSIST                            0x0800                                             // 接口当前处于持久化状态，只读标志
#define IFF_NOFILTER                           0x1000                                             // 当前未启用TAP地址过滤，只读标志

/****************************** Socket 选项 ******************************/

#define TUN_TX_TIMESTAMP                       1                                                  // TUN发送时间戳Socket选项

/****************************** GSO 卸载特性 ******************************/

#define TUN_F_CSUM                             0x01                                               // 允许提交尚未计算完整校验和的报文
#define TUN_F_TSO4                             0x02                                               // 支持IPv4 TCP Segmentation Offload
#define TUN_F_TSO6                             0x04                                               // 支持IPv6 TCP Segmentation Offload
#define TUN_F_TSO_ECN                          0x08                                               // TCP Segmentation Offload支持ECN
#define TUN_F_UFO                              0x10                                               // 支持UDP Fragmentation Offload

/****************************** TUN 报文协议头 ******************************/

#define TUN_PKT_STRIP                          0x0001                                             // 标记报文发生截断

struct tun_pi
{
    __u16  flags; // TUN报文标志，使用TUN_PKT_*
    __be16 proto; // 报文网络层协议类型，采用网络字节序
};

/****************************** TAP 地址过滤 ******************************/

#define TUN_FLT_ALLMULTI                       0x0001                                             // TAP地址过滤器接收全部组播报文

/**
 * TAP地址过滤配置。
 *
 * count为0时关闭地址过滤器并接受全部报文。
 * 启用过滤后，如果需要接收广播报文，必须将广播MAC地址加入addr数组。
 */
struct tun_filter
{
    __u16 flags;            // 地址过滤标志，使用TUN_FLT_*
    __u16 count;            // addr数组中有效MAC地址数量
    __u8  addr[0][ETH_ALEN]; // 允许接收的MAC地址数组
};

#endif // _UAPI__IF_TUN_H

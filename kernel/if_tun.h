/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
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

#include <linux/types.h>
#include <linux/if_ether.h>
#include <linux/filter.h>

/* TUN 读取队列大小 */
#define TUN_READQ_SIZE 500

/* TUN 设备类型标志：已废弃，请使用 IFF_TUN/IFF_TAP */
#define TUN_TUN_DEV   IFF_TUN
#define TUN_TAP_DEV   IFF_TAP
#define TUN_TYPE_MASK 0x000f

/****************************** IOCTL 接口 ******************************/

#define TUNSETNOCSUM         _IOW('T', 200, int)
#define TUNSETDEBUG          _IOW('T', 201, int)
#define TUNSETIFF            _IOW('T', 202, int)
#define TUNSETPERSIST        _IOW('T', 203, int)
#define TUNSETOWNER          _IOW('T', 204, int)
#define TUNSETLINK           _IOW('T', 205, int)
#define TUNSETGROUP          _IOW('T', 206, int)
#define TUNGETFEATURES       _IOR('T', 207, unsigned int)
#define TUNSETOFFLOAD        _IOW('T', 208, unsigned int)
#define TUNSETTXFILTER       _IOW('T', 209, unsigned int)
#define TUNGETIFF            _IOR('T', 210, unsigned int)
#define TUNGETSNDBUF         _IOR('T', 211, int)
#define TUNSETSNDBUF         _IOW('T', 212, int)
#define TUNATTACHFILTER      _IOW('T', 213, struct sock_fprog)
#define TUNDETACHFILTER      _IOW('T', 214, struct sock_fprog)
#define TUNGETVNETHDRSZ      _IOR('T', 215, int)
#define TUNSETVNETHDRSZ      _IOW('T', 216, int)
#define TUNSETQUEUE          _IOW('T', 217, int)
#define TUNSETIFINDEX        _IOW('T', 218, unsigned int)
#define TUNGETFILTER         _IOR('T', 219, struct sock_fprog)
#define TUNSETVNETLE         _IOW('T', 220, int)
#define TUNGETVNETLE         _IOR('T', 221, int)

/*
 * TUNSETVNETBE/TUNGETVNETBE 用于小端主机上的跨端序支持。
 * 并非所有内核配置都支持 SET 接口，但支持 SET 的配置一定同时支持 GET。
 */
#define TUNSETVNETBE         _IOW('T', 222, int)
#define TUNGETVNETBE         _IOR('T', 223, int)
#define TUNSETSTEERINGEBPF   _IOR('T', 224, int)
#define TUNSETFILTEREBPF     _IOR('T', 225, int)
#define TUNSETCARRIER        _IOW('T', 226, int)
#define TUNGETDEVNETNS       _IO('T', 227)

/****************************** LQ 私有批量 I/O ******************************/

/* 单次批量操作允许处理的最大报文数量 */
#define LQ_TUN_BATCH_MAX 1024

/* 批量读取允许配置的最大聚合等待时间，单位：微秒 */
#define LQ_TUN_BATCH_TIMEOUT_MAX_US 1000U

/*
 * 单个批量报文描述符。
 *
 * data:
 *   输入，用户空间报文缓冲区地址。
 *
 * length:
 *   批量读取时：输出实际读取的报文长度。
 *   批量写入时：输入需要写入的报文长度。
 *
 * capacity:
 *   输入，data 指向的用户空间缓冲区总容量。
 */
struct lq_tun_batch_entry {
	__aligned_u64 data; /* 输入：用户空间报文缓冲区地址 */
	__u32 length;       /* 读取：输出报文长度；写入：输入报文长度 */
	__u32 capacity;     /* 输入：用户空间报文缓冲区容量 */
};

/*
 * 批量读取参数。
 *
 * entries:
 *   输入，指向 struct lq_tun_batch_entry[max_pkts] 数组。
 *
 * max_pkts:
 *   输入，本次调用最多读取的报文数量。
 *
 * min_pkts:
 *   输入，批量读取的最小目标报文数量。
 *   首个报文到达后，在达到该数量之前允许继续等待后续报文；
 *   达到该数量后即可结束本次聚合，不要求必须达到 max_pkts。
 *
 * timeout_us:
 *   输入，首个报文读取成功后允许继续聚合的最长时间，单位：微秒。
 *
 * read_pkts:
 *   输出，本次实际读取的报文数量。
 *
 * read_bytes:
 *   输出，本次实际读取的报文字节总数。
 *
 * status:
 *   输出，0 表示成功，负值表示 Linux errno。
 */
struct lq_tun_batch_read {
	__aligned_u64 entries; /* 输入：struct lq_tun_batch_entry[max_pkts] */
	__u32 max_pkts;        /* 输入：本次最多读取的报文数量 */
	__u32 min_pkts;        /* 输入：批量读取的最小目标报文数量 */
	__u32 timeout_us;      /* 输入：首包后的最大聚合等待时间，单位：微秒 */
	__u32 read_pkts;       /* 输出：实际读取的报文数量 */
	__u32 read_bytes;      /* 输出：实际读取的总字节数 */
	__s32 status;          /* 输出：0 或负 errno */
};

#define LQ_TUN_IOC_READ_BATCH _IOWR('T', 240, struct lq_tun_batch_read)

/*
 * 批量写入参数。
 *
 * entries:
 *   输入，指向 struct lq_tun_batch_entry[pkt_count] 数组。
 *
 * pkt_count:
 *   输入，本次需要写入的报文数量。
 *
 * written_pkts:
 *   输出，本次实际成功写入的报文数量。
 *
 * written_bytes:
 *   输出，本次实际成功写入的报文字节总数。
 *
 * status:
 *   输出，0 表示成功，负值表示 Linux errno。
 */
struct lq_tun_batch_write {
	__aligned_u64 entries; /* 输入：struct lq_tun_batch_entry[pkt_count] */
	__u32 pkt_count;       /* 输入：本次需要写入的报文数量 */
	__u32 written_pkts;    /* 输出：实际写入的报文数量 */
	__u32 written_bytes;   /* 输出：实际写入的总字节数 */
	__s32 status;          /* 输出：0 或负 errno */
};

#define LQ_TUN_IOC_WRITE_BATCH _IOWR('T', 241, struct lq_tun_batch_write)

/****************************** TUNSETIFF 标志 ******************************/

#define IFF_TUN          0x0001
#define IFF_TAP          0x0002
#define IFF_NAPI         0x0010
#define IFF_NAPI_FRAGS   0x0020
#define IFF_NO_PI        0x1000

/* 该标志当前没有实际作用 */
#define IFF_ONE_QUEUE    0x2000

#define IFF_VNET_HDR     0x4000
#define IFF_TUN_EXCL     0x8000
#define IFF_MULTI_QUEUE  0x0100
#define IFF_ATTACH_QUEUE 0x0200
#define IFF_DETACH_QUEUE 0x0400

/* 只读标志 */
#define IFF_PERSIST      0x0800
#define IFF_NOFILTER     0x1000

/****************************** Socket 选项 ******************************/

#define TUN_TX_TIMESTAMP 1

/****************************** GSO 卸载特性 ******************************/

/* TUNSETOFFLOAD 使用的 GSO 功能标志 */
#define TUN_F_CSUM     0x01 /* 允许提交尚未计算校验和的报文 */
#define TUN_F_TSO4     0x02 /* 支持 IPv4 TCP Segmentation Offload */
#define TUN_F_TSO6     0x04 /* 支持 IPv6 TCP Segmentation Offload */
#define TUN_F_TSO_ECN  0x08 /* TSO 支持 ECN */
#define TUN_F_UFO      0x10 /* 支持 UDP Fragmentation Offload */

/****************************** TUN 报文协议头 ******************************/

/*
 * 未设置 IFF_NO_PI 时，TUN 会在报文数据前附加该协议描述信息。
 */
#define TUN_PKT_STRIP 0x0001

struct tun_pi {
	__u16 flags;
	__be16 proto;
};

/****************************** TAP 地址过滤 ******************************/

/*
 * 地址过滤配置，仅适用于 TAP（Ethernet）设备。
 *
 * count 为 0 时关闭过滤器，驱动接受所有报文，相当于混杂模式。
 *
 * 启用过滤器后，如果希望接收广播报文，
 * 必须显式将广播 MAC 地址加入地址列表。
 */
#define TUN_FLT_ALLMULTI 0x0001 /* 接收所有组播报文 */

struct tun_filter {
	__u16 flags;             /* TUN_FLT_* 标志 */
	__u16 count;             /* 地址数量 */
	__u8 addr[0][ETH_ALEN];  /* MAC 地址列表 */
};

#endif /* _UAPI__IF_TUN_H */

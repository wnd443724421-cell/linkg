/**
 * @file linkg_fast_nat_rtsp.c
 * @brief LinkG Fast NAT RTSP协议扩展
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-14
 */

#include "linkg_fast_nat_internal.h"

#include <linux/errno.h>
#include <linux/ip.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/tcp.h>

/****************************** 模块常量 ******************************/

#define LINKG_FAST_NAT_RTSP_HEADER_MAX      768U // 单个TCP报文中最多解析的RTSP头长度
#define LINKG_FAST_NAT_RTSP_STATUS_PREFIX   "RTSP/1.0 200" // SETUP成功响应状态前缀
#define LINKG_FAST_NAT_RTSP_TRANSPORT       "Transport:"   // RTSP Transport头名称
#define LINKG_FAST_NAT_RTSP_CLIENT_PORT     "client_port=" // 客户端RTP/RTCP端口参数
#define LINKG_FAST_NAT_RTSP_SERVER_PORT     "server_port=" // 服务端RTP/RTCP端口参数
#define LINKG_FAST_NAT_RTSP_INTERLEAVED     "interleaved=" // TCP复用模式参数
#define LINKG_FAST_NAT_RTSP_RTP_AVP_TCP     "RTP/AVP/TCP"  // RTP over RTSP TCP模式标识

/****************************** 内部类型 ******************************/

typedef struct
{
    __be16 client_rtp_port;  // 客户端RTP接收端口
    __be16 client_rtcp_port; // 客户端RTCP接收端口，0表示未提供
    __be16 server_rtp_port;  // 服务端RTP发送端口
    __be16 server_rtcp_port; // 服务端RTCP端口，0表示未提供
} linkg_fast_nat_rtsp_media_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 在指定字符串范围内执行大小写不敏感的Token查找。
 */
static const char *_linkg_fast_nat_rtsp_find_token(const char *begin, const char *end, const char *token)
{
    size_t token_length;
    const char *cursor;

    if (begin == NULL || end == NULL || token == NULL || begin >= end)
    {
        return NULL;
    }

    token_length = strlen(token);
    if (token_length == 0U || (size_t)(end - begin) < token_length)
    {
        return NULL;
    }

    for (cursor = begin; cursor + token_length <= end; cursor++)
    {
        if (strncasecmp(cursor, token, token_length) == 0)
        {
            return cursor;
        }
    }

    return NULL;
}

/**
 * @brief 解析十进制UDP端口。
 */
static int _linkg_fast_nat_rtsp_parse_port(const char *begin, const char *end, __be16 *port, const char **next)
{
    unsigned int value = 0U;
    const char *cursor;

    if (begin == NULL || end == NULL || port == NULL || begin >= end)
    {
        return -EINVAL;
    }

    cursor = begin;
    if (*cursor < '0' || *cursor > '9')
    {
        return -EINVAL;
    }

    while (cursor < end && *cursor >= '0' && *cursor <= '9')
    {
        value = value * 10U + (unsigned int)(*cursor - '0');
        if (value > 65535U)
        {
            return -ERANGE;
        }

        cursor++;
    }

    if (value == 0U)
    {
        return -EINVAL;
    }

    *port = htons((__u16)value);

    if (next != NULL)
    {
        *next = cursor;
    }

    return 0;
}

/**
 * @brief 解析RTSP Transport参数中的单端口或RTP/RTCP端口对。
 */
static int _linkg_fast_nat_rtsp_parse_port_pair(const char *begin, const char *end, __be16 *first, __be16 *second)
{
    const char *cursor;
    int ret;

    if (begin == NULL || end == NULL || first == NULL || second == NULL)
    {
        return -EINVAL;
    }

    *first  = 0;
    *second = 0;

    ret = _linkg_fast_nat_rtsp_parse_port(begin, end, first, &cursor);
    if (ret != 0)
    {
        return ret;
    }

    if (cursor >= end || *cursor != '-')
    {
        return 0;
    }

    cursor++;
    return _linkg_fast_nat_rtsp_parse_port(cursor, end, second, NULL);
}

/**
 * @brief 从当前TCP报文提取完整RTSP响应头。
 *
 * 第一版只解析完整位于单个TCP skb中的RTSP头；跨TCP segment的控制头后续再增加流式重组。
 */
static int _linkg_fast_nat_rtsp_copy_header(struct sk_buff *skb, char *buffer, size_t capacity, size_t *header_length)
{
    struct iphdr *iph;
    struct tcphdr tcp_header_buffer;
    const struct tcphdr *tcph;
    unsigned int transport_offset;
    unsigned int tcp_header_length;
    unsigned int payload_offset;
    unsigned int payload_length;
    unsigned int copy_length;
    unsigned int network_offset;
    const char *header_end;
    int ret;

    if (skb == NULL || buffer == NULL || capacity < 2U || header_length == NULL)
    {
        return -EINVAL;
    }

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return ret;
    }

    if (iph->protocol != IPPROTO_TCP)
    {
        return -ENOENT;
    }

    network_offset = skb_network_offset(skb);
    tcph = skb_header_pointer(skb,
                              network_offset + transport_offset,
                              sizeof(tcp_header_buffer),
                              &tcp_header_buffer);
    if (tcph == NULL || tcph->doff < 5)
    {
        return -EINVAL;
    }

    tcp_header_length = (unsigned int)tcph->doff * 4U;
    payload_offset    = transport_offset + tcp_header_length;

    if ((unsigned int)ntohs(iph->tot_len) <= payload_offset)
    {
        return -ENOENT;
    }

    payload_length = (unsigned int)ntohs(iph->tot_len) - payload_offset;
    copy_length    = min_t(unsigned int, payload_length, (unsigned int)capacity - 1U);

    if (skb_copy_bits(skb, network_offset + payload_offset, buffer, copy_length) != 0)
    {
        return -EINVAL;
    }

    buffer[copy_length] = '\0';

    if (copy_length < strlen(LINKG_FAST_NAT_RTSP_STATUS_PREFIX) ||
        strncasecmp(buffer, LINKG_FAST_NAT_RTSP_STATUS_PREFIX, strlen(LINKG_FAST_NAT_RTSP_STATUS_PREFIX)) != 0)
    {
        return -ENOENT;
    }

    header_end = strnstr(buffer, "\r\n\r\n", copy_length);
    if (header_end == NULL)
    {
        return -EAGAIN;
    }

    *header_length = (size_t)(header_end - buffer) + 4U;
    return 0;
}

/**
 * @brief 从RTSP SETUP成功响应中解析RTP/RTCP UDP端口。
 */
static int _linkg_fast_nat_rtsp_parse_setup_response(struct sk_buff *skb, linkg_fast_nat_rtsp_media_t *media)
{
    char buffer[LINKG_FAST_NAT_RTSP_HEADER_MAX];
    const char *header_end;
    const char *line;
    const char *line_end;
    const char *client_port;
    const char *server_port;
    size_t header_length;
    int ret;

    if (media == NULL)
    {
        return -EINVAL;
    }

    memset(media, 0, sizeof(*media));

    ret = _linkg_fast_nat_rtsp_copy_header(skb, buffer, sizeof(buffer), &header_length);
    if (ret != 0)
    {
        return ret;
    }

    header_end = buffer + header_length;
    line = _linkg_fast_nat_rtsp_find_token(buffer, header_end, LINKG_FAST_NAT_RTSP_TRANSPORT);
    if (line == NULL)
    {
        return -ENOENT;
    }

    line_end = _linkg_fast_nat_rtsp_find_token(line, header_end, "\r\n");
    if (line_end == NULL)
    {
        return -EINVAL;
    }

    // RTP/RTCP通过RTSP TCP interleaved复用时不需要创建UDP派生流。
    if (_linkg_fast_nat_rtsp_find_token(line, line_end, LINKG_FAST_NAT_RTSP_RTP_AVP_TCP) != NULL ||
        _linkg_fast_nat_rtsp_find_token(line, line_end, LINKG_FAST_NAT_RTSP_INTERLEAVED) != NULL)
    {
        return -EOPNOTSUPP;
    }

    client_port = _linkg_fast_nat_rtsp_find_token(line, line_end, LINKG_FAST_NAT_RTSP_CLIENT_PORT);
    server_port = _linkg_fast_nat_rtsp_find_token(line, line_end, LINKG_FAST_NAT_RTSP_SERVER_PORT);

    if (client_port == NULL || server_port == NULL)
    {
        return -ENOENT;
    }

    client_port += strlen(LINKG_FAST_NAT_RTSP_CLIENT_PORT);
    server_port += strlen(LINKG_FAST_NAT_RTSP_SERVER_PORT);

    ret = _linkg_fast_nat_rtsp_parse_port_pair(client_port, line_end, &media->client_rtp_port, &media->client_rtcp_port);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_fast_nat_rtsp_parse_port_pair(server_port, line_end, &media->server_rtp_port, &media->server_rtcp_port);
    if (ret != 0)
    {
        return ret;
    }

    return 0;
}

/**
 * @brief 为摄像头侧Ethernet入口创建RTP/RTCP Related映射。
 */
static void _linkg_fast_nat_rtsp_create_ethernet_related(struct sk_buff *skb, const linkg_fast_nat_rtsp_media_t *media)
{
    struct iphdr *iph;
    unsigned int transport_offset;
    __be32 server_real_ip;
    __be32 client_virtual_ip;
    int ret;

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return;
    }

    server_real_ip   = iph->saddr;
    client_virtual_ip = iph->daddr;

    ret = linkg_fast_nat_related_add(server_real_ip,
                                     media->server_rtp_port,
                                     media->client_rtp_port,
                                     client_virtual_ip,
                                     media->client_rtp_port,
                                     IPPROTO_UDP);
    if (ret != 0)
    {
        pr_warn_ratelimited("LinkG Fast NAT: RTSP RTP Related create failed: %d\n", ret);
        return;
    }

    if (media->client_rtcp_port != 0 && media->server_rtcp_port != 0)
    {
        ret = linkg_fast_nat_related_add(server_real_ip,
                                         media->server_rtcp_port,
                                         media->client_rtcp_port,
                                         client_virtual_ip,
                                         media->client_rtcp_port,
                                         IPPROTO_UDP);
        if (ret != 0)
        {
            pr_warn_ratelimited("LinkG Fast NAT: RTSP RTCP Related create failed: %d\n", ret);
        }
    }

    pr_info_ratelimited("LinkG Fast NAT: RTSP Related ready real=%pI4 RTP=%u->%u client=%pI4 RTCP=%u->%u\n",
                        &server_real_ip,
                        ntohs(media->server_rtp_port),
                        ntohs(media->client_rtp_port),
                        &client_virtual_ip,
                        ntohs(media->server_rtcp_port),
                        ntohs(media->client_rtcp_port));
}

/**
 * @brief 为客户端侧Ethernet出口预建RTP/RTCP Flow，保证返回数据保留服务端Virtual源地址。
 */
static void _linkg_fast_nat_rtsp_create_client_flows(struct sk_buff *skb, const linkg_fast_nat_rtsp_media_t *media)
{
    struct iphdr *iph;
    unsigned int transport_offset;
    __be32 server_virtual_ip;
    __be32 client_virtual_ip;
    int ret;

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return;
    }

    server_virtual_ip = iph->saddr;
    client_virtual_ip = iph->daddr;

    ret = linkg_fast_nat_related_flow_create(server_virtual_ip,
                                              media->server_rtp_port,
                                              client_virtual_ip,
                                              media->client_rtp_port,
                                              IPPROTO_UDP);
    if (ret != 0)
    {
        pr_warn_ratelimited("LinkG Fast NAT: RTSP RTP client Flow create failed: %d\n", ret);
        return;
    }

    if (media->client_rtcp_port != 0 && media->server_rtcp_port != 0)
    {
        ret = linkg_fast_nat_related_flow_create(server_virtual_ip,
                                                  media->server_rtcp_port,
                                                  client_virtual_ip,
                                                  media->client_rtcp_port,
                                                  IPPROTO_UDP);
        if (ret != 0)
        {
            pr_warn_ratelimited("LinkG Fast NAT: RTSP RTCP client Flow create failed: %d\n", ret);
        }
    }

    pr_info_ratelimited("LinkG Fast NAT: RTSP client Flow ready server=%pI4 RTP=%u->%u client=%pI4 RTCP=%u->%u\n",
                        &server_virtual_ip,
                        ntohs(media->server_rtp_port),
                        ntohs(media->client_rtp_port),
                        &client_virtual_ip,
                        ntohs(media->server_rtcp_port),
                        ntohs(media->client_rtcp_port));
}

/****************************** RTSP接口 ******************************/

/**
 * @brief 观察摄像头侧已经完成Reverse SNAT的RTSP响应并创建UDP Related映射。
 */
void linkg_fast_nat_rtsp_observe_ethernet_rx(struct sk_buff *skb)
{
    linkg_fast_nat_rtsp_media_t media;
    int ret;

    ret = _linkg_fast_nat_rtsp_parse_setup_response(skb, &media);
    if (ret != 0)
    {
        return;
    }

    _linkg_fast_nat_rtsp_create_ethernet_related(skb, &media);
}

/**
 * @brief 观察客户端侧TUN入口RTSP响应并预建UDP返回Flow。
 */
void linkg_fast_nat_rtsp_observe_tun_rx(struct sk_buff *skb)
{
    linkg_fast_nat_rtsp_media_t media;
    int ret;

    ret = _linkg_fast_nat_rtsp_parse_setup_response(skb, &media);
    if (ret != 0)
    {
        return;
    }

    _linkg_fast_nat_rtsp_create_client_flows(skb, &media);
}

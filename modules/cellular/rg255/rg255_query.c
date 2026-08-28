/**
 * @file rg255_query.c
 * @brief RG255查询结果解析实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-28
 */

#define _POSIX_C_SOURCE                    200809L         // 启用strtok_r等POSIX接口

#include "rg255_query.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rg255_cmd.h"

/****************************** 解析常量 ******************************/

#define RG255_QUERY_RESPONSE_SIZE          2048U           // 单次查询响应缓存大小
#define RG255_QUERY_FIELD_MAX              40U             // 单行最多解析字段数量
#define RG255_QUERY_CME_SIM_NOT_INSERTED   10              // SIM未插入CME错误码
#define RG255_QUERY_CME_SIM_PIN_REQUIRED   11              // SIM需要PIN的CME错误码
#define RG255_QUERY_CME_SIM_PUK_REQUIRED   12              // SIM需要PUK的CME错误码
#define RG255_QUERY_CME_SIM_FAILURE        13              // SIM故障CME错误码
#define RG255_QUERY_CME_SIM_BUSY           14              // SIM忙CME错误码
#define RG255_QUERY_CME_SIM_WRONG          15              // SIM错误CME错误码
#define RG255_QUERY_CME_SIM_PIN2_REQUIRED  17              // SIM需要PIN2的CME错误码
#define RG255_QUERY_CME_SIM_PUK2_REQUIRED  18              // SIM需要PUK2的CME错误码
#define RG255_QUERY_PREFIX_CPIN            "+CPIN:"        // SIM状态响应前缀
#define RG255_QUERY_PREFIX_CME_ERROR       "+CME ERROR:"   // CME错误响应前缀
#define RG255_QUERY_PREFIX_NETWORK_MODE    "+QNWPREFCFG:"  // 网络模式响应前缀
#define RG255_QUERY_PREFIX_CEREG           "+CEREG:"       // EPS注册状态响应前缀
#define RG255_QUERY_PREFIX_C5GREG          "+C5GREG:"      // 5GS注册状态响应前缀
#define RG255_QUERY_PREFIX_QENG            "+QENG:"        // 服务小区响应前缀
#define RG255_QUERY_PREFIX_QCFG            "+QCFG:"        // Quectel配置响应前缀
#define RG255_QUERY_PREFIX_CGDCONT         "+CGDCONT:"     // PDP配置响应前缀
#define RG255_QUERY_PREFIX_CGACT           "+CGACT:"       // PDP激活状态响应前缀
#define RG255_QUERY_PREFIX_CGPADDR         "+CGPADDR:"     // PDP地址响应前缀
#define RG255_QUERY_PREFIX_QNETDEVCTL      "+QNETDEVCTL:"  // 网络设备状态响应前缀
#define RG255_QUERY_TEXT_MODE_PREF         "mode_pref"     // 网络模式配置键
#define RG255_QUERY_TEXT_MODE_AUTO         "AUTO"          // 自动网络搜索模式文本
#define RG255_QUERY_TEXT_MODE_AUTO_RATS    "NR5G-SA:LTE"   // 自动网络搜索模式等效RAT列表
#define RG255_QUERY_TEXT_MODE_LTE_FIRST    "LTE:NR5G-SA"   // LTE优先混合搜索模式文本
#define RG255_QUERY_TEXT_USBNET            "usbnet"        // USB网络模式配置键
#define RG255_QUERY_TEXT_NAT               "nat"           // NAT配置键
#define RG255_QUERY_TEXT_NETMASKSET        "netmaskset"    // USB网卡网络参数配置键
#define RG255_QUERY_TEXT_DONGLE            "dongle"        // 网卡模式标识文本
#define RG255_QUERY_NETMASKSET_IPV4_OPT    2               // 查询USB网卡IPv4参数
#define RG255_QUERY_NETMASKSET_IPV6_OPT    3               // 查询USB网卡IPv6参数
#define RG255_QUERY_TEXT_SERVING_CELL      "servingcell"   // 服务小区响应类型
#define RG255_QUERY_TEXT_LTE               "LTE"           // LTE网络类型文本
#define RG255_QUERY_TEXT_NR5G_SA           "NR5G-SA"       // 5G SA网络类型文本
#define RG255_QUERY_TEXT_SEARCH            "SEARCH"        // 服务小区正在搜索文本
#define RG255_QUERY_TEXT_LIMSRV            "LIMSRV"        // 服务小区受限服务文本
#define RG255_QUERY_TEXT_NOCONN            "NOCONN"        // 已注册且RRC空闲文本
#define RG255_QUERY_TEXT_CONNECT           "CONNECT"       // 已注册且RRC连接文本
#define RG255_QUERY_TEXT_PDP_IPV4          "IP"            // IPv4 PDP类型文本
#define RG255_QUERY_TEXT_PDP_IPV6          "IPV6"          // IPv6 PDP类型文本
#define RG255_QUERY_TEXT_PDP_IPV4V6        "IPV4V6"        // 双栈PDP类型文本

/****************************** 内部辅助 ******************************/

/**
 * @brief 去除字符串首尾空白字符。
 */
static char *_rg255_query_trim(char *text)
{
    char *end;

    if (text == NULL)
    {
        return NULL;
    }

    while (*text != '\0' && isspace((unsigned char)*text))
    {
        text++;
    }

    end = text + strlen(text);

    while (end > text && isspace((unsigned char)end[-1]))
    {
        end--;
    }

    *end = '\0';

    return text;
}

/**
 * @brief 获取AT字段文本并移除成对双引号。
 */
static int _rg255_query_get_field_text(char *field, char **text)
{
    size_t length;
    char *value;

    if (field == NULL || text == NULL)
    {
        return -EINVAL;
    }

    value = _rg255_query_trim(field);
    length = strlen(value);

    if (length == 0U)
    {
        *text = value;
        return 0;
    }

    if (value[0] == '"')
    {
        if (length < 2U || value[length - 1U] != '"')
        {
            return -EBADMSG;
        }

        value[length - 1U] = '\0';
        value++;
    }
    else if (strchr(value, '"') != NULL)
    {
        return -EBADMSG;
    }

    *text = value;

    return 0;
}

/**
 * @brief 按AT CSV格式拆分一行字段。
 */
static int _rg255_query_split_csv(char *text, char **fields, size_t fields_max, size_t *field_count)
{
    bool quoted;
    char *cursor;
    char *field_start;
    size_t count;

    if (text == NULL || fields == NULL || field_count == NULL || fields_max == 0U)
    {
        return -EINVAL;
    }

    quoted = false;
    count = 0U;
    field_start = text;

    for (cursor = text; ; cursor++)
    {
        if (*cursor == '"')
        {
            quoted = !quoted;
        }
        else if ((*cursor == ',' && !quoted) || *cursor == '\0')
        {
            bool end_of_text;

            if (count >= fields_max)
            {
                return -E2BIG;
            }

            end_of_text = *cursor == '\0';

            if (!end_of_text)
            {
                *cursor = '\0';
            }

            fields[count++] = _rg255_query_trim(field_start);

            if (end_of_text)
            {
                break;
            }

            field_start = cursor + 1;
        }
    }

    if (quoted)
    {
        return -EBADMSG;
    }

    *field_count = count;

    return 0;
}

/**
 * @brief 获取响应中第一条指定前缀行的正文。
 */
static int _rg255_query_find_line_body(char *response, const char *prefix, char **body)
{
    char *line;
    char *saveptr;
    size_t prefix_length;

    if (response == NULL || prefix == NULL || body == NULL)
    {
        return -EINVAL;
    }

    prefix_length = strlen(prefix);
    saveptr = NULL;
    line = strtok_r(response, "\r\n", &saveptr);

    while (line != NULL)
    {
        line = _rg255_query_trim(line);

        if (strncmp(line, prefix, prefix_length) == 0)
        {
            *body = _rg255_query_trim(line + prefix_length);
            return 0;
        }

        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    return -ENODATA;
}

/**
 * @brief 将十进制文本解析为int。
 */
static int _rg255_query_parse_int(const char *text, int *value)
{
    char *end;
    long parsed;

    if (text == NULL || value == NULL)
    {
        return -EINVAL;
    }

    if (text[0] == '\0')
    {
        return -EBADMSG;
    }

    errno = 0;
    end = NULL;
    parsed = strtol(text, &end, 10);

    if (errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX)
    {
        return -ERANGE;
    }

    if (end == text || *end != '\0')
    {
        return -EBADMSG;
    }

    *value = (int)parsed;

    return 0;
}

/**
 * @brief 将十进制文本解析为uint16_t。
 */
static int _rg255_query_parse_uint16(const char *text, uint16_t *value)
{
    char *end;
    unsigned long parsed;

    if (text == NULL || value == NULL)
    {
        return -EINVAL;
    }

    if (text[0] == '\0' || text[0] == '-')
    {
        return -EBADMSG;
    }

    errno = 0;
    end = NULL;
    parsed = strtoul(text, &end, 10);

    if (errno == ERANGE || parsed > UINT16_MAX)
    {
        return -ERANGE;
    }

    if (end == text || *end != '\0')
    {
        return -EBADMSG;
    }

    *value = (uint16_t)parsed;

    return 0;
}

/**
 * @brief 解析可能暂不可用的无线质量整数。
 */
static int _rg255_query_parse_optional_metric(char *field, int32_t *value, bool *valid)
{
    char *text;
    int parsed;
    int ret;

    if (field == NULL || value == NULL || valid == NULL)
    {
        return -EINVAL;
    }

    *value = 0;
    *valid = false;

    ret = _rg255_query_get_field_text(field, &text);

    if (ret != 0)
    {
        return ret;
    }

    if (text[0] == '\0' || strcmp(text, "-") == 0)
    {
        return 0;
    }

    ret = _rg255_query_parse_int(text, &parsed);

    if (ret != 0)
    {
        return ret;
    }

    *value = (int32_t)parsed;
    *valid = true;

    return 0;
}

/**
 * @brief 安全复制解析后的AT文本字段。
 */
static int _rg255_query_copy_text(char *destination, size_t destination_size, const char *source)
{
    size_t length;

    if (destination == NULL || source == NULL || destination_size == 0U)
    {
        return -EINVAL;
    }

    length = strlen(source);

    if (length >= destination_size)
    {
        return -EMSGSIZE;
    }

    memcpy(destination, source, length + 1U);

    return 0;
}

/**
 * @brief 判断IPv6地址是否为可路由全局地址。
 */
static bool _rg255_query_is_global_ipv6(const struct in6_addr *address)
{
    if (address == NULL)
    {
        return false;
    }

    if (IN6_IS_ADDR_UNSPECIFIED(address) ||
        IN6_IS_ADDR_LOOPBACK(address) ||
        IN6_IS_ADDR_LINKLOCAL(address) ||
        IN6_IS_ADDR_MULTICAST(address))
    {
        return false;
    }

    if ((address->s6_addr[0] & 0xFEU) == 0xFCU)
    {
        return false;
    }

    return true;
}

/**
 * @brief 解析单个PDP地址字段并按地址族保存。
 */
static int _rg255_query_parse_pdp_address_field(char *field, rg255_pdp_address_t *address)
{
    struct in6_addr ipv6;
    struct in_addr ipv4;
    char *text;
    int ret;

    if (field == NULL || address == NULL)
    {
        return -EINVAL;
    }

    ret = _rg255_query_get_field_text(field, &text);

    if (ret != 0)
    {
        return ret;
    }

    if (text[0] == '\0')
    {
        return 0;
    }

    ret = inet_pton(AF_INET, text, &ipv4);

    if (ret < 0)
    {
        return -errno;
    }

    if (ret == 1)
    {
        address->ipv4 = ipv4;
        address->ipv4_valid = ipv4.s_addr != htonl(INADDR_ANY);
        return 0;
    }

    ret = inet_pton(AF_INET6, text, &ipv6);

    if (ret < 0)
    {
        return -errno;
    }

    if (ret == 1)
    {
        if (_rg255_query_is_global_ipv6(&ipv6))
        {
            address->global_ipv6 = ipv6;
            address->global_ipv6_valid = true;
        }

        return 0;
    }

    return -EBADMSG;
}

/**
 * @brief 将CPIN正文映射为LinkG SIM状态。
 */
static int _rg255_query_map_cpin_state(const char *text, linkg_cellular_sim_state_t *state)
{
    if (text == NULL || state == NULL)
    {
        return -EINVAL;
    }

    if (strcmp(text, "READY") == 0)
    {
        *state = LINKG_CELLULAR_SIM_STATE_READY;
        return 0;
    }

    if (strcmp(text, "SIM PIN") == 0)
    {
        *state = LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED;
        return 0;
    }

    if (strcmp(text, "SIM PUK") == 0)
    {
        *state = LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED;
        return 0;
    }

    if (strcmp(text, "NOT INSERTED") == 0)
    {
        *state = LINKG_CELLULAR_SIM_STATE_ABSENT;
        return 0;
    }

    if (strcmp(text, "NOT READY") == 0)
    {
        *state = LINKG_CELLULAR_SIM_STATE_NOT_READY;
        return 0;
    }

    if (text[0] == '\0')
    {
        return -EBADMSG;
    }

    *state = LINKG_CELLULAR_SIM_STATE_NOT_READY;

    return 0;
}

/**
 * @brief 将SIM相关CME错误映射为LinkG SIM状态。
 */
static int _rg255_query_map_sim_cme_error(char *response, linkg_cellular_sim_state_t *state)
{
    char *body;
    char *end;
    long code;
    int ret;

    if (response == NULL || state == NULL)
    {
        return -EINVAL;
    }

    ret = _rg255_query_find_line_body(response, RG255_QUERY_PREFIX_CME_ERROR, &body);

    if (ret != 0)
    {
        return ret;
    }

    if (strcasecmp(body, "SIM not inserted") == 0 ||
        strcasecmp(body, "(U)SIM not inserted") == 0)
    {
        *state = LINKG_CELLULAR_SIM_STATE_ABSENT;
        return 0;
    }

    if (strcasecmp(body, "SIM PIN required") == 0)
    {
        *state = LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED;
        return 0;
    }

    if (strcasecmp(body, "SIM PUK required") == 0)
    {
        *state = LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED;
        return 0;
    }

    if (strcasecmp(body, "SIM failure") == 0 ||
        strcasecmp(body, "SIM busy") == 0 ||
        strcasecmp(body, "SIM wrong") == 0)
    {
        *state = LINKG_CELLULAR_SIM_STATE_NOT_READY;
        return 0;
    }

    errno = 0;
    end = NULL;
    code = strtol(body, &end, 10);

    if (errno != 0 || end == body || *end != '\0')
    {
        return -ENODATA;
    }

    switch (code)
    {
        case RG255_QUERY_CME_SIM_NOT_INSERTED:
            *state = LINKG_CELLULAR_SIM_STATE_ABSENT;
            return 0;

        case RG255_QUERY_CME_SIM_PIN_REQUIRED:
            *state = LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED;
            return 0;

        case RG255_QUERY_CME_SIM_PUK_REQUIRED:
            *state = LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED;
            return 0;

        case RG255_QUERY_CME_SIM_FAILURE:
        case RG255_QUERY_CME_SIM_BUSY:
        case RG255_QUERY_CME_SIM_WRONG:
        case RG255_QUERY_CME_SIM_PIN2_REQUIRED:
        case RG255_QUERY_CME_SIM_PUK2_REQUIRED:
            *state = LINKG_CELLULAR_SIM_STATE_NOT_READY;
            return 0;

        default:
            return -ENODATA;
    }
}

/**
 * @brief 将网络模式文本映射为LinkG网络模式。
 */
static int _rg255_query_map_network_mode(const char *text, linkg_cellular_network_mode_t *mode)
{
    if (text == NULL || mode == NULL)
    {
        return -EINVAL;
    }

    *mode = LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;

    if (strcmp(text, RG255_QUERY_TEXT_MODE_AUTO) == 0 ||
        strcmp(text, RG255_QUERY_TEXT_MODE_AUTO_RATS) == 0)
    {
        *mode = LINKG_CELLULAR_NETWORK_MODE_AUTO;
        return 0;
    }

    if (strcmp(text, RG255_QUERY_TEXT_LTE) == 0)
    {
        *mode = LINKG_CELLULAR_NETWORK_MODE_4G;
        return 0;
    }

    if (strcmp(text, RG255_QUERY_TEXT_NR5G_SA) == 0)
    {
        *mode = LINKG_CELLULAR_NETWORK_MODE_5G;
        return 0;
    }

    if (strcmp(text, RG255_QUERY_TEXT_MODE_LTE_FIRST) == 0)
    {
        return -EOPNOTSUPP;
    }

    return -EBADMSG;
}

/**
 * @brief 将标准注册状态值映射为LinkG注册状态。
 */
static int _rg255_query_map_registration_state(int stat, linkg_cellular_registration_state_t *state)
{
    if (state == NULL)
    {
        return -EINVAL;
    }

    switch (stat)
    {
        case 0:
            *state = LINKG_CELLULAR_REGISTRATION_STATE_NOT_REGISTERED;
            return 0;

        case 1:
        case 5:
            *state = LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED;
            return 0;

        case 2:
            *state = LINKG_CELLULAR_REGISTRATION_STATE_REGISTERING;
            return 0;

        case 3:
            *state = LINKG_CELLULAR_REGISTRATION_STATE_FAILED;
            return 0;

        case 4:
            *state = LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;
            return 0;

        default:
            return -EBADMSG;
    }
}

/**
 * @brief 合并AUTO模式下EPS和5GS注册状态。
 */
static linkg_cellular_registration_state_t _rg255_query_merge_registration_state(linkg_cellular_registration_state_t eps_state, linkg_cellular_registration_state_t nr_state)
{
    if (eps_state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED ||
        nr_state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED)
    {
        return LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED;
    }

    if (eps_state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERING ||
        nr_state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERING)
    {
        return LINKG_CELLULAR_REGISTRATION_STATE_REGISTERING;
    }

    if (eps_state == LINKG_CELLULAR_REGISTRATION_STATE_FAILED ||
        nr_state == LINKG_CELLULAR_REGISTRATION_STATE_FAILED)
    {
        return LINKG_CELLULAR_REGISTRATION_STATE_FAILED;
    }

    if (eps_state == LINKG_CELLULAR_REGISTRATION_STATE_NOT_REGISTERED ||
        nr_state == LINKG_CELLULAR_REGISTRATION_STATE_NOT_REGISTERED)
    {
        return LINKG_CELLULAR_REGISTRATION_STATE_NOT_REGISTERED;
    }

    return LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;
}

/**
 * @brief 解析单个CEREG或C5GREG查询响应。
 */
static int _rg255_query_parse_registration_response(char *response, const char *prefix, linkg_cellular_registration_state_t *state)
{
    char *fields[RG255_QUERY_FIELD_MAX];
    char *body;
    char *text;
    size_t field_count;
    int n;
    int stat;
    int ret;

    if (response == NULL || prefix == NULL || state == NULL)
    {
        return -EINVAL;
    }

    ret = _rg255_query_find_line_body(response, prefix, &body);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_split_csv(body, fields, RG255_QUERY_FIELD_MAX, &field_count);

    if (ret != 0)
    {
        return ret;
    }

    if (field_count < 2U)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[0], &text);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_int(text, &n);

    if (ret != 0)
    {
        return ret;
    }

    if (n < 0 || n > 2)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[1], &text);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_int(text, &stat);

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_query_map_registration_state(stat, state);
}

/**
 * @brief 查询并解析EPS注册状态。
 */
static int _rg255_query_eps_registration(at_channel_t *channel, linkg_cellular_registration_state_t *state)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    int ret;

    response[0] = '\0';

    ret = rg255_cmd_query_eps_registration(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_query_parse_registration_response(response, RG255_QUERY_PREFIX_CEREG, state);
}

/**
 * @brief 查询并解析5GS注册状态。
 */
static int _rg255_query_5g_registration(at_channel_t *channel, linkg_cellular_registration_state_t *state)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    int ret;

    response[0] = '\0';

    ret = rg255_cmd_query_5g_registration(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_query_parse_registration_response(response, RG255_QUERY_PREFIX_C5GREG, state);
}

/**
 * @brief 解析LTE服务小区无线状态。
 */
static int _rg255_query_parse_lte_serving_cell(char **fields, size_t field_count, rg255_serving_cell_info_t *info)
{
    char *text;
    int ret;

    if (fields == NULL || info == NULL)
    {
        return -EINVAL;
    }

    if (field_count < 17U)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[9], &text);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_uint16(text, &info->band);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_optional_metric(fields[13], &info->rsrp_dbm, &info->rsrp_valid);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_optional_metric(fields[14], &info->rsrq_db, &info->rsrq_valid);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_optional_metric(fields[16], &info->sinr_db, &info->sinr_valid);

    if (ret != 0)
    {
        return ret;
    }

    info->network_type = LINKG_CELLULAR_NETWORK_TYPE_LTE;

    return 0;
}

/**
 * @brief 解析NR5G-SA服务小区无线状态。
 */
static int _rg255_query_parse_nr5g_sa_serving_cell(char **fields, size_t field_count, rg255_serving_cell_info_t *info)
{
    char *text;
    int ret;

    if (fields == NULL || info == NULL)
    {
        return -EINVAL;
    }

    if (field_count < 15U)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[10], &text);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_uint16(text, &info->band);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_optional_metric(fields[12], &info->rsrp_dbm, &info->rsrp_valid);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_optional_metric(fields[13], &info->rsrq_db, &info->rsrq_valid);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_optional_metric(fields[14], &info->sinr_db, &info->sinr_valid);

    if (ret != 0)
    {
        return ret;
    }

    info->network_type = LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA;

    return 0;
}

/**
 * @brief 解析QENG服务小区响应。
 */
static int _rg255_query_parse_serving_cell_response(char *response, rg255_serving_cell_info_t *info)
{
    char *fields[RG255_QUERY_FIELD_MAX];
    char *line;
    char *saveptr;
    char *body;
    char *response_type;
    char *network_type;
    char *state_text;
    size_t field_count;
    size_t prefix_length;
    bool serving_cell_seen;
    int ret;

    if (response == NULL || info == NULL)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));
    info->network_type = LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;

    prefix_length = strlen(RG255_QUERY_PREFIX_QENG);
    serving_cell_seen = false;
    saveptr = NULL;
    line = strtok_r(response, "\r\n", &saveptr);

    while (line != NULL)
    {
        line = _rg255_query_trim(line);

        if (strncmp(line, RG255_QUERY_PREFIX_QENG, prefix_length) != 0)
        {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        body = _rg255_query_trim(line + prefix_length);
        ret = _rg255_query_split_csv(body, fields, RG255_QUERY_FIELD_MAX, &field_count);

        if (ret != 0)
        {
            return ret;
        }

        if (field_count == 0U)
        {
            return -EBADMSG;
        }

        ret = _rg255_query_get_field_text(fields[0], &response_type);

        if (ret != 0)
        {
            return ret;
        }

        if (strcmp(response_type, RG255_QUERY_TEXT_SERVING_CELL) != 0)
        {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        serving_cell_seen = true;

        if (field_count >= 2U)
        {
            ret = _rg255_query_get_field_text(fields[1], &state_text);

            if (ret != 0)
            {
                return ret;
            }

            if (strcmp(state_text, RG255_QUERY_TEXT_SEARCH) == 0)
            {
                return -ENODATA;
            }

            if (strcmp(state_text, RG255_QUERY_TEXT_LIMSRV) != 0 &&
                strcmp(state_text, RG255_QUERY_TEXT_NOCONN) != 0 &&
                strcmp(state_text, RG255_QUERY_TEXT_CONNECT) != 0)
            {
                return -EBADMSG;
            }
        }

        if (field_count < 3U)
        {
            return -ENODATA;
        }

        ret = _rg255_query_get_field_text(fields[2], &network_type);

        if (ret != 0)
        {
            return ret;
        }

        if (strcmp(network_type, RG255_QUERY_TEXT_LTE) == 0)
        {
            return _rg255_query_parse_lte_serving_cell(fields, field_count, info);
        }

        if (strcmp(network_type, RG255_QUERY_TEXT_NR5G_SA) == 0)
        {
            return _rg255_query_parse_nr5g_sa_serving_cell(fields, field_count, info);
        }

        return -EOPNOTSUPP;
    }

    if (serving_cell_seen)
    {
        return -ENODATA;
    }

    return -ENODATA;
}

/**
 * @brief 解析QCFG整数配置值。
 */
static int _rg255_query_parse_qcfg_int(char *response, const char *expected_key, int *value)
{
    char *fields[RG255_QUERY_FIELD_MAX];
    char *body;
    char *key;
    char *text;
    size_t field_count;
    int ret;

    if (response == NULL || expected_key == NULL || value == NULL)
    {
        return -EINVAL;
    }

    ret = _rg255_query_find_line_body(response, RG255_QUERY_PREFIX_QCFG, &body);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_split_csv(body, fields, RG255_QUERY_FIELD_MAX, &field_count);

    if (ret != 0)
    {
        return ret;
    }

    if (field_count < 2U)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[0], &key);

    if (ret != 0)
    {
        return ret;
    }

    if (strcmp(key, expected_key) != 0)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[1], &text);

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_query_parse_int(text, value);
}

/**
 * @brief 解析AT字段中的IPv4地址。
 */
static int _rg255_query_parse_ipv4_field(char *field, struct in_addr *address)
{
    char *text;
    int ret;

    if (field == NULL || address == NULL)
    {
        return -EINVAL;
    }

    ret = _rg255_query_get_field_text(field, &text);

    if (ret != 0)
    {
        return ret;
    }

    if (text[0] == '\0')
    {
        return -ENODATA;
    }

    ret = inet_pton(AF_INET, text, address);

    if (ret < 0)
    {
        return -errno;
    }

    if (ret == 0)
    {
        return -EBADMSG;
    }

    if (address->s_addr == htonl(INADDR_ANY))
    {
        return -ENODATA;
    }

    return 0;
}

/**
 * @brief 解析AT字段中的IPv6地址。
 *
 * @note 该辅助函数允许链路本地IPv6地址，用于解析IPv6网关。
 */
static int _rg255_query_parse_ipv6_field(char *field, struct in6_addr *address)
{
    char *text;
    int ret;

    if (field == NULL || address == NULL)
    {
        return -EINVAL;
    }

    ret = _rg255_query_get_field_text(field, &text);

    if (ret != 0)
    {
        return ret;
    }

    if (text[0] == '\0')
    {
        return -ENODATA;
    }

    ret = inet_pton(AF_INET6, text, address);

    if (ret < 0)
    {
        return -errno;
    }

    if (ret == 0)
    {
        return -EBADMSG;
    }

    if (IN6_IS_ADDR_UNSPECIFIED(address) ||
        IN6_IS_ADDR_LOOPBACK(address) ||
        IN6_IS_ADDR_MULTICAST(address))
    {
        return -ENODATA;
    }

    return 0;
}

/**
 * @brief 将IPv6地址按前缀长度归一化为网络前缀。
 */
static void _rg255_query_mask_ipv6_prefix(struct in6_addr *prefix, uint8_t prefix_length)
{
    size_t full_bytes;
    uint8_t remaining_bits;
    size_t index;

    if (prefix == NULL)
    {
        return;
    }

    full_bytes = prefix_length / 8U;
    remaining_bits = prefix_length % 8U;

    if (remaining_bits != 0U && full_bytes < sizeof(prefix->s6_addr))
    {
        prefix->s6_addr[full_bytes] &= (uint8_t)(0xFFU << (8U - remaining_bits));
        full_bytes++;
    }

    for (index = full_bytes; index < sizeof(prefix->s6_addr); index++)
    {
        prefix->s6_addr[index] = 0U;
    }
}

/**
 * @brief 解析AT字段中的IPv6网络前缀。
 */
static int _rg255_query_parse_ipv6_prefix_field(char *field, struct in6_addr *prefix, uint8_t *prefix_length)
{
    char *slash;
    char *text;
    int length;
    int ret;

    if (field == NULL || prefix == NULL || prefix_length == NULL)
    {
        return -EINVAL;
    }

    ret = _rg255_query_get_field_text(field, &text);

    if (ret != 0)
    {
        return ret;
    }

    if (text[0] == '\0')
    {
        return -ENODATA;
    }

    slash = strrchr(text, '/');

    if (slash == NULL || slash == text || slash[1] == '\0')
    {
        return -EBADMSG;
    }

    *slash = '\0';

    ret = _rg255_query_parse_int(slash + 1, &length);

    if (ret != 0)
    {
        return ret;
    }

    if (length < 0 || length > 128)
    {
        return -ERANGE;
    }

    ret = inet_pton(AF_INET6, text, prefix);

    if (ret < 0)
    {
        return -errno;
    }

    if (ret == 0)
    {
        return -EBADMSG;
    }

    if (IN6_IS_ADDR_UNSPECIFIED(prefix) && length != 0)
    {
        return -ENODATA;
    }

    *prefix_length = (uint8_t)length;
    _rg255_query_mask_ipv6_prefix(prefix, *prefix_length);

    return 0;
}

/**
 * @brief 解析网卡模式下USB网卡IPv4网络参数响应。
 */
static int _rg255_query_parse_network_card_ipv4_response(char *response, rg255_network_card_ipv4_info_t *info)
{
    char *fields[RG255_QUERY_FIELD_MAX];
    char *body;
    char *key;
    char *mode_text;
    char *text;
    size_t field_count;
    int opt;
    int ret;

    if (response == NULL || info == NULL)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));

    ret = _rg255_query_find_line_body(response, RG255_QUERY_PREFIX_QCFG, &body);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_split_csv(body, fields, RG255_QUERY_FIELD_MAX, &field_count);

    if (ret != 0)
    {
        return ret;
    }

    if (field_count < 6U)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[0], &key);

    if (ret != 0)
    {
        return ret;
    }

    if (strcmp(key, RG255_QUERY_TEXT_NETMASKSET) != 0)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[1], &text);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_int(text, &opt);

    if (ret != 0)
    {
        return ret;
    }

    if (opt != RG255_QUERY_NETMASKSET_IPV4_OPT)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[2], &mode_text);

    if (ret != 0)
    {
        return ret;
    }

    if (strcmp(mode_text, RG255_QUERY_TEXT_DONGLE) != 0)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_parse_ipv4_field(fields[3], &info->address);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_ipv4_field(fields[4], &info->netmask);

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_query_parse_ipv4_field(fields[5], &info->gateway);
}

/**
 * @brief 解析网卡模式下USB网卡IPv6网络参数响应。
 */
static int _rg255_query_parse_network_card_ipv6_response(char *response, rg255_network_card_ipv6_info_t *info)
{
    char *fields[RG255_QUERY_FIELD_MAX];
    char *body;
    char *key;
    char *mode_text;
    char *text;
    size_t field_count;
    int opt;
    int ret;

    if (response == NULL || info == NULL)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));

    ret = _rg255_query_find_line_body(response, RG255_QUERY_PREFIX_QCFG, &body);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_split_csv(body, fields, RG255_QUERY_FIELD_MAX, &field_count);

    if (ret != 0)
    {
        return ret;
    }

    /**
     * 当前只消费prefix和gateway，因此只要求前5个字段完整存在。
     * 厂家返回的主/备DNS字段由本层忽略，不为未使用信息增加接口负担。
     */
    if (field_count < 5U)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[0], &key);

    if (ret != 0)
    {
        return ret;
    }

    if (strcmp(key, RG255_QUERY_TEXT_NETMASKSET) != 0)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[1], &text);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_int(text, &opt);

    if (ret != 0)
    {
        return ret;
    }

    if (opt != RG255_QUERY_NETMASKSET_IPV6_OPT)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[2], &mode_text);

    if (ret != 0)
    {
        return ret;
    }

    if (strcmp(mode_text, RG255_QUERY_TEXT_DONGLE) != 0)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_parse_ipv6_prefix_field(fields[3], &info->prefix, &info->prefix_length);

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_query_parse_ipv6_field(fields[4], &info->gateway);
}

/**
 * @brief 将PDP类型文本映射为内部PDP类型。
 */
static rg255_pdp_type_t _rg255_query_map_pdp_type(const char *text)
{
    if (text == NULL)
    {
        return RG255_PDP_TYPE_UNKNOWN;
    }

    if (strcmp(text, RG255_QUERY_TEXT_PDP_IPV4) == 0)
    {
        return RG255_PDP_TYPE_IPV4;
    }

    if (strcmp(text, RG255_QUERY_TEXT_PDP_IPV6) == 0)
    {
        return RG255_PDP_TYPE_IPV6;
    }

    if (strcmp(text, RG255_QUERY_TEXT_PDP_IPV4V6) == 0)
    {
        return RG255_PDP_TYPE_IPV4V6;
    }

    return RG255_PDP_TYPE_UNKNOWN;
}

/****************************** SIM查询 ******************************/

/**
 * @brief 查询并解析RG255当前SIM状态。
 */
int rg255_query_sim_state(at_channel_t *channel, linkg_cellular_sim_state_t *state)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    char *body;
    char *text;
    int ret;

    if (channel == NULL || state == NULL)
    {
        return -EINVAL;
    }

    *state = LINKG_CELLULAR_SIM_STATE_UNKNOWN;
    response[0] = '\0';

    ret = rg255_cmd_query_sim_status(channel, response, sizeof(response));

    if (ret != 0)
    {
        if (ret == -EREMOTEIO)
        {
            int map_ret;

            map_ret = _rg255_query_map_sim_cme_error(response, state);

            if (map_ret == 0)
            {
                return 0;
            }
        }

        return ret;
    }

    ret = _rg255_query_find_line_body(response, RG255_QUERY_PREFIX_CPIN, &body);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_get_field_text(body, &text);

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_query_map_cpin_state(text, state);
}

/****************************** 网络查询 ******************************/

/**
 * @brief 查询并解析RG255当前网络搜索模式。
 */
int rg255_query_network_mode(at_channel_t *channel, linkg_cellular_network_mode_t *mode)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    char *fields[RG255_QUERY_FIELD_MAX];
    char *body;
    char *key;
    char *text;
    size_t field_count;
    int ret;

    if (channel == NULL || mode == NULL)
    {
        return -EINVAL;
    }

    *mode = LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;
    response[0] = '\0';

    ret = rg255_cmd_query_network_mode(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_find_line_body(response, RG255_QUERY_PREFIX_NETWORK_MODE, &body);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_split_csv(body, fields, RG255_QUERY_FIELD_MAX, &field_count);

    if (ret != 0)
    {
        return ret;
    }

    if (field_count < 2U)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[0], &key);

    if (ret != 0)
    {
        return ret;
    }

    if (strcmp(key, RG255_QUERY_TEXT_MODE_PREF) != 0)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[1], &text);

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_query_map_network_mode(text, mode);
}

/**
 * @brief 查询并归一化RG255当前网络注册状态。
 */
int rg255_query_registration(at_channel_t *channel, linkg_cellular_network_mode_t mode, linkg_cellular_network_type_t network_type, linkg_cellular_registration_state_t *state)
{
    linkg_cellular_registration_state_t eps_state;
    linkg_cellular_registration_state_t nr_state;
    int eps_ret;
    int nr_ret;

    if (channel == NULL || state == NULL)
    {
        return -EINVAL;
    }

    *state = LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;

    if (mode == LINKG_CELLULAR_NETWORK_MODE_4G)
    {
        return _rg255_query_eps_registration(channel, state);
    }

    if (mode == LINKG_CELLULAR_NETWORK_MODE_5G)
    {
        return _rg255_query_5g_registration(channel, state);
    }

    if (network_type == LINKG_CELLULAR_NETWORK_TYPE_LTE)
    {
        return _rg255_query_eps_registration(channel, state);
    }

    if (network_type == LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA)
    {
        return _rg255_query_5g_registration(channel, state);
    }

    eps_state = LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;
    nr_state = LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;

    eps_ret = _rg255_query_eps_registration(channel, &eps_state);
    nr_ret = _rg255_query_5g_registration(channel, &nr_state);

    if (eps_ret != 0 && nr_ret != 0)
    {
        return eps_ret;
    }

    /**
     * AUTO且实际网络类型未知时需要同时查询EPS和5GS。
     * 如果其中一个查询失败，只有另一个明确REGISTERED时才能确认整体已经驻网；
     * 其余情况无法排除失败的注册域已经注册，因此保留查询错误而不输出错误事实。
     */
    if (eps_ret != 0)
    {
        if (nr_state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED)
        {
            *state = nr_state;
            return 0;
        }

        return eps_ret;
    }

    if (nr_ret != 0)
    {
        if (eps_state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED)
        {
            *state = eps_state;
            return 0;
        }

        return nr_ret;
    }

    *state = _rg255_query_merge_registration_state(eps_state, nr_state);

    return 0;
}

/**
 * @brief 查询并解析RG255当前服务小区无线状态。
 */
int rg255_query_serving_cell(at_channel_t *channel, rg255_serving_cell_info_t *info)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    int ret;

    if (channel == NULL || info == NULL)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));
    info->network_type = LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;
    response[0] = '\0';

    ret = rg255_cmd_query_serving_cell(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_query_parse_serving_cell_response(response, info);
}

/****************************** USB配置查询 ******************************/

/**
 * @brief 查询并解析RG255当前USB网卡接口协议。
 */
int rg255_query_usbnet_mode(at_channel_t *channel, rg255_usbnet_mode_t *mode)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    int value;
    int ret;

    if (channel == NULL || mode == NULL)
    {
        return -EINVAL;
    }

    *mode = RG255_USBNET_MODE_UNKNOWN;
    response[0] = '\0';

    ret = rg255_cmd_query_usbnet(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_qcfg_int(response, RG255_QUERY_TEXT_USBNET, &value);

    if (ret != 0)
    {
        return ret;
    }

    switch (value)
    {
        case RG255_USBNET_MODE_ECM:
            *mode = RG255_USBNET_MODE_ECM;
            return 0;

        case RG255_USBNET_MODE_MBIM:
            *mode = RG255_USBNET_MODE_MBIM;
            return 0;

        case RG255_USBNET_MODE_RNDIS:
            *mode = RG255_USBNET_MODE_RNDIS;
            return 0;

        default:
            return -EBADMSG;
    }
}

/**
 * @brief 查询并解析RG255当前USB网卡工作模式。
 */
int rg255_query_network_card_mode(at_channel_t *channel, rg255_network_card_mode_t *mode)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    int value;
    int ret;

    if (channel == NULL || mode == NULL)
    {
        return -EINVAL;
    }

    *mode = RG255_NETWORK_CARD_MODE_UNKNOWN;
    response[0] = '\0';

    ret = rg255_cmd_query_network_card_mode(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_qcfg_int(response, RG255_QUERY_TEXT_NAT, &value);

    if (ret != 0)
    {
        return ret;
    }

    if (value == RG255_NETWORK_CARD_MODE_ROUTER)
    {
        *mode = RG255_NETWORK_CARD_MODE_ROUTER;
        return 0;
    }

    if (value == RG255_NETWORK_CARD_MODE_NIC)
    {
        *mode = RG255_NETWORK_CARD_MODE_NIC;
        return 0;
    }

    return -EBADMSG;
}

/**
 * @brief 查询并解析RG255提供给Host USB网卡的IPv4网络参数。
 *
 * @note 该查询需要QNETDEV网卡拨号成功后执行。
 */
int rg255_query_network_card_ipv4(at_channel_t *channel, rg255_network_card_ipv4_info_t *info)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    int ret;

    if (channel == NULL || info == NULL)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));
    response[0] = '\0';

    ret = rg255_cmd_query_network_card_ipv4(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_query_parse_network_card_ipv4_response(response, info);
}

/**
 * @brief 查询并解析RG255提供给Host USB网卡的IPv6网络参数。
 *
 * @note 该查询需要QNETDEV网卡拨号成功后执行；返回的是IPv6网络前缀和网关，不是Host完整IPv6地址。
 */
int rg255_query_network_card_ipv6(at_channel_t *channel, rg255_network_card_ipv6_info_t *info)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    int ret;

    if (channel == NULL || info == NULL)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));
    response[0] = '\0';

    ret = rg255_cmd_query_network_card_ipv6(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_query_parse_network_card_ipv6_response(response, info);
}

/****************************** PDP查询 ******************************/

/**
 * @brief 查询并解析默认PDP上下文配置。
 */
int rg255_query_pdp_config(at_channel_t *channel, rg255_pdp_config_t *config)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    char *fields[RG255_QUERY_FIELD_MAX];
    char *line;
    char *saveptr;
    char *body;
    char *text;
    size_t field_count;
    size_t prefix_length;
    int cid;
    int ret;

    if (channel == NULL || config == NULL)
    {
        return -EINVAL;
    }

    memset(config, 0, sizeof(*config));
    config->pdp_type = RG255_PDP_TYPE_UNKNOWN;
    response[0] = '\0';

    ret = rg255_cmd_query_pdp_config(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    prefix_length = strlen(RG255_QUERY_PREFIX_CGDCONT);
    saveptr = NULL;
    line = strtok_r(response, "\r\n", &saveptr);

    while (line != NULL)
    {
        line = _rg255_query_trim(line);

        if (strncmp(line, RG255_QUERY_PREFIX_CGDCONT, prefix_length) != 0)
        {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        body = _rg255_query_trim(line + prefix_length);
        ret = _rg255_query_split_csv(body, fields, RG255_QUERY_FIELD_MAX, &field_count);

        if (ret != 0)
        {
            return ret;
        }

        if (field_count < 3U)
        {
            return -EBADMSG;
        }

        ret = _rg255_query_get_field_text(fields[0], &text);

        if (ret != 0)
        {
            return ret;
        }

        ret = _rg255_query_parse_int(text, &cid);

        if (ret != 0)
        {
            return ret;
        }

        if (cid != (int)RG255_PDP_CONTEXT_ID)
        {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        config->cid = (uint8_t)cid;

        ret = _rg255_query_get_field_text(fields[1], &text);

        if (ret != 0)
        {
            return ret;
        }

        config->pdp_type = _rg255_query_map_pdp_type(text);

        if (config->pdp_type == RG255_PDP_TYPE_UNKNOWN)
        {
            return -EOPNOTSUPP;
        }

        ret = _rg255_query_get_field_text(fields[2], &text);

        if (ret != 0)
        {
            return ret;
        }

        return _rg255_query_copy_text(config->apn, sizeof(config->apn), text);
    }

    return -ENOENT;
}

/**
 * @brief 查询并解析默认PDP上下文激活状态。
 */
int rg255_query_pdp_active(at_channel_t *channel, bool *active)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    char *fields[RG255_QUERY_FIELD_MAX];
    char *line;
    char *saveptr;
    char *body;
    char *text;
    size_t field_count;
    size_t prefix_length;
    int cid;
    int state;
    int ret;

    if (channel == NULL || active == NULL)
    {
        return -EINVAL;
    }

    *active = false;
    response[0] = '\0';

    ret = rg255_cmd_query_pdp_state(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    prefix_length = strlen(RG255_QUERY_PREFIX_CGACT);
    saveptr = NULL;
    line = strtok_r(response, "\r\n", &saveptr);

    while (line != NULL)
    {
        line = _rg255_query_trim(line);

        if (strncmp(line, RG255_QUERY_PREFIX_CGACT, prefix_length) != 0)
        {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        body = _rg255_query_trim(line + prefix_length);
        ret = _rg255_query_split_csv(body, fields, RG255_QUERY_FIELD_MAX, &field_count);

        if (ret != 0)
        {
            return ret;
        }

        if (field_count < 2U)
        {
            return -EBADMSG;
        }

        ret = _rg255_query_get_field_text(fields[0], &text);

        if (ret != 0)
        {
            return ret;
        }

        ret = _rg255_query_parse_int(text, &cid);

        if (ret != 0)
        {
            return ret;
        }

        if (cid != (int)RG255_PDP_CONTEXT_ID)
        {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        ret = _rg255_query_get_field_text(fields[1], &text);

        if (ret != 0)
        {
            return ret;
        }

        ret = _rg255_query_parse_int(text, &state);

        if (ret != 0)
        {
            return ret;
        }

        if (state == 0)
        {
            *active = false;
            return 0;
        }

        if (state == 1)
        {
            *active = true;
            return 0;
        }

        return -EBADMSG;
    }

    return -ENOENT;
}

/**
 * @brief 查询并解析默认PDP上下文模组地址。
 */
int rg255_query_pdp_address(at_channel_t *channel, rg255_pdp_address_t *address)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    char *fields[RG255_QUERY_FIELD_MAX];
    char *body;
    char *text;
    size_t field_count;
    size_t index;
    int cid;
    int ret;

    if (channel == NULL || address == NULL)
    {
        return -EINVAL;
    }

    memset(address, 0, sizeof(*address));
    response[0] = '\0';

    ret = rg255_cmd_query_pdp_address(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_find_line_body(response, RG255_QUERY_PREFIX_CGPADDR, &body);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_split_csv(body, fields, RG255_QUERY_FIELD_MAX, &field_count);

    if (ret != 0)
    {
        return ret;
    }

    if (field_count < 1U)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[0], &text);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_int(text, &cid);

    if (ret != 0)
    {
        return ret;
    }

    if (cid != (int)RG255_PDP_CONTEXT_ID)
    {
        return -EBADMSG;
    }

    for (index = 1U; index < field_count; index++)
    {
        ret = _rg255_query_parse_pdp_address_field(fields[index], address);

        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** 网络设备查询 ******************************/

/**
 * @brief 查询并解析RG255 USB网络设备状态。
 */
int rg255_query_netdev_status(at_channel_t *channel, rg255_netdev_status_t *status)
{
    char response[RG255_QUERY_RESPONSE_SIZE];
    char *fields[RG255_QUERY_FIELD_MAX];
    char *body;
    char *text;
    size_t field_count;
    int type;
    int cid;
    int urc_enabled;
    int state;
    int ret;

    if (channel == NULL || status == NULL)
    {
        return -EINVAL;
    }

    memset(status, 0, sizeof(*status));
    status->type = RG255_NETDEV_TYPE_UNKNOWN;
    response[0] = '\0';

    ret = rg255_cmd_query_netdev(channel, response, sizeof(response));

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_find_line_body(response, RG255_QUERY_PREFIX_QNETDEVCTL, &body);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_split_csv(body, fields, RG255_QUERY_FIELD_MAX, &field_count);

    if (ret != 0)
    {
        return ret;
    }

    if (field_count < 4U)
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[0], &text);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_int(text, &type);

    if (ret != 0)
    {
        return ret;
    }

    switch (type)
    {
        case RG255_NETDEV_TYPE_DISCONNECT:
            status->type = RG255_NETDEV_TYPE_DISCONNECT;
            break;

        case RG255_NETDEV_TYPE_ONCE:
            status->type = RG255_NETDEV_TYPE_ONCE;
            break;

        case RG255_NETDEV_TYPE_AUTO:
            status->type = RG255_NETDEV_TYPE_AUTO;
            break;

        default:
            return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[1], &text);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_int(text, &cid);

    if (ret != 0)
    {
        return ret;
    }

    if (status->type == RG255_NETDEV_TYPE_DISCONNECT)
    {
        if (cid < 0 || cid > 11)
        {
            return -EBADMSG;
        }
    }
    else if (cid < 1 || cid > 11)
    {
        return -EBADMSG;
    }

    status->cid = (uint8_t)cid;

    ret = _rg255_query_get_field_text(fields[2], &text);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_int(text, &urc_enabled);

    if (ret != 0)
    {
        return ret;
    }

    if (urc_enabled == 0)
    {
        status->urc_enabled = false;
    }
    else if (urc_enabled == 1)
    {
        status->urc_enabled = true;
    }
    else
    {
        return -EBADMSG;
    }

    ret = _rg255_query_get_field_text(fields[3], &text);

    if (ret != 0)
    {
        return ret;
    }

    ret = _rg255_query_parse_int(text, &state);

    if (ret != 0)
    {
        return ret;
    }

    if (state == 0)
    {
        status->connected = false;
        return 0;
    }

    if (state == 1)
    {
        status->connected = true;
        return 0;
    }

    return -EBADMSG;
}

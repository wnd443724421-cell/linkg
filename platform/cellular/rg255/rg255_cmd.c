/**
 * @file rg255_cmd.c
 * @brief RG255 AT命令封装实现
 * @author Dawn
 * @version 2.1.0
 * @date 2026-08-28
 */

#include "rg255_cmd.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/****************************** 命令常量 ******************************/

#define RG255_CMD_BUFFER_SIZE                 160U    // 动态AT命令缓存大小
#define RG255_CMD_TIMEOUT_DEFAULT_MS          3000    // 普通AT命令超时，单位ms
#define RG255_CMD_TIMEOUT_QUERY_MS            5000    // 普通查询命令超时，单位ms
#define RG255_CMD_TIMEOUT_CONFIG_MS           5000    // 普通配置命令超时，单位ms
#define RG255_CMD_TIMEOUT_NETWORK_MODE_MS     10000   // 网络搜索模式查询或设置超时，单位ms
#define RG255_CMD_TIMEOUT_PIN_MS              5000    // SIM PIN相关命令超时，单位ms
#define RG255_CMD_TIMEOUT_PDP_ACTIVE_MS       150000  // PDP激活或去激活超时，单位ms
#define RG255_CMD_TIMEOUT_NETDEV_MS           3000    // USB网络设备控制超时，单位ms
#define RG255_CMD_TIMEOUT_RESTART_MS          15000   // 模块功能复位命令超时，单位ms
#define RG255_CMD_CONTINUATION_MAX_NETMASKSET 1U      // netmaskset IPv6响应最大续行数量

/****************************** 内部辅助 ******************************/

/**
 * @brief 执行不需要返回内容的AT命令。
 */
static int _rg255_cmd_exec(at_channel_t *channel, const char *command, int timeout_ms)
{
    at_command_config_t config;

    if (channel == NULL)
    {
        return -EINVAL;
    }

    if (command == NULL)
    {
        return -EINVAL;
    }

    memset(&config, 0, sizeof(config));
    config.timeout_ms = timeout_ms;

    return at_channel_exec(channel, command, &config, NULL, 0);
}

/**
 * @brief 执行返回普通文本的AT命令。
 */
static int _rg255_cmd_exec_plain_text(at_channel_t *channel, const char *command, int timeout_ms, char *response, int response_size)
{
    at_command_config_t config;

    if (channel == NULL)
    {
        return -EINVAL;
    }

    if (command == NULL)
    {
        return -EINVAL;
    }

    if (response == NULL)
    {
        return -EINVAL;
    }

    if (response_size <= 0)
    {
        return -EINVAL;
    }

    memset(&config, 0, sizeof(config));
    config.timeout_ms = timeout_ms;
    config.accept_plain_text = true;

    return at_channel_exec(channel, command, &config, response, response_size);
}

/**
 * @brief 执行支持命令级续行匹配的AT查询命令。
 */
static int _rg255_cmd_exec_query_ex(at_channel_t *channel, const char *command, int timeout_ms, const char *expect_prefix, at_response_continuation_match_t continuation_match, unsigned int continuation_max_lines, char *response, int response_size)
{
    at_command_config_t config;

    if (channel == NULL)
    {
        return -EINVAL;
    }

    if (command == NULL)
    {
        return -EINVAL;
    }

    if (expect_prefix == NULL)
    {
        return -EINVAL;
    }

    if (response == NULL)
    {
        return -EINVAL;
    }

    if (response_size <= 0)
    {
        return -EINVAL;
    }

    if (continuation_match == NULL && continuation_max_lines != 0U)
    {
        return -EINVAL;
    }

    memset(&config, 0, sizeof(config));
    config.timeout_ms = timeout_ms;
    config.expect_prefix = expect_prefix;
    config.continuation_match = continuation_match;
    config.continuation_max_lines = continuation_max_lines;

    return at_channel_exec(channel, command, &config, response, response_size);
}

/**
 * @brief 执行返回指定前缀响应的AT查询命令。
 */
static int _rg255_cmd_exec_query(at_channel_t *channel, const char *command, int timeout_ms, const char *expect_prefix, char *response, int response_size)
{
    return _rg255_cmd_exec_query_ex(channel, command, timeout_ms, expect_prefix, NULL, 0U, response, response_size);
}

/**
 * @brief 判断netmaskset IPv6响应行是否为厂家定义的无前缀续行。
 *
 * @note 该函数只识别续行结构 `,"<field>"`，不解析字段业务含义。
 */
static bool _rg255_cmd_match_netmaskset_ipv6_continuation(const char *line)
{
    size_t index;
    size_t length;

    if (line == NULL)
    {
        return false;
    }

    length = strlen(line);

    if (length < 3U ||
        line[0] != ',' ||
        line[1] != '"' ||
        line[length - 1U] != '"')
    {
        return false;
    }

    for (index = 2U; index + 1U < length; index++)
    {
        if (line[index] == ',' || line[index] == '"')
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 检查snprintf结果是否完整写入命令缓存。
 */
static int _rg255_cmd_check_format_result(int length, size_t buffer_size)
{
    if (length < 0)
    {
        return -EIO;
    }

    if ((size_t)length >= buffer_size)
    {
        return -EMSGSIZE;
    }

    return 0;
}

/**
 * @brief 判断字符是否允许出现在APN中。
 */
static bool _rg255_cmd_is_apn_char(unsigned char value)
{
    if (value >= 'a' && value <= 'z')
    {
        return true;
    }

    if (value >= 'A' && value <= 'Z')
    {
        return true;
    }

    if (value >= '0' && value <= '9')
    {
        return true;
    }

    if (value == '-' || value == '.')
    {
        return true;
    }

    return false;
}

/**
 * @brief 校验APN是否可以安全写入AT命令。
 */
static int _rg255_cmd_validate_apn(const char *apn)
{
    const unsigned char *cursor;
    size_t length;

    if (apn == NULL)
    {
        return -EINVAL;
    }

    if (apn[0] == '\0')
    {
        return -EINVAL;
    }

    length = strlen(apn);

    if (length > LINKG_CELLULAR_APN_MAX || length > RG255_APN_MAX_LENGTH)
    {
        return -EMSGSIZE;
    }

    cursor = (const unsigned char *)apn;

    while (*cursor != '\0')
    {
        if (!_rg255_cmd_is_apn_char(*cursor))
        {
            return -EINVAL;
        }

        cursor++;
    }

    return 0;
}

/**
 * @brief 校验SIM PIN格式。
 */
static int _rg255_cmd_validate_pin(const char *pin)
{
    size_t index;
    size_t length;

    if (pin == NULL)
    {
        return -EINVAL;
    }

    length = strlen(pin);

    if (length < LINKG_CELLULAR_PIN_MIN || length > LINKG_CELLULAR_PIN_MAX)
    {
        return -EINVAL;
    }

    for (index = 0U; index < length; index++)
    {
        if (pin[index] < '0' || pin[index] > '9')
        {
            return -EINVAL;
        }
    }

    return 0;
}

/**
 * @brief 校验IMSI文本格式。
 */
static int _rg255_cmd_validate_imsi(const char *imsi)
{
    size_t index;
    size_t length;

    if (imsi == NULL)
    {
        return -EINVAL;
    }

    length = strlen(imsi);

    if (length == 0U || length > RG255_IMSI_MAX_LENGTH)
    {
        return -EBADMSG;
    }

    for (index = 0U; index < length; index++)
    {
        if (imsi[index] < '0' || imsi[index] > '9')
        {
            return -EBADMSG;
        }
    }

    return 0;
}

/****************************** 基础命令 ******************************/

/**
 * @brief 检测RG255 AT通信是否正常。
 */
int rg255_cmd_test(at_channel_t *channel)
{
    return _rg255_cmd_exec(channel, "AT", RG255_CMD_TIMEOUT_DEFAULT_MS);
}

/**
 * @brief 设置RG255 AT命令Echo状态。
 */
int rg255_cmd_set_echo(at_channel_t *channel, bool enable)
{
    if (enable)
    {
        return _rg255_cmd_exec(channel, "ATE1", RG255_CMD_TIMEOUT_DEFAULT_MS);
    }

    return _rg255_cmd_exec(channel, "ATE0", RG255_CMD_TIMEOUT_DEFAULT_MS);
}

/**
 * @brief 开启RG255详细AT错误信息。
 */
int rg255_cmd_enable_cmee(at_channel_t *channel)
{
    return _rg255_cmd_exec(channel, "AT+CMEE=2", RG255_CMD_TIMEOUT_DEFAULT_MS);
}

/**
 * @brief 关闭RG255自动休眠。
 */
int rg255_cmd_disable_sleep(at_channel_t *channel)
{
    return _rg255_cmd_exec(channel, "AT+QSCLK=0", RG255_CMD_TIMEOUT_DEFAULT_MS);
}

/****************************** SIM接口 ******************************/

/**
 * @brief 查询当前SIM PIN及可用状态。
 */
int rg255_cmd_query_sim_pin_status(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+CPIN?", RG255_CMD_TIMEOUT_PIN_MS, "+CPIN:", response, response_size);
}

/**
 * @brief 输入SIM PIN码。
 */
int rg255_cmd_enter_pin(at_channel_t *channel, const char *pin)
{
    char command[RG255_CMD_BUFFER_SIZE];
    int length;
    int ret;

    ret = _rg255_cmd_validate_pin(pin);

    if (ret != 0)
    {
        return ret;
    }

    length = snprintf(command, sizeof(command), "AT+CPIN=\"%s\"", pin);
    ret = _rg255_cmd_check_format_result(length, sizeof(command));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_cmd_exec(channel, command, RG255_CMD_TIMEOUT_PIN_MS);
}

/**
 * @brief 获取当前SIM卡IMSI。
 */
int rg255_cmd_get_imsi(at_channel_t *channel, char *imsi, int imsi_size)
{
    int ret;

    if (imsi == NULL)
    {
        return -EINVAL;
    }

    if (imsi_size < (int)RG255_IMSI_BUFFER_SIZE)
    {
        return -EINVAL;
    }

    imsi[0] = '\0';

    ret = _rg255_cmd_exec_plain_text(channel, "AT+CIMI", RG255_CMD_TIMEOUT_QUERY_MS, imsi, imsi_size);

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_cmd_validate_imsi(imsi);
}

/**
 * @brief 查询SIM插拔检测配置。
 */
int rg255_cmd_query_sim_detect(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+QSIMDET?", RG255_CMD_TIMEOUT_DEFAULT_MS, "+QSIMDET:", response, response_size);
}

/**
 * @brief 设置SIM插拔检测状态及插入有效电平。
 *
 * @note 配置自动保存，模块重启后生效。
 */
int rg255_cmd_set_sim_detect(at_channel_t *channel, bool enable, rg255_sim_insert_level_t insert_level)
{
    char command[RG255_CMD_BUFFER_SIZE];
    int state;
    int length;
    int ret;

    if (insert_level != RG255_SIM_INSERT_LEVEL_LOW &&
        insert_level != RG255_SIM_INSERT_LEVEL_HIGH)
    {
        return -EINVAL;
    }

    state = enable ? 1 : 0;
    length = snprintf(command, sizeof(command), "AT+QSIMDET=%d,%d", state, (int)insert_level);
    ret = _rg255_cmd_check_format_result(length, sizeof(command));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_cmd_exec(channel, command, RG255_CMD_TIMEOUT_CONFIG_MS);
}

/**
 * @brief 查询SIM状态URC配置及当前插拔状态。
 */
int rg255_cmd_query_sim_status_urc(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+QSIMSTAT?", RG255_CMD_TIMEOUT_DEFAULT_MS, "+QSIMSTAT:", response, response_size);
}

/**
 * @brief 设置SIM插拔状态URC上报。
 *
 * @note 配置自动保存，模块重启后生效。
 */
int rg255_cmd_set_sim_status_urc(at_channel_t *channel, bool enable)
{
    if (enable)
    {
        return _rg255_cmd_exec(channel, "AT+QSIMSTAT=1", RG255_CMD_TIMEOUT_CONFIG_MS);
    }

    return _rg255_cmd_exec(channel, "AT+QSIMSTAT=0", RG255_CMD_TIMEOUT_CONFIG_MS);
}

/****************************** 网络模式 ******************************/

/**
 * @brief 查询RG255当前网络搜索模式。
 */
int rg255_cmd_query_network_mode(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+QNWPREFCFG=\"mode_pref\"", RG255_CMD_TIMEOUT_NETWORK_MODE_MS, "+QNWPREFCFG:", response, response_size);
}

/**
 * @brief 设置RG255网络搜索模式。
 */
int rg255_cmd_set_network_mode(at_channel_t *channel, linkg_cellular_network_mode_t mode)
{
    const char *mode_name;
    char command[RG255_CMD_BUFFER_SIZE];
    int length;
    int ret;

    switch (mode)
    {
        case LINKG_CELLULAR_NETWORK_MODE_AUTO:
            mode_name = "AUTO";
            break;

        case LINKG_CELLULAR_NETWORK_MODE_4G:
            mode_name = "LTE";
            break;

        case LINKG_CELLULAR_NETWORK_MODE_5G:
            mode_name = "NR5G-SA";
            break;

        default:
            return -EINVAL;
    }

    length = snprintf(command, sizeof(command), "AT+QNWPREFCFG=\"mode_pref\",%s", mode_name);
    ret = _rg255_cmd_check_format_result(length, sizeof(command));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_cmd_exec(channel, command, RG255_CMD_TIMEOUT_NETWORK_MODE_MS);
}

/****************************** 网络注册 ******************************/

/**
 * @brief 查询EPS网络注册状态。
 */
int rg255_cmd_query_eps_registration(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+CEREG?", RG255_CMD_TIMEOUT_DEFAULT_MS, "+CEREG:", response, response_size);
}

/**
 * @brief 查询5GS网络注册状态。
 */
int rg255_cmd_query_5g_registration(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+C5GREG?", RG255_CMD_TIMEOUT_DEFAULT_MS, "+C5GREG:", response, response_size);
}

/****************************** 无线状态 ******************************/

/**
 * @brief 查询当前服务小区及无线质量信息。
 */
int rg255_cmd_query_serving_cell(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+QENG=\"servingcell\"", RG255_CMD_TIMEOUT_QUERY_MS, "+QENG:", response, response_size);
}

/****************************** USB配置 ******************************/

/**
 * @brief 查询当前USB网络模式。
 */
int rg255_cmd_query_usbnet(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+QCFG=\"usbnet\"", RG255_CMD_TIMEOUT_DEFAULT_MS, "+QCFG:", response, response_size);
}

/**
 * @brief 设置USB网络模式。
 */
int rg255_cmd_set_usbnet(at_channel_t *channel, rg255_usbnet_mode_t mode)
{
    char command[RG255_CMD_BUFFER_SIZE];
    int length;
    int ret;

    if (mode == RG255_USBNET_MODE_MBIM)
    {
        return -EOPNOTSUPP;
    }

    if (mode != RG255_USBNET_MODE_ECM && mode != RG255_USBNET_MODE_RNDIS)
    {
        return -EINVAL;
    }

    length = snprintf(command, sizeof(command), "AT+QCFG=\"usbnet\",%d", (int)mode);
    ret = _rg255_cmd_check_format_result(length, sizeof(command));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_cmd_exec(channel, command, RG255_CMD_TIMEOUT_CONFIG_MS);
}

/**
 * @brief 查询当前USB网卡工作模式。
 */
int rg255_cmd_query_network_card_mode(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+QCFG=\"nat\"", RG255_CMD_TIMEOUT_DEFAULT_MS, "+QCFG:", response, response_size);
}

/**
 * @brief 设置USB网卡工作模式。
 */
int rg255_cmd_set_network_card_mode(at_channel_t *channel, rg255_network_card_mode_t mode)
{
    char command[RG255_CMD_BUFFER_SIZE];
    int length;
    int ret;

    if (mode != RG255_NETWORK_CARD_MODE_ROUTER && mode != RG255_NETWORK_CARD_MODE_NIC)
    {
        return -EINVAL;
    }

    length = snprintf(command, sizeof(command), "AT+QCFG=\"nat\",%d", (int)mode);
    ret = _rg255_cmd_check_format_result(length, sizeof(command));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_cmd_exec(channel, command, RG255_CMD_TIMEOUT_CONFIG_MS);
}

/**
 * @brief 查询网卡模式下RG255提供给Host的IPv4网络参数。
 *
 * @note 该命令需要在QNETDEV拨号成功后执行。
 */
int rg255_cmd_query_network_card_ipv4(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+QCFG=\"netmaskset\",2", RG255_CMD_TIMEOUT_DEFAULT_MS, "+QCFG:", response, response_size);
}

/**
 * @brief 查询网卡模式下RG255提供给Host的IPv6网络参数。
 *
 * @note RG255AA实机确认该命令可能将最后一个字段拆为不重复+QCFG前缀的第二行，
 *       因此仅此命令启用命令级响应续行匹配。
 */
int rg255_cmd_query_network_card_ipv6(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query_ex(channel,
                                    "AT+QCFG=\"netmaskset\",3",
                                    RG255_CMD_TIMEOUT_DEFAULT_MS,
                                    "+QCFG:",
                                    _rg255_cmd_match_netmaskset_ipv6_continuation,
                                    RG255_CMD_CONTINUATION_MAX_NETMASKSET,
                                    response,
                                    response_size);
}

/****************************** PDP配置 ******************************/

/**
 * @brief 查询当前PDP上下文配置。
 */
int rg255_cmd_query_pdp_config(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+CGDCONT?", RG255_CMD_TIMEOUT_QUERY_MS, "+CGDCONT:", response, response_size);
}

/**
 * @brief 配置默认PDP上下文。
 */
int rg255_cmd_set_pdp_context(at_channel_t *channel, const char *apn)
{
    char command[RG255_CMD_BUFFER_SIZE];
    int length;
    int ret;

    ret = _rg255_cmd_validate_apn(apn);

    if (ret != 0)
    {
        return ret;
    }

    length = snprintf(command, sizeof(command), "AT+CGDCONT=%d,\"IPV4V6\",\"%s\"", RG255_PDP_CONTEXT_ID, apn);
    ret = _rg255_cmd_check_format_result(length, sizeof(command));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_cmd_exec(channel, command, RG255_CMD_TIMEOUT_CONFIG_MS);
}

/**
 * @brief 查询当前PDP上下文激活状态。
 */
int rg255_cmd_query_pdp_state(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+CGACT?", RG255_CMD_TIMEOUT_QUERY_MS, "+CGACT:", response, response_size);
}

/**
 * @brief 设置默认PDP上下文激活状态。
 */
int rg255_cmd_set_pdp_active(at_channel_t *channel, bool active)
{
    char command[RG255_CMD_BUFFER_SIZE];
    int state;
    int length;
    int ret;

    state = active ? 1 : 0;
    length = snprintf(command, sizeof(command), "AT+CGACT=%d,%d", state, RG255_PDP_CONTEXT_ID);
    ret = _rg255_cmd_check_format_result(length, sizeof(command));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_cmd_exec(channel, command, RG255_CMD_TIMEOUT_PDP_ACTIVE_MS);
}

/**
 * @brief 查询默认PDP上下文地址信息。
 */
int rg255_cmd_query_pdp_address(at_channel_t *channel, char *response, int response_size)
{
    char command[RG255_CMD_BUFFER_SIZE];
    int length;
    int ret;

    length = snprintf(command, sizeof(command), "AT+CGPADDR=%d", RG255_PDP_CONTEXT_ID);
    ret = _rg255_cmd_check_format_result(length, sizeof(command));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_cmd_exec_query(channel, command, RG255_CMD_TIMEOUT_QUERY_MS, "+CGPADDR:", response, response_size);
}

/**
 * @brief 查询默认PDP上下文运行参数。
 */
int rg255_cmd_query_pdp_runtime(at_channel_t *channel, char *response, int response_size)
{
    char command[RG255_CMD_BUFFER_SIZE];
    int length;
    int ret;

    length = snprintf(command, sizeof(command), "AT+CGCONTRDP=%d", RG255_PDP_CONTEXT_ID);
    ret = _rg255_cmd_check_format_result(length, sizeof(command));

    if (ret != 0)
    {
        return ret;
    }

    return _rg255_cmd_exec_query(channel, command, RG255_CMD_TIMEOUT_QUERY_MS, "+CGCONTRDP:", response, response_size);
}

/****************************** 网络设备 ******************************/

/**
 * @brief 启动RG255 USB网络设备拨号。
 */
int rg255_cmd_start_netdev(at_channel_t *channel)
{
    return _rg255_cmd_exec(channel, "AT+QNETDEVCTL=1,1,1", RG255_CMD_TIMEOUT_NETDEV_MS);
}

/**
 * @brief 停止RG255 USB网络设备拨号。
 */
int rg255_cmd_stop_netdev(at_channel_t *channel)
{
    return _rg255_cmd_exec(channel, "AT+QNETDEVCTL=0,1,0", RG255_CMD_TIMEOUT_NETDEV_MS);
}

/**
 * @brief 查询RG255 USB网络设备状态。
 */
int rg255_cmd_query_netdev(at_channel_t *channel, char *response, int response_size)
{
    return _rg255_cmd_exec_query(channel, "AT+QNETDEVCTL?", RG255_CMD_TIMEOUT_QUERY_MS, "+QNETDEVCTL:", response, response_size);
}

/**
 * @brief 启用RG255 USB网络设备自动保持模式。
 */
int rg255_cmd_enable_netdev_auto_keep(at_channel_t *channel)
{
    return _rg255_cmd_exec(channel, "AT+QNETDEVCTL=3,1,1", RG255_CMD_TIMEOUT_NETDEV_MS);
}

/****************************** 模块控制 ******************************/

/**
 * @brief 重启RG255模块。
 *
 * @note 命令成功发送后AT端口会重新枚举，调用方负责关闭旧通道并等待设备重新就绪。
 */
int rg255_cmd_restart(at_channel_t *channel)
{
    return _rg255_cmd_exec(channel, "AT+CFUN=1,1", RG255_CMD_TIMEOUT_RESTART_MS);
}

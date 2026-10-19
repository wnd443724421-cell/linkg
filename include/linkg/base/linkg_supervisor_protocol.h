/**
 * @file linkg_supervisor_protocol.h
 * @brief Supervisor匿名管道通知协议
 */

#ifndef LINKG_SUPERVISOR_PROTOCOL_H
#define LINKG_SUPERVISOR_PROTOCOL_H

/****************************** 模块常量 ******************************/

#define LINKG_SUPERVISOR_FD_ENV       "LINKG_SUPERVISOR_FD" // 仅初始exec继承的通知描述符
#define LINKG_SUPERVISOR_MESSAGE_SIZE 1U                    // 单字节消息，无字节序和结构体填充

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_SUPERVISOR_READY      = 1, // 服务已进入RUN阶段
    LINKG_SUPERVISOR_HEARTBEAT  = 2, // RUN阶段基本活性通知
    LINKG_SUPERVISOR_RESTARTING = 3, // 同一进程开始重启服务
    LINKG_SUPERVISOR_STOPPING   = 4  // 主动关闭通知通道并清理进程
} linkg_supervisor_message_t;

#endif

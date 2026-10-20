
/**
 * @file linkg_upgrade.h
 * @brief LinkG升级安装模块接口
 */

#ifndef LINKG_UPGRADE_H
#define LINKG_UPGRADE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 模块常量 ******************************/

#define LINKG_UPGRADE_MESSAGE_MAX 128U // 升级状态描述最大长度，包含字符串结束符

/****************************** 升级任务状态 ******************************/

typedef enum
{
    LINKG_UPGRADE_STATE_IDLE           = 0, // 空闲，尚未接受升级任务
    LINKG_UPGRADE_STATE_PENDING,            // 已接受任务，等待后台执行
    LINKG_UPGRADE_STATE_VERIFYING,          // 正在校验升级包
    LINKG_UPGRADE_STATE_INSTALLING,         // 正在安装升级文件
    LINKG_UPGRADE_STATE_PREPARING,          // 正在提交候选JSON与ENV
    LINKG_UPGRADE_STATE_REBOOT_PENDING,     // 候选已提交，等待重启，不代表最终升级成功
    LINKG_UPGRADE_STATE_FAILED              // 本次升级安装失败
} linkg_upgrade_state_t;

/****************************** 升级处理阶段 ******************************/

typedef enum
{
    LINKG_UPGRADE_STAGE_NONE               = 0, // 尚未进入处理阶段
    LINKG_UPGRADE_STAGE_ARCHIVE_VERIFY,         // 校验归档结构及文件类型
    LINKG_UPGRADE_STAGE_MANIFEST_VERIFY,        // 校验FILES.txt与归档文件全集
    LINKG_UPGRADE_STAGE_TARGET_PREPARE,         // 确定并准备非稳定Release
    LINKG_UPGRADE_STAGE_APP_INSTALL,            // 安装App文件
    LINKG_UPGRADE_STAGE_WEB_INSTALL,            // 安装Web文件
    LINKG_UPGRADE_STAGE_KERNEL_COMPARE,         // 比较Kernel镜像
    LINKG_UPGRADE_STAGE_KERNEL_WRITE,           // 写入非稳定Kernel槽位
    LINKG_UPGRADE_STAGE_KERNEL_VERIFY,          // 读回校验Kernel镜像
    LINKG_UPGRADE_STAGE_SYSTEM_INSTALL,         // 根据可选声明替换系统文件
    LINKG_UPGRADE_STAGE_SUPERVISOR_PREPARE      // 提交候选JSON与ENV
} linkg_upgrade_stage_t;

/****************************** 状态快照 ******************************/

typedef struct
{
    linkg_upgrade_state_t state;                              // 当前任务状态
    linkg_upgrade_stage_t stage;                              // 当前处理阶段，失败时保留失败阶段
    uint32_t              task_id;                            // 本进程内的任务编号，0表示尚无任务
    uint8_t               progress;                           // 本地安装进度0至100，不包含候选试运行
    int                   error_code;                         // 安装失败错误码，非失败状态为0
    char                  message[LINKG_UPGRADE_MESSAGE_MAX]; // 状态描述或错误信息，以\0结束
} linkg_upgrade_status_t;

/****************************** 生命周期 ******************************/

int linkg_upgrade_init(void);
int linkg_upgrade_start(void);
int linkg_upgrade_stop(void);
int linkg_upgrade_deinit(void);

/****************************** 升级任务 ******************************/

int linkg_upgrade_request_install(void);

/****************************** 状态查询 ******************************/

int linkg_upgrade_get_status(linkg_upgrade_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // LINKG_UPGRADE_H

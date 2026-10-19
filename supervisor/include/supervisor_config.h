/**
 * @file supervisor_config.h
 * @brief Supervisor产品参数与隔离测试参数
 */

#ifndef SUPERVISOR_CONFIG_H
#define SUPERVISOR_CONFIG_H

/****************************** 模块常量 ******************************/

#define SUPERVISOR_DEFAULT_PROGRAM    "/app/current/bin/linkg"      // 当前版本可执行文件
#define SUPERVISOR_DEFAULT_LOCK_FILE  "/run/linkg-supervisor.lock"  // 防止重复启动Supervisor

#ifdef LINKG_SUPERVISOR_TEST_BUILD
#define SUPERVISOR_START_TIMEOUT_MS   600U                          // 测试启动窗口
#define SUPERVISOR_HEARTBEAT_MS       400U                          // 测试心跳窗口
#define SUPERVISOR_RESTART_TIMEOUT_MS 900U                          // 测试重启和清理窗口
#define SUPERVISOR_TERMINATE_MS       200U                          // 测试TERM等待窗口
#define SUPERVISOR_RETRY_INITIAL_MS   100U                          // 测试初始退避
#define SUPERVISOR_RETRY_MAX_MS       800U                          // 测试最大退避
#define SUPERVISOR_STABLE_MS          700U                          // 测试稳定窗口
#define SUPERVISOR_POLL_MS            20U                           // 测试轮询上限
#else
#define SUPERVISOR_START_TIMEOUT_MS   60000U                        // 首次READY期限，单位毫秒
#define SUPERVISOR_HEARTBEAT_MS       4000U                         // RUN心跳期限，单位毫秒
#define SUPERVISOR_RESTART_TIMEOUT_MS 60000U                        // 单次重启和清理期限，单位毫秒
#define SUPERVISOR_TERMINATE_MS       2000U                         // SIGTERM宽限，单位毫秒
#define SUPERVISOR_RETRY_INITIAL_MS   1000U                         // 连续故障初始退避，单位毫秒
#define SUPERVISOR_RETRY_MAX_MS       30000U                        // 连续故障最大退避，单位毫秒
#define SUPERVISOR_STABLE_MS          60000U                        // 连续RUN后清除故障计数
#define SUPERVISOR_POLL_MS            100U                          // waitpid与信号响应轮询上限
#endif

#endif

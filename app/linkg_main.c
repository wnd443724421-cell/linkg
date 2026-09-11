/**
 * @file linkg_main.c
 * @brief LinkG应用程序入口
 * @author Dawn
 * @version 1.2.1
 * @date 2026-09-11
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

#include "linkg_config.h"
#include "linkg_discovery.h"
#include "linkg_lifecycle.h"
#include "linkg_link_manager.h"
#include "linkg_log.h"
#include "linkg_nat.h"
#include "linkg_network.h"
#include "linkg_node.h"
#include "linkg_packet_pool.h"
#include "linkg_route.h"
#include "linkg_scheduler.h"
#include "linkg_switch.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"
#include "linkg_transport.h"
#include "linkg_tun.h"
#include "linkg_udhcp.h"

/****************************** 应用资源 ******************************/

#define LINKG_APP_PACKET_POOL_COUNT 2048U // 全局数据包数量
#define LINKG_APP_PACKET_SLOT_SIZE  2048U // 单个数据包槽位大小
#define LINKG_APP_PACKET_HEADROOM   64U   // Transport等协议头预留空间

_Static_assert(LINKG_APP_PACKET_SLOT_SIZE > LINKG_APP_PACKET_HEADROOM, "invalid application packet pool layout");
_Static_assert(LINKG_APP_PACKET_HEADROOM >= LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE, "application packet headroom is too small");
_Static_assert(LINKG_APP_PACKET_SLOT_SIZE - LINKG_APP_PACKET_HEADROOM >= LINKG_RESOURCE_TUN_MTU, "application packet pool capacity is too small");

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_packet_pool_t packet_pool;                 // 全局共享Packet Pool
    bool                config_loaded;               // 全局配置是否已经加载
    bool                packet_pool_initialized;     // Packet Pool是否已经初始化
    bool                network_initialized;         // Network Service是否已经初始化
    bool                link_manager_initialized;    // Link Manager是否已经初始化
    bool                node_initialized;            // Node模块是否已经初始化
    bool                switch_initialized;          // Switch模块是否已经初始化
    bool                scheduler_initialized;       // Scheduler模块是否已经初始化
    bool                transport_initialized;       // Transport模块是否已经初始化
    bool                tun_initialized;             // TUN模块是否已经初始化
    bool                route_initialized;           // Route模块是否已经初始化
    bool                nat_initialized;             // NAT模块是否已经初始化
    bool                discovery_initialized;       // Discovery模块是否已经初始化
    bool                udhcp_initialized;           // UDHCP模块是否已经初始化
    bool                network_started;             // Network Service是否进入过start生命周期
    bool                link_manager_started;        // Link Manager是否进入过start生命周期
    bool                tun_started;                 // TUN模块是否进入过start生命周期
    bool                route_started;               // Route模块是否进入过start生命周期
    bool                nat_started;                 // NAT模块是否进入过start生命周期
    bool                discovery_started;           // Discovery模块是否进入过start生命周期
    bool                udhcp_started;               // UDHCP模块是否进入过start生命周期
} linkg_app_context_t;

/****************************** 全局上下文 ******************************/

static linkg_app_context_t g_app;

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断是否存在已经初始化的应用模块。
 */
static bool _linkg_app_has_initialized_modules(void)
{
    return g_app.config_loaded ||
           g_app.packet_pool_initialized ||
           g_app.network_initialized ||
           g_app.link_manager_initialized ||
           g_app.node_initialized ||
           g_app.switch_initialized ||
           g_app.scheduler_initialized ||
           g_app.transport_initialized ||
           g_app.tun_initialized ||
           g_app.route_initialized ||
           g_app.nat_initialized ||
           g_app.discovery_initialized ||
           g_app.udhcp_initialized;
}

/**
 * @brief 判断是否存在已经进入start生命周期的应用模块。
 */
static bool _linkg_app_has_started_modules(void)
{
    return g_app.network_started ||
           g_app.link_manager_started ||
           g_app.tun_started ||
           g_app.route_started ||
           g_app.nat_started ||
           g_app.discovery_started ||
           g_app.udhcp_started;
}

/**
 * @brief 根据当前配置构造本机Node信息。
 */
static int _linkg_app_build_local_node(const linkg_config_t *config, linkg_node_info_t *node)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    if (node == NULL)
    {
        return -EINVAL;
    }

    memset(node, 0, sizeof(*node));

    node->node_id = config->network.node_id;
    node->role    = config->device.role;

    return 0;
}

/****************************** 应用初始化 ******************************/

/**
 * @brief 初始化全部应用模块。
 *
 * 本阶段只建立模块初始化资源，不启动任何具有start生命周期的模块。
 */
static int _linkg_app_init(void)
{
    linkg_link_manager_config_t link_manager_config;
    linkg_packet_pool_config_t  packet_pool_config;
    linkg_node_info_t           local_node;
    linkg_config_t              config;
    int                         ret;

    if (_linkg_app_has_initialized_modules() ||
        _linkg_app_has_started_modules())
    {
        return -EALREADY;
    }

    ret = linkg_config_load(NULL);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("load configuration failed, error=%d", ret);
        return ret;
    }

    g_app.config_loaded = true;

    memset(&config, 0, sizeof(config));

    ret = linkg_config_create_snapshot(&config);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("create configuration snapshot failed, error=%d", ret);
        return ret;
    }

    memset(&packet_pool_config, 0, sizeof(packet_pool_config));

    packet_pool_config.packet_count = LINKG_APP_PACKET_POOL_COUNT;
    packet_pool_config.slot_size    = LINKG_APP_PACKET_SLOT_SIZE;
    packet_pool_config.headroom     = LINKG_APP_PACKET_HEADROOM;

    ret = linkg_packet_pool_init(&g_app.packet_pool, &packet_pool_config);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize packet pool failed, error=%d", ret);
        return ret;
    }

    g_app.packet_pool_initialized = true;

    ret = linkg_network_init();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize network service failed, error=%d", ret);
        return ret;
    }

    g_app.network_initialized = true;

    memset(&link_manager_config, 0, sizeof(link_manager_config));

    link_manager_config.packet_pool = &g_app.packet_pool;

    ret = linkg_link_manager_init(&link_manager_config);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize link manager failed, error=%d", ret);
        return ret;
    }

    g_app.link_manager_initialized = true;

    ret = _linkg_app_build_local_node(&config, &local_node);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("build local node failed, error=%d", ret);
        return ret;
    }

    ret = linkg_node_init(&local_node);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize node module failed, error=%d", ret);
        return ret;
    }

    g_app.node_initialized = true;

    ret = linkg_switch_init();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize switch module failed, error=%d", ret);
        return ret;
    }

    g_app.switch_initialized = true;

    // Transport先初始化并在Link启动前注册Link Manager统一接收回调。
    ret = linkg_transport_init();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize transport module failed, error=%d", ret);
        return ret;
    }

    g_app.transport_initialized = true;

    // Scheduler依赖Transport注册中继回调，因此必须在Transport初始化完成后初始化。
    ret = linkg_scheduler_init();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize scheduler module failed, error=%d", ret);
        return ret;
    }

    g_app.scheduler_initialized = true;

    // TUN初始化时向Transport注册USER_DATA本机交付回调，并通过Scheduler提交发送数据。
    ret = linkg_tun_init(&g_app.packet_pool);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize TUN module failed, error=%d", ret);
        return ret;
    }

    g_app.tun_initialized = true;

    ret = linkg_route_init(&config.network);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize route module failed, error=%d", ret);
        return ret;
    }

    g_app.route_initialized = true;

    ret = linkg_nat_init(&config.network, config.links.cellular.enabled);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize NAT module failed, error=%d", ret);
        return ret;
    }

    g_app.nat_initialized = true;

    // Discovery内部统一管理当前配置启用的Wi-Fi和Cellular Channel。
    ret = linkg_discovery_init();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize discovery module failed, error=%d", ret);
        return ret;
    }

    g_app.discovery_initialized = true;

    ret = linkg_udhcp_init(&config.network);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize UDHCP module failed, error=%d", ret);
        return ret;
    }

    g_app.udhcp_initialized = true;

    LINKG_LOG_INFO("application modules initialized");

    return 0;
}

/****************************** 应用启动 ******************************/

/**
 * @brief 启动全部具有运行态的应用模块。
 *
 * 启动顺序严格按照运行依赖建立：
 * Network -> Link Manager -> TUN -> Route -> NAT -> Discovery -> UDHCP。
 */
static int _linkg_app_start(void)
{
    int ret;

    if (!g_app.config_loaded ||
        !g_app.packet_pool_initialized ||
        !g_app.network_initialized ||
        !g_app.link_manager_initialized ||
        !g_app.node_initialized ||
        !g_app.switch_initialized ||
        !g_app.scheduler_initialized ||
        !g_app.transport_initialized ||
        !g_app.tun_initialized ||
        !g_app.route_initialized ||
        !g_app.nat_initialized ||
        !g_app.discovery_initialized ||
        !g_app.udhcp_initialized)
    {
        return -ENODEV;
    }

    if (_linkg_app_has_started_modules())
    {
        return -EALREADY;
    }

    /**
     * started标志表示模块已经进入过start生命周期，
     * 即使start中途失败，cleanup仍然会调用对应stop完成兜底清理。
     */

    g_app.network_started = true;

    ret = linkg_network_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start network service failed, error=%d", ret);
        return ret;
    }

    /**
     * Transport已经在初始化阶段注册统一接收回调，
     * 此时才允许业务Link启动收发线程。
     */
    g_app.link_manager_started = true;

    ret = linkg_link_manager_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start link manager failed, error=%d", ret);
        return ret;
    }

    /**
     * Route和NAT都依赖linkg0运行资源，
     * 因此必须首先启动TUN。
     */
    g_app.tun_started = true;

    ret = linkg_tun_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start TUN module failed, error=%d", ret);
        return ret;
    }

    g_app.route_started = true;

    ret = linkg_route_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start route module failed, error=%d", ret);
        return ret;
    }

    /**
     * Network已经完成Ethernet配置和IPv4 forwarding，
     * linkg0此时也已经建立，可以安装NAT规则。
     */
    g_app.nat_started = true;

    ret = linkg_nat_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start NAT module failed, error=%d", ret);
        return ret;
    }

    /**
     * Discovery在DHCP之前启动。
     * 从此刻开始Peer上线流程才允许创建Node、Transport、Path和Linux Route状态。
     */
    g_app.discovery_started = true;

    ret = linkg_discovery_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start discovery module failed, error=%d", ret);
        return ret;
    }

    /**
     * UDHCP最后启动。
     * 此时Ethernet、TUN、Route、NAT和Discovery均已经进入运行态，
     * DHCP Client一旦获取地址和虚拟网络路由即可直接使用完整LinkG数据面。
     */
    g_app.udhcp_started = true;

    ret = linkg_udhcp_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start UDHCP module failed, error=%d", ret);
        return ret;
    }

    LINKG_LOG_INFO("application modules started");

    return 0;
}

/****************************** 应用停止 ******************************/

/**
 * @brief 停止全部运行模块。
 *
 * 按运行依赖反序停止。
 * 任一关键模块停止失败后保留下层依赖，不继续执行不安全的资源拆除。
 */
static int _linkg_app_stop(void)
{
    int ret;

    /**
     * UDHCP必须最先停止，避免应用拆除期间继续向新接入设备分配地址和路由。
     * 已经获得租约的客户端不依赖udhcpd进程继续运行。
     */
    if (g_app.udhcp_started)
    {
        ret = linkg_udhcp_stop();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("stop UDHCP module failed, error=%d", ret);
            return ret;
        }

        g_app.udhcp_started = false;
    }

    /**
     * Discovery随后停止。
     * 停止过程中仍需要Link、Route等下层资源发送LEAVE并注销Peer状态。
     */
    if (g_app.discovery_started)
    {
        ret = linkg_discovery_stop();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("stop discovery module failed, error=%d", ret);
            return ret;
        }

        g_app.discovery_started = false;
    }

    // Discovery已经删除全部Peer路由后才能关闭NAT和Route运行资源。
    if (g_app.nat_started)
    {
        ret = linkg_nat_stop();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("stop NAT module failed, error=%d", ret);
            return ret;
        }

        g_app.nat_started = false;
    }

    if (g_app.route_started)
    {
        ret = linkg_route_stop();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("stop route module failed, error=%d", ret);
            return ret;
        }

        g_app.route_started = false;
    }

    // Route已经释放linkg0接口索引和Netlink资源后才能销毁TUN运行接口。
    if (g_app.tun_started)
    {
        ret = linkg_tun_stop();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("stop TUN module failed, error=%d", ret);
            return ret;
        }

        g_app.tun_started = false;
    }

    if (g_app.link_manager_started)
    {
        ret = linkg_link_manager_stop();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("stop link manager failed, error=%d", ret);
            return ret;
        }

        g_app.link_manager_started = false;
    }

    // 所有业务Link线程退出以后再关闭底层网络接入模块。
    if (g_app.network_started)
    {
        ret = linkg_network_stop();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("stop network service failed, error=%d", ret);
            return ret;
        }

        g_app.network_started = false;
    }

    LINKG_LOG_INFO("application modules stopped");

    return 0;
}

/****************************** 应用反初始化 ******************************/

/**
 * @brief 按初始化依赖反序释放全部应用模块。
 *
 * 调用前必须完成应用停止，本函数不隐式停止运行模块。
 */
static int _linkg_app_deinit(void)
{
    int ret;

    if (_linkg_app_has_started_modules())
    {
        return -EBUSY;
    }

    if (g_app.udhcp_initialized)
    {
        ret = linkg_udhcp_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize UDHCP module failed, error=%d", ret);
            return ret;
        }

        g_app.udhcp_initialized = false;
    }

    if (g_app.discovery_initialized)
    {
        ret = linkg_discovery_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize discovery module failed, error=%d", ret);
            return ret;
        }

        g_app.discovery_initialized = false;
    }

    if (g_app.nat_initialized)
    {
        ret = linkg_nat_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize NAT module failed, error=%d", ret);
            return ret;
        }

        g_app.nat_initialized = false;
    }

    if (g_app.route_initialized)
    {
        ret = linkg_route_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize route module failed, error=%d", ret);
            return ret;
        }

        g_app.route_initialized = false;
    }

    /**
     * Link RX/TX线程已经停止，Route也不再引用linkg0，
     * 此时可以安全注销TUN Transport Handler并释放TUN软件资源。
     */
    if (g_app.tun_initialized)
    {
        ret = linkg_tun_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize TUN module failed, error=%d", ret);
            return ret;
        }

        g_app.tun_initialized = false;
    }

    /**
     * TUN已经注销本机USER_DATA交付回调，业务Link线程也已经停止。
     * Scheduler必须先注销Transport中继回调，随后才能反初始化Transport。
     */
    if (g_app.scheduler_initialized)
    {
        ret = linkg_scheduler_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize scheduler module failed, error=%d", ret);
            return ret;
        }

        g_app.scheduler_initialized = false;
    }

    if (g_app.transport_initialized)
    {
        ret = linkg_transport_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize transport module failed, error=%d", ret);
            return ret;
        }

        g_app.transport_initialized = false;
    }

    if (g_app.switch_initialized)
    {
        ret = linkg_switch_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize switch module failed, error=%d", ret);
            return ret;
        }

        g_app.switch_initialized = false;
    }

    if (g_app.node_initialized)
    {
        ret = linkg_node_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize node module failed, error=%d", ret);
            return ret;
        }

        g_app.node_initialized = false;
    }

    /**
     * Transport已经注销Link Manager接收回调，
     * 所有业务Link也已经停止，此时才能销毁Link实例。
     */
    if (g_app.link_manager_initialized)
    {
        ret = linkg_link_manager_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize link manager failed, error=%d", ret);
            return ret;
        }

        g_app.link_manager_initialized = false;
    }

    if (g_app.network_initialized)
    {
        ret = linkg_network_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize network service failed, error=%d", ret);
            return ret;
        }

        g_app.network_initialized = false;
    }

    if (g_app.packet_pool_initialized)
    {
        ret = linkg_packet_pool_deinit(&g_app.packet_pool);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize packet pool failed, error=%d", ret);
            return ret;
        }

        g_app.packet_pool_initialized = false;
    }

    if (g_app.config_loaded)
    {
        linkg_config_deinit();

        g_app.config_loaded = false;
    }

    memset(&g_app, 0, sizeof(g_app));

    LINKG_LOG_INFO("application modules deinitialized");

    return 0;
}

/****************************** 应用清理 ******************************/

/**
 * @brief 清理当前应用生命周期。
 *
 * 先停止全部已经进入运行态的模块，再执行反初始化。
 * 如果停止失败则保留下层依赖，不继续执行可能不安全的反初始化。
 */
static int _linkg_app_cleanup(void)
{
    int ret;

    if (_linkg_app_has_started_modules())
    {
        ret = _linkg_app_stop();
        if (ret != 0)
        {
            return ret;
        }
    }

    if (_linkg_app_has_initialized_modules())
    {
        ret = _linkg_app_deinit();
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** 退出动作 ******************************/

/**
 * @brief 执行最终应用退出动作。
 */
static int _linkg_app_execute_exit_action(linkg_exit_action_t action)
{
    if (action != LINKG_EXIT_ACTION_REBOOT_SYSTEM)
    {
        return EXIT_SUCCESS;
    }

    LINKG_LOG_INFO("rebooting system");

    linkg_log_flush();
    sync();

    if (reboot(RB_AUTOBOOT) != 0)
    {
        LINKG_LOG_ERROR("reboot system failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/****************************** 主程序 ******************************/

/**
 * @brief 启动LinkG应用程序。
 */
int main(void)
{
    linkg_exit_action_t action;
    int                 cleanup_ret;
    int                 shutdown_ret;
    int                 ret;

    action       = LINKG_EXIT_ACTION_NONE;
    shutdown_ret = 0;

    ret = linkg_time_init();
    if (ret != 0)
    {
        return EXIT_FAILURE;
    }

    ret = linkg_log_init(LINKG_LOG_DEFAULT_LEVEL);
    if (ret != 0)
    {
        return EXIT_FAILURE;
    }

    if (linkg_log_add_console_output(true, true) < 0)
    {
        linkg_log_deinit();
        return EXIT_FAILURE;
    }

    if (linkg_log_add_default_directory_output() < 0)
    {
        LINKG_LOG_WARN("add file log output failed, continue with console output");
    }

    ret = linkg_lifecycle_init();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("lifecycle initialization failed, error=%d", ret);

        linkg_log_deinit();

        return EXIT_FAILURE;
    }

    LINKG_LOG_INFO("LinkG application starting");

    while (true)
    {
        action       = LINKG_EXIT_ACTION_NONE;
        shutdown_ret = 0;

        /****************************** INIT ******************************/

        ret = _linkg_app_init();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("application initialization failed, error=%d", ret);

            shutdown_ret = ret;
            action       = LINKG_EXIT_ACTION_EXIT;
        }

        /****************************** START ******************************/

        if (shutdown_ret == 0)
        {
            ret = _linkg_app_start();
            if (ret != 0)
            {
                LINKG_LOG_ERROR("application startup failed, error=%d", ret);

                shutdown_ret = ret;
                action       = LINKG_EXIT_ACTION_EXIT;
            }
        }

        /****************************** RUN ******************************/

        if (shutdown_ret == 0)
        {
            LINKG_LOG_INFO("LinkG application ready");

            action = linkg_lifecycle_wait_for_exit();

            LINKG_LOG_INFO("application exit requested, action=%d", (int)action);
        }

        /****************************** CLEANUP ******************************/

        cleanup_ret = _linkg_app_cleanup();
        if (cleanup_ret != 0)
        {
            LINKG_LOG_ERROR("application cleanup failed, error=%d", cleanup_ret);

            if (shutdown_ret == 0)
            {
                shutdown_ret = cleanup_ret;
            }

            action = LINKG_EXIT_ACTION_EXIT;
        }

        if (shutdown_ret != 0 ||
            action != LINKG_EXIT_ACTION_RESTART_SERVICES)
        {
            break;
        }

        /****************************** RESTART ******************************/

        ret = linkg_lifecycle_reset();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("lifecycle reset failed, error=%d", ret);

            shutdown_ret = ret;
            action       = LINKG_EXIT_ACTION_EXIT;

            break;
        }

        LINKG_LOG_INFO("reloading configuration and restarting services");
    }

    ret = _linkg_app_execute_exit_action(action);

    if (ret == EXIT_SUCCESS &&
        shutdown_ret != 0)
    {
        ret = EXIT_FAILURE;
    }

    linkg_lifecycle_deinit();

    LINKG_LOG_INFO("LinkG application exited");

    linkg_log_flush();
    linkg_log_deinit();

    return ret;
}

/**
 * @file sunxi_gmac.c
 * @brief Allwinner Sunxi GMAC网络驱动实现
 * @author Dawn
 * @version 1.0.1
 * @date 2026-09-05
 *
 * Copyright © 2016-2018, fuzhaoke
 * Original author: fuzhaoke <fuzhaoke@allwinnertech.com>
 *
 * This file is provided under a dual BSD/GPL license. When using or
 * redistributing this file, you may do so under either license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "sunxi-gmac.h"

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/mii.h>
#include <linux/gpio.h>
#include <linux/crc32.h>
#include <linux/skbuff.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/crypto.h>
#include <crypto/algapi.h>
#include <crypto/hash.h>
#include <linux/err.h>
#include <linux/scatterlist.h>
#include <linux/regulator/consumer.h>
#include <linux/of_net.h>
#include <linux/of_gpio.h>
#include <linux/io.h>
#include <linux/timer.h>
#include <linux/hrtimer.h>
// #include <linux/sunxi-sid.h> // 原厂SID接口当前未启用
#include <linux/reset.h>

#ifdef CONFIG_RTL8363_NB
#include <smi.h>
#include <rtk8363.h>
#include <rtk_types.h>
#include <port.h>
#include <stat.h>
#include <rtk_switch.h>
#include <rtk_error.h>
#include <rtl8367c_asicdrv_port.h>
#endif

/****************************** 模块常量 ******************************/

#define DMA_DESC_RX                     256                                       // 默认RX DMA描述符数量
#define DMA_DESC_TX                     256                                       // 默认TX DMA描述符数量
#define BUDGET                          (dma_desc_rx / 4)                         // NAPI默认轮询预算
#define TX_THRESH                       (dma_desc_tx / 4)                         // TX环队列唤醒阈值
#define TX_COAL_FRAMES_DEFAULT          25                                        // 默认TX完成中断聚合帧数
#define TX_COAL_TIMER_MS                10                                        // TX回收兜底定时器周期，单位毫秒
#define RX_COAL_FRAMES_LOW              1                                         // RX低流量模式每次中断帧数
#define RX_COAL_FRAMES_HIGH             16                                        // RX高流量模式中断聚合帧数
#define RX_COAL_PPS_HIGH                3200                                      // RX切换HIGH模式PPS阈值
#define RX_COAL_PPS_LOW                 2400                                      // RX切换LOW模式PPS阈值
#define RX_COAL_TIMER_MS                10                                        // RX HIGH模式兜底定时器周期，单位毫秒
#define RX_DRAIN_TIMER_MIN_MS           2                                         // RX DRAIN最小检查周期，单位毫秒
#define RX_DRAIN_TIMER_STEP_MS          5                                         // RX DRAIN退避步长，单位毫秒
#define RX_DRAIN_TIMER_MAX_MS           20                                        // RX DRAIN最大检查周期，单位毫秒
#define RX_DRAIN_IDLE_DELAY_MS          2000                                      // RX DRAIN首次退避等待时间，单位毫秒
#define RX_DRAIN_BACKOFF_STEP_MS        5000                                      // RX DRAIN后续退避间隔，单位毫秒
#define HASH_TABLE_SIZE                 64                                        // MAC多播哈希表容量
#define MAX_BUF_SZ                      (SZ_2K - 1)                               // 单个DMA描述符最大缓冲区长度
#define POWER_CHAN_NUM                  3                                         // 外部PHY电源通道数量
#define TX_TIMEO                        5000                                      // 默认发送看门狗超时，单位毫秒
#define GETH_MAC_ADDRESS                "00:00:00:00:00:00"                       // 默认MAC地址字符串
#define INT_PHY                         0                                         // 内置PHY类型标识
#define EXT_PHY                         1                                         // 外置PHY类型标识
#define circ_cnt(head, tail, size)      (((head) > (tail)) ? \
                                        ((head) - (tail)) : \
                                        ((head) - (tail)) & ((size) - 1))         // 环形队列已使用元素数量
#define circ_space(head, tail, size)    circ_cnt((tail), ((head) + 1), (size))    // 环形队列剩余空间
#define circ_inc(n, s)                  (((n) + 1) % (s))                         // 环形队列索引递增

// 默认关闭数据包调试输出。
#undef PKT_DEBUG

// 默认关闭DMA描述符调试输出。
#undef DESC_PRINT

/****************************** 模块参数 ******************************/

static char *mac_str = GETH_MAC_ADDRESS;                            // MAC地址模块参数
module_param(mac_str, charp, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(mac_str, "MAC Address String.(xx:xx:xx:xx:xx:xx)");

static int rxmode = 1;                                              // RX DMA阈值控制模式
module_param(rxmode, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(rxmode, "DMA threshold control value");

static int txmode = 1;                                              // TX DMA阈值控制模式
module_param(txmode, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(txmode, "DMA threshold control value");

static int pause = 0x400;                                           // 流控Pause时间
module_param(pause, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(pause, "Flow Control Pause Time");

static int watchdog = TX_TIMEO;                                     // TX看门狗超时时间
module_param(watchdog, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(watchdog, "Transmit timeout in milliseconds");

static int dma_desc_rx = DMA_DESC_RX;                               // RX DMA描述符数量
module_param(dma_desc_rx, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(watchdog, "The number of receive's descriptors");

static int dma_desc_tx = DMA_DESC_TX;                               // TX DMA描述符数量
module_param(dma_desc_tx, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(watchdog, "The number of transmit's descriptors");

static unsigned int tx_coal_frames = TX_COAL_FRAMES_DEFAULT;        // TX完成中断聚合帧数
module_param(tx_coal_frames, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(tx_coal_frames, "Number of packets between TX completion interrupts");

/**
 * flow_ctrl取值：
 * 0关闭流控，1启用RX流控，2启用TX流控，3同时启用RX和TX流控。
 */
static int flow_ctrl;                                               // MAC流控模式
module_param(flow_ctrl, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(flow_ctrl, "Flow control [0: off, 1: rx, 2: tx, 3: both]");

/****************************** 内部类型 ******************************/

struct geth_priv
{
    struct dma_desc             *dma_tx;                            // TX DMA描述符环
    struct sk_buff              **tx_sk;                            // TX描述符对应SKB数组
    unsigned int                tx_clean;                           // TX已回收位置
    unsigned int                tx_dirty;                           // TX已提交位置
    unsigned int                tx_count_frames;                    // TX中断聚合累计帧数
    struct timer_list           tx_timer;                           // TX延迟回收兜底定时器
    dma_addr_t                  dma_tx_phy;                         // TX描述符环DMA物理地址

    unsigned long               buf_sz;                             // RX单个SKB缓冲区大小

    struct dma_desc             *dma_rx;                            // RX DMA描述符环
    struct sk_buff              **rx_sk;                            // RX描述符对应SKB数组
    unsigned int                rx_clean;                           // RX已补充位置
    unsigned int                rx_dirty;                           // RX待消费位置
    unsigned int                rx_count_frames;                    // RX中断聚合累计帧数

    unsigned int                rx_coal_current;                    // 当前RX中断聚合帧数
    unsigned int                rx_coal_packets;                    // 当前PPS统计周期累计RX描述符数
    unsigned int                rx_drain_target;                    // HIGH切换LOW时冻结的旧RX提交边界
    unsigned int                rx_drain_timer_ms;                  // RX DRAIN当前检查周期，单位毫秒
    unsigned long               rx_coal_next;                       // 下一次RX PPS统计时间
    unsigned long               rx_drain_next_backoff;              // 下一次RX DRAIN退避时间
    spinlock_t                  rx_coal_lock;                       // RX聚合状态锁，保护聚合模式和DRAIN状态

    struct timer_list           rx_timer;                           // RX HIGH模式兜底定时器
    struct hrtimer              rx_drain_timer;                     // RX LOW/DRAIN高精度排空定时器
    bool                        rx_timer_enabled;                   // RX HIGH模式定时器是否启用
    bool                        rx_drain_timer_enabled;             // RX DRAIN定时器是否启用
    bool                        rx_timer_ready;                     // RX自适应定时器是否允许启动
    dma_addr_t                  dma_rx_phy;                         // RX描述符环DMA物理地址

    struct net_device           *ndev;                              // Linux网络设备
    struct device               *dev;                               // 平台设备对象
    struct napi_struct          napi;                               // GMAC NAPI上下文

    struct geth_extra_stats     xstats;                             // GMAC扩展统计信息

    struct mii_bus              *mii;                               // MDIO总线
    int                         link;                               // 当前PHY链路状态
    int                         speed;                              // 当前PHY链路速率
    int                         duplex;                             // 当前PHY双工模式
    int                         phy_ext;                            // PHY类型，INT_PHY或EXT_PHY
    int                         phy_interface;                      // PHY接口模式

    void __iomem                *base;                              // GMAC寄存器映射基址
    void __iomem                *base_phy;                          // PHY控制寄存器映射基址
    struct clk                  *geth_clk;                          // GMAC主时钟
    struct clk                  *ephy_clk;                          // EPHY时钟
    struct reset_control        *reset;                             // GMAC复位控制器
    struct pinctrl              *pinctrl;                           // 外部PHY引脚控制器

    struct regulator            *gmac_power[POWER_CHAN_NUM];        // 外部PHY电源通道
    bool                        is_suspend;                         // 是否处于系统挂起恢复流程
    int                         phyrst;                             // 外部PHY复位GPIO
    u8                          rst_active_low;                     // PHY复位GPIO是否低有效
    spinlock_t                  lock;                               // GMAC状态锁
    spinlock_t                  tx_lock;                            // TX描述符环锁

    int                         use_ephy_clk;                       // 外部PHY是否使用EPHY 25MHz时钟
    int                         phy_addr;                           // 当前PHY MDIO地址

    unsigned int                tx_delay;                           // TX时钟延迟配置，范围0~7
    unsigned int                rx_delay;                           // RX时钟延迟配置，范围0~31

    struct work_struct          eth_work;                           // 系统恢复异步工作项
    struct mii_reg_dump         mii_reg;                            // PHY寄存器调试读写状态
};

/****************************** 全局状态 ******************************/

#ifdef CONFIG_RTL8363_NB
rtk_port_mac_ability_t mac_cfg;                                     // RTL8363外部端口MAC能力配置
rtk_stat_counter_t     cntr;                                        // RTL8363统计计数缓存
rtk_mode_ext_t         mode;                                        // RTL8363外部端口接口模式
struct net_device     *ndev = NULL;                                 // RTL8363兼容路径使用的全局网络设备
struct geth_priv      *priv;                                        // RTL8363兼容路径使用的全局GMAC上下文
#endif

static u64 geth_dma_mask = DMA_BIT_MASK(32);                        // GMAC 32位DMA地址掩码

/****************************** 基础辅助 ******************************/

/**
 * @brief 提供GMAC硬件层使用的微秒级延时回调。
 */
void sunxi_udelay(int n)
{
    udelay(n);
}

/****************************** 前置声明 ******************************/

static int  geth_stop(struct net_device *ndev);
static int  geth_open(struct net_device *ndev);
static void geth_tx_complete(struct geth_priv *priv);
static void geth_rx_refill(struct net_device *ndev);
static void geth_rx_coal_update(struct geth_priv *priv, unsigned int packets);

/****************************** Sysfs调试 ******************************/

#ifdef CONFIG_GETH_ATTRS
/**
 * @brief 读取内部PHY BGS调节值。
 */
static ssize_t adjust_bgs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int value = 0;
    u32 efuse_value;
    struct net_device *ndev = to_net_dev(dev);
    struct geth_priv *priv = netdev_priv(ndev);

    if (priv->phy_ext == INT_PHY)
    {
        value = readl(priv->base_phy) >> 28;
        if (sunxi_efuse_read(EFUSE_OEM_NAME, &efuse_value) != 0)
            pr_err("get PHY efuse fail!\n");
        else
#if IS_ENABLED(CONFIG_ARCH_SUN50IW2)
            value = value - ((efuse_value >> 24) & 0x0F);
#else
            pr_warn("miss config come from efuse!\n");
#endif
    }

    return sprintf(buf, "bgs: %d\n", value);
}

/**
 * @brief 设置内部PHY BGS调节值。
 */
static ssize_t adjust_bgs_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    unsigned int out = 0;
    struct net_device *ndev = to_net_dev(dev);
    struct geth_priv *priv = netdev_priv(ndev);
    u32 clk_value = readl(priv->base_phy);
    u32 efuse_value;

    out = simple_strtoul(buf, NULL, 10);

    if (priv->phy_ext == INT_PHY)
    {
        clk_value &= ~(0xF << 28);
        if (sunxi_efuse_read(EFUSE_OEM_NAME, &efuse_value) != 0)
            pr_err("get PHY efuse fail!\n");
        else
#if IS_ENABLED(CONFIG_ARCH_SUN50IW2)
            clk_value |= (((efuse_value >> 24) & 0x0F) + out) << 28;
#else
            pr_warn("miss config come from efuse!\n");
#endif
    }

    writel(clk_value, priv->base_phy);

    return count;
}

static struct device_attribute adjust_reg[] =
{
    __ATTR(adjust_bgs, 0664, adjust_bgs_show, adjust_bgs_write),
};

/**
 * @brief 创建GMAC可选Sysfs属性。
 */
static int geth_create_attrs(struct net_device *ndev)
{
    int j, ret;

    for (j = 0; j < ARRAY_SIZE(adjust_reg); j++)
    {
        ret = device_create_file(&ndev->dev, &adjust_reg[j]);
        if (ret)
            goto sysfs_failed;
    }
    goto succeed;

sysfs_failed:
    while (j--)
        device_remove_file(&ndev->dev, &adjust_reg[j]);
succeed:
    return ret;
}
#endif

#ifdef DEBUG
/**
 * @brief 打印DMA描述符内容用于调试。
 */
static void desc_print(struct dma_desc *desc, int size)
{
#ifdef DESC_PRINT
    int i;

    for (i = 0; i < size; i++)
    {
        u32 *x = (u32 *)(desc + i);

        pr_info("\t%d [0x%08lx]: %08x %08x %08x %08x\n", i, (unsigned long)(&desc[i]), x[0], x[1], x[2], x[3]);
    }
    pr_info("\n");
#endif
}
#endif

/**
 * @brief 输出GMAC扩展TX统计信息。
 */
static ssize_t extra_tx_stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct net_device *ndev = dev_get_drvdata(dev);
    struct geth_priv *priv = netdev_priv(ndev);

    if (!dev)
    {
        pr_err("Argment is invalid\n");
        return 0;
    }

    if (!ndev)
    {
        pr_err("Net device is null\n");
        return 0;
    }

    return sprintf(buf, "tx_underflow: %lu\ntx_carrier: %lu\n"
            "tx_losscarrier: %lu\nvlan_tag: %lu\n"
            "tx_deferred: %lu\ntx_vlan: %lu\n"
            "tx_jabber: %lu\ntx_frame_flushed: %lu\n"
            "tx_payload_error: %lu\ntx_ip_header_error: %lu\n\n",
            priv->xstats.tx_underflow, priv->xstats.tx_carrier,
            priv->xstats.tx_losscarrier, priv->xstats.vlan_tag,
            priv->xstats.tx_deferred, priv->xstats.tx_vlan,
            priv->xstats.tx_jabber, priv->xstats.tx_frame_flushed,
            priv->xstats.tx_payload_error, priv->xstats.tx_ip_header_error);
}
static DEVICE_ATTR(extra_tx_stats, 0444, extra_tx_stats_show, NULL);

/**
 * @brief 输出GMAC扩展RX统计信息。
 */
static ssize_t extra_rx_stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct net_device *ndev = dev_get_drvdata(dev);
    struct geth_priv *priv = netdev_priv(ndev);

    if (!dev)
    {
        pr_err("Argment is invalid\n");
        return 0;
    }

    if (!ndev)
    {
        pr_err("Net device is null\n");
        return 0;
    }

    return sprintf(buf, "rx_desc: %lu\nsa_filter_fail: %lu\n"
            "overflow_error: %lu\nipc_csum_error: %lu\n"
            "rx_collision: %lu\nrx_crc: %lu\n"
            "dribbling_bit: %lu\nrx_length: %lu\n"
            "rx_mii: %lu\nrx_multicast: %lu\n"
            "rx_gmac_overflow: %lu\nrx_watchdog: %lu\n"
            "da_rx_filter_fail: %lu\nsa_rx_filter_fail: %lu\n"
            "rx_missed_cntr: %lu\nrx_overflow_cntr: %lu\n"
            "rx_vlan: %lu\n\n",
            priv->xstats.rx_desc, priv->xstats.sa_filter_fail,
            priv->xstats.overflow_error, priv->xstats.ipc_csum_error,
            priv->xstats.rx_collision, priv->xstats.rx_crc,
            priv->xstats.dribbling_bit, priv->xstats.rx_length,
            priv->xstats.rx_mii, priv->xstats.rx_multicast,
            priv->xstats.rx_gmac_overflow, priv->xstats.rx_length,
            priv->xstats.da_rx_filter_fail, priv->xstats.sa_rx_filter_fail,
            priv->xstats.rx_missed_cntr, priv->xstats.rx_overflow_cntr,
            priv->xstats.rx_vlan);
}
static DEVICE_ATTR(extra_rx_stats, 0444, extra_rx_stats_show, NULL);

/**
 * @brief 输出PHY测试模式Sysfs使用说明。
 */
static ssize_t gphy_test_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct net_device *ndev = dev_get_drvdata(dev);

    if (!dev)
    {
        pr_err("Argment is invalid\n");
        return 0;
    }

    if (!ndev)
    {
        pr_err("Net device is null\n");
        return 0;
    }

    return sprintf(buf, "Usage:\necho [0/1/2/3/4] > gphy_test\n"
            "0 - Normal Mode\n"
            "1 - Transmit Jitter Test\n"
            "2 - Transmit Jitter Test(MASTER mode)\n"
            "3 - Transmit Jitter Test(SLAVE mode)\n"
            "4 - Transmit Distortion Test\n\n");
}

/**
 * @brief 配置PHY发送测试模式。
 */
static ssize_t gphy_test_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct net_device *ndev = dev_get_drvdata(dev);
    struct geth_priv *priv = netdev_priv(ndev);
    u16 value = 0;
    int ret = 0;
    u16 data = 0;

    if (!dev)
    {
        pr_err("Argument is invalid\n");
        return count;
    }

    if (!ndev)
    {
        pr_err("Net device is null\n");
        return count;
    }

    data = sunxi_mdio_read(priv->base, priv->phy_addr, MII_CTRL1000);

    ret = kstrtou16(buf, 0, &value);
    if (ret)
        return ret;

    if (value >= 0 && value <= 4)
    {
        data &= ~(0x7 << 13);
        data |= value << 13;
        sunxi_mdio_write(priv->base, priv->phy_addr, MII_CTRL1000, data);
        pr_info("Set MII_CTRL1000(0x09) Reg: 0x%x\n", data);
    }
    else
    {
        pr_info("unknown value (%d)\n", value);
    }

    return count;
}

static DEVICE_ATTR(gphy_test, 0664, gphy_test_show, gphy_test_store);

/**
 * @brief 读取并输出指定PHY寄存器值。
 */
static ssize_t mii_read_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct net_device *ndev = NULL;
    struct geth_priv *priv = NULL;

    if (dev == NULL)
    {
        pr_err("Argment is invalid\n");
        return 0;
    }

    ndev = dev_get_drvdata(dev);
    if (ndev == NULL)
    {
        pr_err("Net device is null\n");
        return 0;
    }

    priv = netdev_priv(ndev);
    if (priv == NULL)
    {
        pr_err("geth_priv is null\n");
        return 0;
    }

    if (!netif_running(ndev))
    {
        pr_warn("eth is down!\n");
        return 0;
    }

    priv->mii_reg.value = sunxi_mdio_read(priv->base, priv->mii_reg.addr, priv->mii_reg.reg);
    return sprintf(buf, "ADDR[0x%02x]:REG[0x%02x] = 0x%04x\n", priv->mii_reg.addr, priv->mii_reg.reg, priv->mii_reg.value);
}

/**
 * @brief 解析并保存待读取的PHY寄存器地址。
 */
static ssize_t mii_read_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct net_device *ndev = NULL;
    struct geth_priv *priv = NULL;
    int ret = 0;
    u16 reg, addr;
    char *ptr;

    ptr = (char *)buf;

    if (dev == NULL)
    {
        pr_err("Argment is invalid\n");
        return count;
    }

    ndev = dev_get_drvdata(dev);
    if (ndev == NULL)
    {
        pr_err("Net device is null\n");
        return count;
    }

    priv = netdev_priv(ndev);
    if (priv == NULL)
    {
        pr_err("geth_priv is null\n");
        return count;
    }

    if (!netif_running(ndev))
    {
        pr_warn("eth is down!\n");
        return count;
    }

    ret = sunxi_parse_read_str(ptr, &addr, &reg);
    if (ret)
        return ret;

    priv->mii_reg.addr = addr;
    priv->mii_reg.reg = reg;

    return count;
}

static DEVICE_ATTR(mii_read, 0664, mii_read_show, mii_read_store);

/**
 * @brief 执行PHY寄存器写入并输出写入前后值。
 */
static ssize_t mii_write_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct net_device *ndev = NULL;
    struct geth_priv *priv = NULL;
    u16 bef_val, aft_val;

    if (dev == NULL)
    {
        pr_err("Argment is invalid\n");
        return 0;
    }

    ndev = dev_get_drvdata(dev);
    if (ndev == NULL)
    {
        pr_err("Net device is null\n");
        return 0;
    }

    priv = netdev_priv(ndev);
    if (priv == NULL)
    {
        pr_err("geth_priv is null\n");
        return 0;
    }

    if (!netif_running(ndev))
    {
        pr_warn("eth is down!\n");
        return 0;
    }

    bef_val = sunxi_mdio_read(priv->base, priv->mii_reg.addr, priv->mii_reg.reg);
    sunxi_mdio_write(priv->base, priv->mii_reg.addr, priv->mii_reg.reg, priv->mii_reg.value);
    aft_val = sunxi_mdio_read(priv->base, priv->mii_reg.addr, priv->mii_reg.reg);
    return sprintf(buf, "before ADDR[0x%02x]:REG[0x%02x] = 0x%04x\n"
                "after  ADDR[0x%02x]:REG[0x%02x] = 0x%04x\n",
                priv->mii_reg.addr, priv->mii_reg.reg, bef_val,
                priv->mii_reg.addr, priv->mii_reg.reg, aft_val);
}

/**
 * @brief 解析并保存待写入的PHY寄存器和值。
 */
static ssize_t mii_write_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct net_device *ndev = NULL;
    struct geth_priv *priv = NULL;
    int ret = 0;
    u16 reg, addr, val;
    char *ptr;

    ptr = (char *)buf;

    if (dev == NULL)
    {
        pr_err("Argment is invalid\n");
        return count;
    }

    ndev = dev_get_drvdata(dev);
    if (ndev == NULL)
    {
        pr_err("Net device is null\n");
        return count;
    }

    priv = netdev_priv(ndev);
    if (priv == NULL)
    {
        pr_err("geth_priv is null\n");
        return count;
    }

    if (!netif_running(ndev))
    {
        pr_warn("eth is down!\n");
        return count;
    }

    ret = sunxi_parse_write_str(ptr, &addr, &reg, &val);
    if (ret)
        return ret;

    priv->mii_reg.reg = reg;
    priv->mii_reg.addr = addr;
    priv->mii_reg.value = val;

    return count;
}

static DEVICE_ATTR(mii_write, 0664, mii_write_show, mii_write_store);

/**
 * @brief 输出MAC/PHY回环测试Sysfs使用说明。
 */
static ssize_t loopback_test_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "Usage:\necho [0/1/2] > loopback_test\n" "0 - Normal Mode\n" "1 - Mac loopback test mode\n" "2 - Phy loopback test mode\n");
}

/**
 * @brief 配置MAC或PHY回环测试模式。
 */
static ssize_t loopback_test_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct net_device *ndev = NULL;
    struct geth_priv *priv = NULL;
    u16 value = 0;
    int ret = 0;
    u16 data = 0;

    if (dev == NULL)
    {
        pr_err("Argment is invalid\n");
        return count;
    }

    ndev = dev_get_drvdata(dev);
    if (ndev == NULL)
    {
        pr_err("Net device is null\n");
        return count;
    }

    priv = netdev_priv(ndev);
    if (priv == NULL)
    {
        pr_err("geth_priv is null\n");
        return count;
    }

    if (!netif_running(ndev))
    {
        pr_warn("eth is down!\n");
        return count;
    }

    ret = kstrtou16(buf, 0, &value);
    if (ret)
        return ret;

    // Normal模式。
    if (value == 0)
    {
        // 清除MAC回环。
        sunxi_mac_loopback(priv->base, 0);

        // 清除PHY回环。
        data = sunxi_mdio_read(priv->base, priv->phy_addr, MII_BMCR);
        sunxi_mdio_write(priv->base, priv->phy_addr, MII_BMCR, data & ~BMCR_LOOPBACK);
    }
    else if (value == 1)
    {
        // MAC回环测试模式。
        data = sunxi_mdio_read(priv->base, priv->phy_addr, MII_BMCR);
        sunxi_mdio_write(priv->base, priv->phy_addr, MII_BMCR, data & ~BMCR_LOOPBACK);

        sunxi_mac_loopback(priv->base, 1);
    }
    else if (value == 2)
    {
        // PHY回环测试模式。
        sunxi_mac_loopback(priv->base, 0);

        data = sunxi_mdio_read(priv->base, priv->phy_addr, MII_BMCR);
        sunxi_mdio_write(priv->base, priv->phy_addr, MII_BMCR, data | BMCR_LOOPBACK);
    }
    else
    {
        pr_err("Undefined value (%d)\n", value);
    }

    return count;
}
static DEVICE_ATTR(loopback_test, 0664, loopback_test_show, loopback_test_store);

/**
 * @brief 开启GMAC及PHY相关电源。
 */
static int geth_power_on(struct geth_priv *priv)
{
    int value;
    int i;

    value = readl(priv->base_phy);
    if (priv->phy_ext == INT_PHY)
    {
        value |= (1 << 15);
        value &= ~(1 << 16);
        value |= (3 << 17);
    }
    else
    {
        value &= ~(1 << 15);

        for (i = 0; i < POWER_CHAN_NUM; i++)
        {
            if (IS_ERR_OR_NULL(priv->gmac_power[i]))
                continue;
            if (regulator_enable(priv->gmac_power[i]) != 0)
            {
                pr_err("gmac-power%d enable error\n", i);
                return -EINVAL;
            }
        }
    }

    writel(value, priv->base_phy);

    return 0;
}

/**
 * @brief 关闭GMAC及PHY相关电源。
 */
static void geth_power_off(struct geth_priv *priv)
{
    int value;
    int i;

    if (priv->phy_ext == INT_PHY)
    {
        value = readl(priv->base_phy);
        value |= (1 << 16);
        writel(value, priv->base_phy);
    }
    else
    {
        for (i = 0; i < POWER_CHAN_NUM; i++)
        {
            if (IS_ERR_OR_NULL(priv->gmac_power[i]))
                continue;
            regulator_disable(priv->gmac_power[i]);
        }
    }
}

/****************************** RTL8363支持 ******************************/

#ifdef CONFIG_RTL8363_NB
/**
 * @brief 初始化RTL8363NB交换芯片及外部RGMII端口。
 */
static int rtl8363nb_vb_init(void)
{
    pr_info("%s->%d rtk8363 init=====\n", __func__, __LINE__);

    if (rtk_switch_init() != RT_ERR_OK)
    {
        pr_info("rtk switch init failed!\n");
        return -1;
    }
    mode = MODE_EXT_RGMII;
    mac_cfg.forcemode = MAC_FORCE;
    mac_cfg.speed = SPD_1000M;
    mac_cfg.duplex = FULL_DUPLEX;
    mac_cfg.link = PORT_LINKUP;
    mac_cfg.nway = DISABLED;
    mac_cfg.txpause = ENABLED;
    mac_cfg.rxpause = ENABLED;

    if (rtk_port_macForceLinkExt_set(EXT_PORT0, mode, &mac_cfg) != RT_ERR_OK)
    {
        pr_info("macForceLinkExt set failed!\n");
        return -1;
    }

    rtk_port_rgmiiDelayExt_set(EXT_PORT0, 1, 0);
    rtk_port_phyEnableAll_set(ENABLED);
}

/**
 * @brief 通过GMAC MDIO接口读取RTL8363寄存器。
 */
int rtk_mdio_read(u32 len, u8 phy_adr, u8 reg, u32 *value)
{

    struct geth_priv *priv = netdev_priv(ndev);
    *value = sunxi_mdio_read(priv->base, 0, reg);

    return 0;
}

/**
 * @brief 通过GMAC MDIO接口写入RTL8363寄存器。
 */
int rtk_mdio_write(u32 len, u8 phy_adr, u8 reg, u32 data)
{

    struct geth_priv *priv = netdev_priv(ndev);
    sunxi_mdio_write(priv->base, 0, reg, data);

    return 0;
}
/**
 * @brief 实现RTL8363 MDIO总线PHY读取回调。
 */
static int rtk_phy_read(struct mii_bus *bus, int phyaddr, int phyreg)
{
    struct net_device *ndev = bus->priv;
    struct geth_priv *priv = netdev_priv(ndev);

    return (int)smi_read(phyreg, NULL);
}

/**
 * @brief 实现RTL8363 MDIO总线PHY写入回调。
 */
static int rtk_phy_write(struct mii_bus *bus, int phyaddr, int phyreg, u16 data)
{
    struct net_device *ndev = bus->priv;
    struct geth_priv *priv = netdev_priv(ndev);

    smi_write(phyreg, data);

    return 0;
}
#endif

/****************************** PHY与MDIO ******************************/

/**
 * @brief 通过Sunxi GMAC读取PHY寄存器。
 */
static int geth_mdio_read(struct mii_bus *bus, int phyaddr, int phyreg)
{
    struct net_device *ndev = bus->priv;
    struct geth_priv *priv = netdev_priv(ndev);

    return (int)sunxi_mdio_read(priv->base,  phyaddr, phyreg);
}

/**
 * @brief 通过Sunxi GMAC写入PHY寄存器。
 */
static int geth_mdio_write(struct mii_bus *bus, int phyaddr, int phyreg, u16 data)
{
    struct net_device *ndev = bus->priv;
    struct geth_priv *priv = netdev_priv(ndev);

    sunxi_mdio_write(priv->base, phyaddr, phyreg, data);

    return 0;
}

/**
 * @brief 复位GMAC MDIO控制器。
 */
static int geth_mdio_reset(struct mii_bus *bus)
{
    struct net_device *ndev = bus->priv;
    struct geth_priv *priv = netdev_priv(ndev);

    return sunxi_mdio_reset(priv->base);
}

/**
 * @brief 根据PHY状态更新GMAC链路速率、双工和流控配置。
 */
static void geth_adjust_link(struct net_device *ndev)
{
    struct geth_priv *priv = netdev_priv(ndev);
    struct phy_device *phydev = ndev->phydev;
    unsigned long flags;
    int new_state = 0;
    if (!phydev)
        return;

    spin_lock_irqsave(&priv->lock, flags);
#ifdef CONFIG_RTL8363_NB
    priv->speed = 1000;
    priv->duplex = 1;
    sunxi_set_link_mode(priv->base, 1, 1000);
    phy_print_status(phydev);
#else
    if (phydev->link)
    {
        /** Now we make sure that we can be in full duplex mode.
         * If not, we operate in half-duplex mode.
         */
        if (phydev->duplex != priv->duplex)
        {
            new_state = 1;
            priv->duplex = phydev->duplex;
        }
        // 配置PHY协商得到的Pause流控。
        if (phydev->pause)
            sunxi_flow_ctrl(priv->base, phydev->duplex, flow_ctrl, pause);

        if (phydev->speed != priv->speed)
        {
            new_state = 1;
            priv->speed = phydev->speed;
        }

        if (priv->link == 0)
        {
            new_state = 1;
            priv->link = phydev->link;
        }

        if (new_state)
            sunxi_set_link_mode(priv->base, priv->duplex, priv->speed);

#ifdef LOOPBACK_DEBUG
        phydev->state = PHY_FORCING;
#endif

    }
    else if (priv->link != phydev->link)
    {
        new_state = 1;
        priv->link = 0;
        priv->speed = 0;
        priv->duplex = -1;
    }

    if (new_state)
        phy_print_status(phydev);
#endif

    spin_unlock_irqrestore(&priv->lock, flags);
}

/**
 * @brief 初始化MDIO总线并连接PHY设备。
 */
static int geth_phy_init(struct net_device *ndev)
{
    int value;
    struct mii_bus *new_bus;
    struct geth_priv *priv = netdev_priv(ndev);
    struct phy_device *phydev = ndev->phydev;

    // 修正内部PHY使用的接口类型。
    if (priv->phy_ext == INT_PHY)
    {
        priv->phy_interface = PHY_INTERFACE_MODE_MII;
    }
    else
    {
        // 配置了PHY复位GPIO时执行硬件复位。
        if (gpio_is_valid(priv->phyrst))
        {
            gpio_direction_output(priv->phyrst, priv->rst_active_low);
            msleep(10);
            gpio_direction_output(priv->phyrst, !priv->rst_active_low);
            msleep(10);
        }
    }

    if (priv->is_suspend && phydev)
        goto resume;

    new_bus = mdiobus_alloc();
    if (!new_bus)
    {
        netdev_err(ndev, "Failed to alloc new mdio bus\n");
        return -ENOMEM;
    }

    new_bus->name = dev_name(priv->dev);
#ifdef CONFIG_RTL8363_NB
    // RTL8363不使用内核标准PHY访问接口，改用自定义读写回调。
    new_bus->read = &rtk_phy_read;
    new_bus->write = &rtk_phy_write;
#if 0
    // 读取寄存器0x1b00。
    sunxi_mdio_write(priv->base, priv->phy_addr, 31, 0x000E);
    sunxi_mdio_write(priv->base, priv->phy_addr, 23, 0x1b00);
    sunxi_mdio_write(priv->base, priv->phy_addr, 21, 0x1);
    sunxi_mdio_read(priv->base, priv->phy_addr, 25);
    pr_info("%s->%d =====> reg 0x1b00 = %x!\n", __func__, __LINE__, \ sunxi_mdio_read(priv->base, priv->phy_addr, 25));
#endif
#else
    new_bus->read = &geth_mdio_read;
    new_bus->write = &geth_mdio_write;
    new_bus->reset = &geth_mdio_reset;
#endif
    snprintf(new_bus->id, MII_BUS_ID_SIZE, "%s-%x", new_bus->name, 0);

    new_bus->parent = priv->dev;
    new_bus->priv = ndev;

    if (mdiobus_register(new_bus))
    {
        pr_err("%s: Cannot register as MDIO bus\n", new_bus->name);
        goto reg_fail;
    }

    priv->mii = new_bus;

    {
        int addr;
        for (addr = 0; addr < PHY_MAX_ADDR; addr++)
        {
            struct phy_device *phydev_tmp = mdiobus_get_phy(new_bus, addr);

#if defined(CONFIG_ARCH_SUN50IW9)
            if (IS_ERR_OR_NULL(phydev_tmp) || phydev_tmp->phy_id == 0xffff)
            {
                if (!IS_ERR_OR_NULL(phydev_tmp))
                    phy_device_remove(phydev_tmp);
                phydev_tmp = mdiobus_scan(new_bus, addr);
            }

            if (!phydev_tmp)
                continue;

            if (phydev_tmp->phy_id == EPHY_ID)
            {
                phydev = phydev_tmp;
                priv->phy_addr = addr;
                break;
            }
#else
            if (phydev_tmp && (phydev_tmp->phy_id != 0x00))
            {
                phydev = phydev_tmp;
                priv->phy_addr = addr;
                break;
            }
#endif
        }
    }

    if (!phydev)
    {
        netdev_err(ndev, "No PHY found!\n");
        goto err;
    }

    phydev->irq = PHY_POLL;

    value = phy_connect_direct(ndev, phydev, &geth_adjust_link, priv->phy_interface);
    if (value)
    {
        netdev_err(ndev, "Could not attach to PHY\n");
        goto err;
    }
    else
    {
        netdev_info(ndev, "%s: Type(%d) PHY ID %08x at %d IRQ %s (%s)\n", ndev->name, phydev->interface, phydev->phy_id, phydev->mdio.addr, "poll", dev_name(&phydev->mdio.dev));
    }

    // phydev->supported &= PHY_GBIT_FEATURES;
    phydev->is_gigabit_capable = 1;
    // phydev->advertising = phydev->supported;

resume:
    phy_write(phydev, MII_BMCR, BMCR_RESET);
    while (BMCR_RESET & phy_read(phydev, MII_BMCR))
        msleep(30);

    value = phy_read(phydev, MII_BMCR);
    phy_write(phydev, MII_BMCR, (value & ~BMCR_PDOWN));

    if (priv->phy_ext == INT_PHY)
    {
        // 初始化内部EPHY模拟前端参数。
        phy_write(phydev, 0x1f, 0x0100); // 切换到Page 1
        phy_write(phydev, 0x12, 0x4824); // 关闭APS
        phy_write(phydev, 0x1f, 0x0200); // 切换到Page 2
        phy_write(phydev, 0x18, 0x0000); // PHY AFE收发优化
        phy_write(phydev, 0x1f, 0x0600); // 切换到Page 6
        phy_write(phydev, 0x14, 0x708F); // PHY AFE发送优化
        phy_write(phydev, 0x19, 0x0000);
        phy_write(phydev, 0x13, 0xf000); // PHY AFE接收优化
        phy_write(phydev, 0x15, 0x1530);
        phy_write(phydev, 0x1f, 0x0800); // 切换到Page 8
        phy_write(phydev, 0x18, 0x00bc); // PHY AFE收发优化
        phy_write(phydev, 0x1f, 0x0100); // 切换到Page 1
        // 将寄存器0x17 bit3清零以关闭iEEE。
        phy_write(phydev, 0x17, phy_read(phydev, 0x17) & (~(1<<3)));
        phy_write(phydev, 0x1f, 0x0000); // 切换到Page 0
    }
    if (priv->is_suspend)
        phy_init_hw(phydev);

    return 0;

err:
    mdiobus_unregister(new_bus);
reg_fail:
    mdiobus_free(new_bus);

    return -EINVAL;
}

/**
 * @brief 停止并释放当前PHY及MDIO总线。
 */
static int geth_phy_release(struct net_device *ndev)
{
    struct geth_priv *priv = netdev_priv(ndev);
    struct phy_device *phydev = ndev->phydev;
    int value = 0;

    // 停止并断开PHY。
    if (phydev && phy_is_started(phydev))
        phy_stop(phydev);

    priv->link = PHY_DOWN;
    priv->speed = 0;
    priv->duplex = -1;

    if (phydev)
    {
        value = phy_read(phydev, MII_BMCR);
        phy_write(phydev, MII_BMCR, (value | BMCR_PDOWN));
    }

    if (priv->is_suspend)
        return 0;

    if (phydev)
    {
        phy_disconnect(phydev);
        ndev->phydev = NULL;
    }

    if (priv->mii)
    {
        mdiobus_unregister(priv->mii);
        priv->mii->priv = NULL;
        mdiobus_free(priv->mii);
        priv->mii = NULL;
    }

    return 0;
}

/****************************** DMA描述符 ******************************/

/**
 * @brief 补充RX DMA描述符并重新提交给GMAC接收数据。
 *
 * 为可用RX描述符准备SKB接收缓冲区并建立DMA映射，
 * 根据当前RX中断聚合策略配置完成中断，并将描述符
 * 所有权重新交给DMA。
 *
 * RX聚合状态锁只覆盖描述符最终提交阶段，保证聚合模式
 * 切换与dis_ic配置、RX描述符提交边界保持一致。
 */
static void geth_rx_refill(struct net_device *ndev)
{
    struct geth_priv *priv = netdev_priv(ndev);
    struct dma_desc *desc;
    struct sk_buff *sk = NULL;
    dma_addr_t paddr;
    unsigned long flags;
    unsigned int coal_frames;
    bool set_ic;

    while (circ_space(priv->rx_clean, priv->rx_dirty, dma_desc_rx) > 0)
    {
        int entry = priv->rx_clean;

        desc = priv->dma_rx + entry;

        if (priv->rx_sk[entry] == NULL)
        {
            sk = netdev_alloc_skb_ip_align(ndev, priv->buf_sz);

            if (unlikely(sk == NULL))
            {
                break;
            }

            priv->rx_sk[entry] = sk;

            paddr = dma_map_single(priv->dev, sk->data, priv->buf_sz, DMA_FROM_DEVICE);
            if (dma_mapping_error(priv->dev, paddr))
            {
                priv->rx_sk[entry] = NULL;
                priv->ndev->stats.rx_dropped++;
                dev_kfree_skb_any(sk);
                break;
            }

            desc_buf_set(desc, paddr, priv->buf_sz);
        }

        /**
         * 聚合模式可能由RX timer在另一CPU上切换。
         * 在最终配置dis_ic并提交OWN时持锁，保证从RX=16切换
         * 到RX=1以后不会再提交新的旧模式抑制中断描述符。
         */
        spin_lock_irqsave(&priv->rx_coal_lock, flags);

        coal_frames = priv->rx_coal_current;
        set_ic = ++priv->rx_count_frames >= coal_frames;

        if (set_ic)
        {
            priv->rx_count_frames = 0;
        }

        // RX的dis_ic为高有效，0表示允许产生RX完成中断。
        desc->desc1.rx.dis_ic = !set_ic;

        // 确保描述符配置完成后再将所有权交给DMA。
        wmb();
        desc_set_own(desc);

        /**
         * OWN提交和rx_clean推进必须处于同一聚合状态锁内。
         * HIGH->LOW切换时读取rx_clean即可得到完整的旧RX提交边界，
         * 不会遗漏已经按HIGH模式提交但尚未推进rx_clean的描述符。
         */
        priv->rx_clean = circ_inc(priv->rx_clean, dma_desc_rx);

        spin_unlock_irqrestore(&priv->rx_coal_lock, flags);
    }
}

/**
 * @brief 分配RX/TX描述符环及对应SKB索引数组。
 */
static int geth_dma_desc_init(struct net_device *ndev)
{
    struct geth_priv *priv = netdev_priv(ndev);
    unsigned int buf_sz;

    priv->rx_sk = kzalloc(sizeof(struct sk_buff *) * dma_desc_rx, GFP_KERNEL);
    if (!priv->rx_sk)
        return -ENOMEM;

    priv->tx_sk = kzalloc(sizeof(struct sk_buff *) * dma_desc_tx, GFP_KERNEL);
    if (!priv->tx_sk)
        goto tx_sk_err;

    // 根据MTU和最大缓冲区限制设置DMA缓冲区大小。
    buf_sz = MAX_BUF_SZ;

    priv->dma_tx = dma_alloc_coherent(priv->dev, dma_desc_tx * sizeof(struct dma_desc), &priv->dma_tx_phy, GFP_KERNEL);
    if (!priv->dma_tx)
        goto dma_tx_err;

    priv->dma_rx = dma_alloc_coherent(priv->dev, dma_desc_rx * sizeof(struct dma_desc), &priv->dma_rx_phy, GFP_KERNEL);
    if (!priv->dma_rx)
        goto dma_rx_err;

    priv->buf_sz = buf_sz;

    return 0;

dma_rx_err:
    dma_free_coherent(priv->dev, dma_desc_rx * sizeof(struct dma_desc), priv->dma_tx, priv->dma_tx_phy);
dma_tx_err:
    kfree(priv->tx_sk);
tx_sk_err:
    kfree(priv->rx_sk);

    return -ENOMEM;
}

/**
 * @brief 释放RX描述符环中仍持有的SKB和DMA映射。
 */
static void geth_free_rx_sk(struct geth_priv *priv)
{
    int i;

    for (i = 0; i < dma_desc_rx; i++)
    {
        if (priv->rx_sk[i] != NULL)
        {
            struct dma_desc *desc = priv->dma_rx + i;

            dma_unmap_single(priv->dev, (u32)desc_buf_get_addr(desc), desc_buf_get_len(desc), DMA_FROM_DEVICE);
            dev_kfree_skb_any(priv->rx_sk[i]);
            priv->rx_sk[i] = NULL;
        }
    }
}

/**
 * @brief 释放TX描述符环中仍持有的SKB和DMA映射。
 */
static void geth_free_tx_sk(struct geth_priv *priv)
{
    int i;

    for (i = 0; i < dma_desc_tx; i++)
    {
        if (priv->tx_sk[i] != NULL)
        {
            struct dma_desc *desc = priv->dma_tx + i;

            if (desc_buf_get_addr(desc))
                dma_unmap_single(priv->dev, (u32)desc_buf_get_addr(desc), desc_buf_get_len(desc), DMA_TO_DEVICE);
            dev_kfree_skb_any(priv->tx_sk[i]);
            priv->tx_sk[i] = NULL;
        }
    }
}

/**
 * @brief 释放RX/TX DMA描述符环及SKB索引数组。
 */
static void geth_free_dma_desc(struct geth_priv *priv)
{
    // 释放此前为DMA描述符环分配的一致性内存。
    dma_free_coherent(priv->dev, dma_desc_tx * sizeof(struct dma_desc), priv->dma_tx, priv->dma_tx_phy);
    dma_free_coherent(priv->dev, dma_desc_rx * sizeof(struct dma_desc), priv->dma_rx, priv->dma_rx_phy);

    kfree(priv->rx_sk);
    kfree(priv->tx_sk);
}

/****************************** 电源管理 ******************************/

#if IS_ENABLED(CONFIG_PM)
/**
 * @brief 切换GMAC pinctrl状态。
 */
static int geth_select_gpio_state(struct pinctrl *pctrl, char *name)
{
    int ret = 0;
    struct pinctrl_state *pctrl_state = NULL;

    pctrl_state = pinctrl_lookup_state(pctrl, name);
    if (IS_ERR(pctrl_state))
    {
        pr_err("gmac pinctrl_lookup_state(%s) failed! return %p\n", name, pctrl_state);
        return -EINVAL;
    }

    ret = pinctrl_select_state(pctrl, pctrl_state);
    if (ret < 0)
        pr_err("gmac pinctrl_select_state(%s) failed! return %d\n", name, ret);

    return ret;
}

/**
 * @brief 执行GMAC系统挂起准备流程。
 */
static int geth_suspend(struct device *dev)
{
    struct net_device *ndev = dev_get_drvdata(dev);
    struct geth_priv *priv = netdev_priv(ndev);

    cancel_work_sync(&priv->eth_work);

    if (!ndev || !netif_running(ndev))
        return 0;

    priv->is_suspend = true;

    spin_lock(&priv->lock);
    netif_device_detach(ndev);
    spin_unlock(&priv->lock);

    geth_stop(ndev);

    if (priv->phy_ext == EXT_PHY)
        geth_select_gpio_state(priv->pinctrl, PINCTRL_STATE_SLEEP);

    return 0;
}

/**
 * @brief 在异步工作项中恢复GMAC网络设备。
 */
static void geth_resume_work(struct work_struct *work)
{
    struct geth_priv *priv = container_of(work, struct geth_priv, eth_work);
    struct net_device *ndev = priv->ndev;
    int ret = 0;

    if (!netif_running(ndev))
        return;

    if (priv->phy_ext == EXT_PHY)
        geth_select_gpio_state(priv->pinctrl, PINCTRL_STATE_DEFAULT);

    spin_lock(&priv->lock);
    netif_device_attach(ndev);
    spin_unlock(&priv->lock);

#if IS_ENABLED(CONFIG_SUNXI_EPHY)
    if (!ephy_is_enable())
    {
        pr_info("[geth_resume] ephy is not enable, waiting...\n");
        msleep(2000);
        if (!ephy_is_enable())
        {
            netdev_err(ndev, "Wait for ephy resume timeout.\n");
            return;
        }
    }
#endif

    ret = geth_open(ndev);
    if (!ret)
        priv->is_suspend = false;
}

/**
 * @brief 调度GMAC系统恢复工作项。
 */
static void geth_resume(struct device *dev)
{
    struct net_device *ndev = dev_get_drvdata(dev);
    struct geth_priv *priv = netdev_priv(ndev);

    schedule_work(&priv->eth_work);
}

/**
 * @brief 处理GMAC冻结电源管理回调。
 */
static int geth_freeze(struct device *dev)
{
    return 0;
}

/**
 * @brief 处理GMAC恢复电源管理回调。
 */
static int geth_restore(struct device *dev)
{
    return 0;
}

static const struct dev_pm_ops geth_pm_ops =
{
    .complete = geth_resume,
    .prepare  = geth_suspend,
    .suspend  = NULL,
    .resume   = NULL,
    .freeze   = geth_freeze,
    .restore  = geth_restore,
};
#else
static const struct dev_pm_ops geth_pm_ops;
#endif // CONFIG_PM

/****************************** MAC与时钟 ******************************/

#define sunxi_get_soc_chipid(x)         {}                                        // SoC Chip ID读取兼容占位宏

/**
 * @brief 基于SoC Chip ID生成本地管理MAC地址。
 */
static void geth_chip_hwaddr(u8 *addr)
{
#define MD5_SIZE                        16                                        // MD5摘要长度
#define CHIP_SIZE                       16                                        // SoC Chip ID缓冲区长度

    struct crypto_ahash *tfm;
    struct ahash_request *req;
    struct scatterlist sg;
    u8 result[MD5_SIZE];
    u8 chipid[CHIP_SIZE];
    int i = 0;
    int ret = -1;

    memset(chipid, 0, sizeof(chipid));
    memset(result, 0, sizeof(result));

    sunxi_get_soc_chipid((u8 *)chipid);

    tfm = crypto_alloc_ahash("md5", 0, CRYPTO_ALG_ASYNC);
    if (IS_ERR(tfm))
    {
        pr_err("Failed to alloc md5\n");
        return;
    }

    req = ahash_request_alloc(tfm, GFP_KERNEL);
    if (!req)
        goto out;

    ahash_request_set_callback(req, 0, NULL, NULL);

    ret = crypto_ahash_init(req);
    if (ret)
    {
        pr_err("crypto_ahash_init() failed\n");
        goto out;
    }

    sg_init_one(&sg, chipid, sizeof(chipid));
    ahash_request_set_crypt(req, &sg, result, sizeof(chipid));
    ret = crypto_ahash_update(req);
    if (ret)
    {
        pr_err("crypto_ahash_update() failed for id\n");
        goto out;
    }

    ret = crypto_ahash_final(req);
    if (ret)
    {
        pr_err("crypto_ahash_final() failed for result\n");
        goto out;
    }

    ahash_request_free(req);

    // 取MD5结果的[0][2][4][6][8][10]字节构造MAC地址。
    for (i = 0; i < ETH_ALEN; i++)
        addr[i] = result[2 * i];
    addr[0] &= 0xfe; // 清除组播地址位。
    addr[0] |= 0x02; // 设置IEEE 802本地管理地址位。

out:
    crypto_free_ahash(tfm);
}

/**
 * @brief 校验并补充网络设备MAC地址。
 */
static void geth_check_addr(struct net_device *ndev, unsigned char *mac)
{
    int i;
    char *p = mac;

    if (!is_valid_ether_addr(ndev->dev_addr))
    {
        for (i = 0; i < ETH_ALEN; i++, p++)
            ndev->dev_addr[i] = simple_strtoul(p, &p, 16);

        if (!is_valid_ether_addr(ndev->dev_addr))
            geth_chip_hwaddr(ndev->dev_addr);

        if (!is_valid_ether_addr(ndev->dev_addr))
        {
            random_ether_addr(ndev->dev_addr);
            pr_warn("%s: Use random mac address\n", ndev->name);
        }
    }
}

/**
 * @brief 解除GMAC复位并开启相关时钟。
 */
static int geth_clk_enable(struct geth_priv *priv)
{
    int ret;
    int phy_interface = 0;
    u32 clk_value;
    // u32 efuse_value;

    ret = reset_control_deassert(priv->reset);
    if (ret)
    {
        pr_err("deassert gmac rst failed!\n");
        return ret;
    }

    ret = clk_prepare_enable(priv->geth_clk);
    if (ret)
    {
        pr_err("try to enable geth_clk failed!\n");
        goto assert_reset;
    }

    if (((priv->phy_ext == INT_PHY) || priv->use_ephy_clk) && !IS_ERR_OR_NULL(priv->ephy_clk))
    {
        ret = clk_prepare_enable(priv->ephy_clk);
        if (ret)
        {
            pr_err("try to enable ephy_clk failed!\n");
            goto ephy_clk_disable;
        }
    }

    phy_interface = priv->phy_interface;

    clk_value = readl(priv->base_phy);
    if (phy_interface == PHY_INTERFACE_MODE_RGMII)
        clk_value |= 0x00000004;
    else
        clk_value &= (~0x00000004);

    clk_value &= (~0x00002003);
    if (phy_interface == PHY_INTERFACE_MODE_RGMII || phy_interface == PHY_INTERFACE_MODE_GMII)
        clk_value |= 0x00000002;
    else if (phy_interface == PHY_INTERFACE_MODE_RMII)
        clk_value |= 0x00002001;

    /**
     * 原厂EFUSE时钟配置逻辑当前保持禁用：
     *
     * if (priv->phy_ext == INT_PHY)
     * {
     *     if (0 != sunxi_efuse_read(EFUSE_OEM_NAME, &efuse_value))
     *         pr_err("get PHY efuse fail!\n");
     *     else
     * #if IS_ENABLED(CONFIG_ARCH_SUN50IW2)
     *         clk_value |= (((efuse_value >> 24) & 0x0F) + 3) << 28;
     * #else
     *         pr_warn("miss config come from efuse!\n");
     * #endif
     * }
     */

    // 配置TX/RX时钟延迟。
    clk_value &= ~(0x07 << 10);
    clk_value |= ((priv->tx_delay & 0x07) << 10);
    clk_value &= ~(0x1F << 5);
    clk_value |= ((priv->rx_delay & 0x1F) << 5);

    writel(clk_value, priv->base_phy);

    return 0;

ephy_clk_disable:
    clk_disable_unprepare(priv->ephy_clk);
assert_reset:
    reset_control_assert(priv->reset);

    return ret;
}

/**
 * @brief 关闭GMAC相关时钟并重新置于复位状态。
 */
static void geth_clk_disable(struct geth_priv *priv)
{
    if (((priv->phy_ext == INT_PHY) || priv->use_ephy_clk) && !IS_ERR_OR_NULL(priv->ephy_clk))
        clk_disable_unprepare(priv->ephy_clk);

    clk_disable_unprepare(priv->geth_clk);
    reset_control_assert(priv->reset);
}

/**
 * @brief 执行TX硬错误恢复并重建TX描述符环。
 */
static void geth_tx_err(struct geth_priv *priv)
{
    netif_stop_queue(priv->ndev);

    sunxi_stop_tx(priv->base);

    geth_free_tx_sk(priv);
    memset(priv->dma_tx, 0, dma_desc_tx * sizeof(struct dma_desc));
    desc_init_chain(priv->dma_tx, (unsigned long)priv->dma_tx_phy, dma_desc_tx);
    priv->tx_dirty = 0;
    priv->tx_clean = 0;
    priv->tx_count_frames = 0;
    sunxi_start_tx(priv->base, priv->dma_tx_phy);

    priv->ndev->stats.tx_errors++;
    netif_wake_queue(priv->ndev);
}

/****************************** 中断与调度 ******************************/

/**
 * @brief 调度GMAC的NAPI处理。
 *
 * 当当前NAPI尚未被调度时，先关闭GMAC中断，
 * 再将NAPI加入内核轮询调度队列。
 */
static inline void geth_schedule(struct geth_priv *priv)
{
    if (likely(napi_schedule_prep(&priv->napi)))
    {
        // NAPI开始处理期间关闭GMAC中断，避免重复中断调度。
        sunxi_int_disable(priv->base);

        // 将当前网卡的NAPI加入内核poll调度队列。
        __napi_schedule(&priv->napi);
    }
}

/**
 * @brief TX描述符延迟回收定时器处理函数。
 *
 * 当TX中断聚合导致部分描述符没有及时触发完成中断时，
 * 定时检查TX环中是否仍存在未回收描述符。
 * 如果仍有待处理描述符，则调度NAPI执行TX完成回收。
 */
static void geth_tx_timer(struct timer_list *t)
{
    struct geth_priv *priv = from_timer(priv, t, tx_timer);

    // 网卡已经停止运行时不再调度TX回收。
    if (!netif_running(priv->ndev))
    {
        return;
    }

    // TX环中仍存在未回收描述符时调度NAPI进行处理。
    if (READ_ONCE(priv->tx_dirty) != READ_ONCE(priv->tx_clean))
    {
        geth_schedule(priv);
    }
}

/**
 * @brief 在持有RX聚合状态锁时启动HIGH模式RX兜底定时器。
 *
 * 调用方必须持有priv->rx_coal_lock。
 */
static void geth_rx_timer_start_locked(struct geth_priv *priv)
{
    if (!priv->rx_timer_ready)
    {
        return;
    }

    if (priv->rx_timer_enabled)
    {
        return;
    }

    priv->rx_timer_enabled = true;

    mod_timer(&priv->rx_timer, jiffies + msecs_to_jiffies(RX_COAL_TIMER_MS));
}

/**
 * @brief 在HIGH模式RX timer仍然启用时重新安排下一次检查。
 */
static void geth_rx_timer_rearm(struct geth_priv *priv)
{
    unsigned long flags;

    spin_lock_irqsave(&priv->rx_coal_lock, flags);

    if (priv->rx_timer_ready && priv->rx_timer_enabled)
    {
        mod_timer(&priv->rx_timer, jiffies + msecs_to_jiffies(RX_COAL_TIMER_MS));
    }

    spin_unlock_irqrestore(&priv->rx_coal_lock, flags);
}

/**
 * @brief 在持有RX聚合状态锁时启动DRAIN高精度定时器。
 *
 * DRAIN刚开始始终使用2ms检查周期。
 * 连续2秒没有收到任何RX以后才允许进入退避，
 * 后续每连续5秒无RX增加5ms，最大20ms。
 *
 * 调用方必须持有priv->rx_coal_lock。
 */
static void geth_rx_drain_timer_start_locked(struct geth_priv *priv)
{
    if (!priv->rx_timer_ready)
    {
        return;
    }

    if (priv->rx_drain_timer_enabled)
    {
        return;
    }

    priv->rx_drain_timer_ms = RX_DRAIN_TIMER_MIN_MS;
    priv->rx_drain_next_backoff = jiffies + msecs_to_jiffies(RX_DRAIN_IDLE_DELAY_MS);
    priv->rx_drain_timer_enabled = true;

    hrtimer_start(&priv->rx_drain_timer, ms_to_ktime(priv->rx_drain_timer_ms), HRTIMER_MODE_REL);
}

/**
 * @brief DRAIN阶段收到RX数据后立即恢复2ms快速检查。
 *
 * 只要DRAIN尚未完成，任何实际RX活动都会取消当前退避状态，
 * 将检查周期恢复为2ms，并重新从2秒无RX开始计算退避时间。
 */
static void geth_rx_drain_activity(struct geth_priv *priv)
{
    unsigned long flags;
    unsigned int old_timer_ms = RX_DRAIN_TIMER_MIN_MS;
    bool restored = false;

    spin_lock_irqsave(&priv->rx_coal_lock, flags);

    if (priv->rx_coal_current == RX_COAL_FRAMES_LOW &&
        priv->rx_drain_timer_enabled)
    {
        old_timer_ms = priv->rx_drain_timer_ms;

        priv->rx_drain_timer_ms = RX_DRAIN_TIMER_MIN_MS;
        priv->rx_drain_next_backoff = jiffies + msecs_to_jiffies(RX_DRAIN_IDLE_DELAY_MS);

        restored = old_timer_ms != RX_DRAIN_TIMER_MIN_MS;

        hrtimer_start(&priv->rx_drain_timer, ms_to_ktime(RX_DRAIN_TIMER_MIN_MS), HRTIMER_MODE_REL);
    }

    spin_unlock_irqrestore(&priv->rx_coal_lock, flags);

    if (restored)
    {
        netdev_info(priv->ndev, "RX coalescing drain timer restored: %u->%u ms\n", old_timer_ms, RX_DRAIN_TIMER_MIN_MS);
    }
}

/**
 * @brief 在持有RX聚合状态锁时判断DRAIN边界是否已经到达。
 *
 * rx_drain_target记录HIGH切换LOW瞬间的rx_clean位置。
 * 当rx_dirty推进到该位置时，说明切换前已经提交的旧HIGH
 * RX描述符全部消费完成，可以关闭DRAIN hrtimer。
 *
 * 调用方必须持有priv->rx_coal_lock。
 */
static bool geth_rx_drain_complete_locked(struct geth_priv *priv)
{
    if (priv->rx_coal_current != RX_COAL_FRAMES_LOW ||
        !priv->rx_drain_timer_enabled ||
        READ_ONCE(priv->rx_dirty) != priv->rx_drain_target)
    {
        return false;
    }

    priv->rx_drain_timer_enabled = false;

    return true;
}

/**
 * @brief 检查RX消费位置是否已经到达DRAIN边界。
 */
static void geth_rx_drain_check_complete(struct geth_priv *priv)
{
    unsigned long flags;
    bool drain_complete;

    spin_lock_irqsave(&priv->rx_coal_lock, flags);
    drain_complete = geth_rx_drain_complete_locked(priv);
    spin_unlock_irqrestore(&priv->rx_coal_lock, flags);

    if (drain_complete)
    {
        netdev_info(priv->ndev, "RX coalescing drain complete\n");
    }
}

/**
 * @brief 根据RX PPS动态调整接收中断聚合策略。
 *
 * 每个统计周期约为1秒。
 *
 * RX PPS达到高阈值时从每包中断切换为16包聚合。
 * RX PPS降低到低阈值时恢复每包中断，并冻结切换瞬间
 * 的rx_clean位置作为旧HIGH描述符的DRAIN结束边界。
 *
 * DRAIN刚开始使用2ms周期；连续2秒无RX后开始退避，
 * 后续每5秒增加5ms，最大20ms。任意RX活动立即恢复2ms。
 *
 * 高低阈值之间保持当前模式，形成滞回区间。
 */
static void geth_rx_coal_update(struct geth_priv *priv, unsigned int packets)
{
    unsigned long flags;
    unsigned long now;
    unsigned int pps;
    unsigned int old_frames;
    unsigned int new_frames;
    unsigned int drain_target = 0;
    unsigned int drain_dirty = 0;

    now = jiffies;

    spin_lock_irqsave(&priv->rx_coal_lock, flags);

    // 累计当前统计周期已经处理的RX描述符数量。
    priv->rx_coal_packets += packets;

    // 尚未到达下一次统计时间时只累计包数。
    if (time_before(now, priv->rx_coal_next))
    {
        spin_unlock_irqrestore(&priv->rx_coal_lock, flags);
        return;
    }

    /**
     * 当前统计周期约为1秒，因此累计包数可以直接作为PPS。
     * LOW/DRAIN模式由RX NAPI驱动检查；
     * HIGH模式由50ms RX timer保证无流量时也能够推进判断。
     */
    pps = priv->rx_coal_packets;

    priv->rx_coal_packets = 0;
    priv->rx_coal_next = now + HZ;

    old_frames = priv->rx_coal_current;
    new_frames = old_frames;

    /**
     * LOW和DRAIN都使用RX=1。
     * PPS重新达到高阈值时立即放弃DRAIN并重新进入HIGH。
     */
    if (old_frames == RX_COAL_FRAMES_LOW)
    {
        if (pps >= RX_COAL_PPS_HIGH)
        {
            new_frames = RX_COAL_FRAMES_HIGH;

            priv->rx_coal_current = new_frames;
            priv->rx_count_frames = 0;

            // 重新进入HIGH后不再需要DRAIN hrtimer和旧DRAIN边界。
            priv->rx_drain_timer_enabled = false;
            priv->rx_drain_target = READ_ONCE(priv->rx_dirty);
            priv->rx_drain_timer_ms = RX_DRAIN_TIMER_MIN_MS;
            priv->rx_drain_next_backoff = 0;

            // HIGH模式使用普通50ms timer作为尾包和PPS检测兜底。
            geth_rx_timer_start_locked(priv);
        }
    }
    // HIGH模式下降到低PPS阈值后进入LOW/DRAIN。
    else if (pps <= RX_COAL_PPS_LOW)
    {
        new_frames = RX_COAL_FRAMES_LOW;

        priv->rx_coal_current = new_frames;
        priv->rx_count_frames = 0;

        /**
         * 停止HIGH模式普通timer。
         * 已经pending的timer最多再进入一次，看到enabled=false后退出。
         */
        priv->rx_timer_enabled = false;

        /**
         * 冻结HIGH模式最后的RX提交边界。
         * geth_rx_refill在同一把rx_coal_lock内完成OWN提交和rx_clean推进，
         * 因此从这一刻开始不会再有遗漏在边界之外的HIGH描述符。
         */
        drain_target = priv->rx_clean;
        drain_dirty = READ_ONCE(priv->rx_dirty);
        priv->rx_drain_target = drain_target;

        /**
         * rx_dirty尚未到达冻结边界时启动DRAIN。
         * 如果二者已经相等，说明切换前提交的旧HIGH描述符已经全部消费。
         */
        if (drain_dirty != drain_target)
        {
            geth_rx_drain_timer_start_locked(priv);
        }
        else
        {
            priv->rx_drain_timer_enabled = false;
        }
    }

    spin_unlock_irqrestore(&priv->rx_coal_lock, flags);

    if (old_frames != new_frames)
    {
        if (new_frames == RX_COAL_FRAMES_LOW)
        {
            netdev_info(priv->ndev, "RX coalescing changed: pps=%u, frames=%u->%u, drain_target=%u, dirty=%u\n", pps, old_frames, new_frames, drain_target, drain_dirty);
        }
        else
        {
            netdev_info(priv->ndev, "RX coalescing changed: pps=%u, frames=%u->%u\n", pps, old_frames, new_frames);
        }
    }
}

/**
 * @brief HIGH模式RX描述符接收兜底定时器处理函数。
 *
 * HIGH模式下每50ms检查RX描述符，防止尾部不足16包的数据
 * 长时间无法触发RX中断，同时在完全无流量时推进PPS判断。
 */
static void geth_rx_timer(struct timer_list *t)
{
    struct geth_priv *priv = from_timer(priv, t, rx_timer);
    struct dma_desc *desc;
    unsigned int entry;

    if (!READ_ONCE(priv->rx_timer_enabled))
    {
        return;
    }

    /**
     * HIGH模式即使已经完全没有RX数据，
     * timer仍然能够推进PPS判断并自动进入LOW/DRAIN。
     */
    geth_rx_coal_update(priv, 0);

    // 自适应判断可能已经切换到LOW并关闭普通timer。
    if (!READ_ONCE(priv->rx_timer_enabled))
    {
        return;
    }

    entry = READ_ONCE(priv->rx_dirty);
    desc = priv->dma_rx + entry;

    // OWN已经返回CPU，说明存在没有依靠RX中断及时处理的数据。
    if (!desc_get_own(desc))
    {
        geth_schedule(priv);
    }

    geth_rx_timer_rearm(priv);
}

/**
 * @brief LOW模式退出HIGH后的RX高精度排空定时器。
 *
 * DRAIN阶段只在rx_dirty尚未到达HIGH切换LOW时冻结的rx_clean边界时运行。
 *
 * 刚进入DRAIN或最近存在RX活动时使用2ms周期；
 * 连续2秒没有收到任何RX以后退避到7ms，
 * 后续每连续5秒无RX增加5ms，依次为12/17/20ms，
 * 最大保持20ms。
 *
 * 任意时刻重新发现RX完成后立即恢复2ms快速检查。
 */
static enum hrtimer_restart geth_rx_drain_timer(struct hrtimer *timer)
{
    struct geth_priv *priv = container_of(timer, struct geth_priv, rx_drain_timer);
    struct dma_desc *desc;
    unsigned long flags;
    unsigned long now;
    unsigned int entry;
    unsigned int old_timer_ms;
    unsigned int timer_ms;
    bool enabled;
    bool rx_ready;
    bool drain_complete = false;
    bool backoff_changed = false;
    bool restored = false;

    spin_lock_irqsave(&priv->rx_coal_lock, flags);

    drain_complete = geth_rx_drain_complete_locked(priv);

    enabled = priv->rx_timer_ready &&
              priv->rx_drain_timer_enabled &&
              priv->rx_coal_current == RX_COAL_FRAMES_LOW;

    spin_unlock_irqrestore(&priv->rx_coal_lock, flags);

    if (drain_complete)
    {
        netdev_info(priv->ndev, "RX coalescing drain complete\n");
    }

    if (!enabled)
    {
        return HRTIMER_NORESTART;
    }

    entry = READ_ONCE(priv->rx_dirty);
    desc = priv->dma_rx + entry;
    rx_ready = !desc_get_own(desc);
    now = jiffies;

    spin_lock_irqsave(&priv->rx_coal_lock, flags);

    drain_complete = geth_rx_drain_complete_locked(priv);

    enabled = priv->rx_timer_ready &&
              priv->rx_drain_timer_enabled &&
              priv->rx_coal_current == RX_COAL_FRAMES_LOW;

    if (!enabled)
    {
        spin_unlock_irqrestore(&priv->rx_coal_lock, flags);

        if (drain_complete)
        {
            netdev_info(priv->ndev, "RX coalescing drain complete\n");
        }

        return HRTIMER_NORESTART;
    }

    old_timer_ms = priv->rx_drain_timer_ms;

    if (rx_ready)
    {
        restored = priv->rx_drain_timer_ms != RX_DRAIN_TIMER_MIN_MS;

        priv->rx_drain_timer_ms = RX_DRAIN_TIMER_MIN_MS;
        priv->rx_drain_next_backoff = now + msecs_to_jiffies(RX_DRAIN_IDLE_DELAY_MS);
    }
    else if (time_after_eq(now, priv->rx_drain_next_backoff))
    {
        /**
         * 第一次退避发生在连续2秒无RX以后。
         * 后续每5秒最多增加5ms，最终限制在20ms。
         */
        if (priv->rx_drain_timer_ms < RX_DRAIN_TIMER_MAX_MS)
        {
            priv->rx_drain_timer_ms += RX_DRAIN_TIMER_STEP_MS;

            if (priv->rx_drain_timer_ms > RX_DRAIN_TIMER_MAX_MS)
            {
                priv->rx_drain_timer_ms = RX_DRAIN_TIMER_MAX_MS;
            }

            backoff_changed = true;
        }

        priv->rx_drain_next_backoff = now + msecs_to_jiffies(RX_DRAIN_BACKOFF_STEP_MS);
    }

    timer_ms = priv->rx_drain_timer_ms;
    hrtimer_forward_now(timer, ms_to_ktime(timer_ms));

    spin_unlock_irqrestore(&priv->rx_coal_lock, flags);

    if (restored)
    {
        netdev_info(priv->ndev, "RX coalescing drain timer restored: %u->%u ms\n", old_timer_ms, RX_DRAIN_TIMER_MIN_MS);
    }

    if (backoff_changed)
    {
        netdev_info(priv->ndev, "RX coalescing drain timer backoff: %u->%u ms\n", old_timer_ms, timer_ms);
    }

    /**
     * 当前descriptor已经由DMA完成。
     * 它可能是旧的dis_ic=1描述符，因此主动调度NAPI。
     */
    if (rx_ready)
    {
        geth_schedule(priv);
    }

    return HRTIMER_RESTART;
}

/**
 * @brief GMAC硬件中断处理函数。
 *
 * 读取并解析GMAC中断状态。
 * 正常TX/RX事件通过NAPI统一处理，
 * TX硬件错误则进入对应的错误恢复流程。
 */
static irqreturn_t geth_interrupt(int irq, void *dev_id)
{
    struct net_device *ndev = (struct net_device *)dev_id;
    struct geth_priv *priv = netdev_priv(ndev);
    int status;

    if (unlikely(!ndev))
    {
        pr_err("%s: invalid ndev pointer\n", __func__);
        return IRQ_NONE;
    }

    // 读取并解析GMAC硬件中断状态。
    status = sunxi_int_status(priv->base, (void *)(&priv->xstats));

    // 正常TX/RX事件统一调度NAPI处理。
    if (likely(status == handle_tx_rx))
    {
        geth_schedule(priv);
    }
    else if (unlikely(status == tx_hard_error_bump_tc))
    {
        netdev_info(ndev, "Do nothing for bump tc\n");
    }
    else if (unlikely(status == tx_hard_error))
    {
        geth_tx_err(priv);
    }
    else
    {
        netdev_info(ndev, "Do nothing.....\n");
    }

    return IRQ_HANDLED;
}

/****************************** 网络设备生命周期 ******************************/

/**
 * @brief 启动GMAC网络设备并初始化PHY、DMA和NAPI。
 */
static int geth_open(struct net_device *ndev)
{
    struct geth_priv *priv = netdev_priv(ndev);
    int ret = 0;

    ret = geth_power_on(priv);
    if (ret)
    {
        netdev_err(ndev, "Power on is failed\n");
        ret = -EINVAL;
    }

    ret = geth_clk_enable(priv);
    if (ret)
    {
        pr_err("%s: clk enable is failed\n", __func__);
        ret = -EINVAL;
    }

    netif_carrier_off(ndev);

    ret = geth_phy_init(ndev);
    if (ret)
    {
        netdev_dbg(ndev, "phy init again...\n");
        ret = geth_phy_init(ndev);
        if (ret)
        {
            netdev_err(ndev, "phy init failed\n");
            ret = -EINVAL;
            goto err;
        }
    }

    ret = sunxi_mac_reset((void *)priv->base, &sunxi_udelay, 10000);
    if (ret)
    {
        netdev_err(ndev, "Initialize hardware error\n");
        goto desc_err;
    }

    sunxi_mac_init(priv->base, txmode, rxmode);
    sunxi_set_umac(priv->base, ndev->dev_addr, 0);

    if (!priv->is_suspend)
    {
        ret = geth_dma_desc_init(ndev);
        if (ret)
        {
            ret = -EINVAL;
            goto desc_err;
        }
    }

    memset(priv->dma_tx, 0, dma_desc_tx * sizeof(struct dma_desc));
    memset(priv->dma_rx, 0, dma_desc_rx * sizeof(struct dma_desc));

    desc_init_chain(priv->dma_rx, (unsigned long)priv->dma_rx_phy, dma_desc_rx);
    desc_init_chain(priv->dma_tx, (unsigned long)priv->dma_tx_phy, dma_desc_tx);

    priv->rx_clean = 0;
    priv->rx_dirty = 0;
    priv->rx_count_frames = 0;

    // RX始终从低延迟模式启动。
    priv->rx_coal_current = RX_COAL_FRAMES_LOW;
    priv->rx_coal_packets = 0;
    priv->rx_drain_target = 0;
    priv->rx_drain_timer_ms = RX_DRAIN_TIMER_MIN_MS;
    priv->rx_coal_next = jiffies + HZ;
    priv->rx_drain_next_backoff = 0;
    priv->rx_timer_enabled = false;
    priv->rx_drain_timer_enabled = false;
    priv->rx_timer_ready = false;

    priv->tx_clean = 0;
    priv->tx_dirty = 0;
    priv->tx_count_frames = 0;

    // 初始RX ring全部按照每包产生完成中断配置。
    geth_rx_refill(ndev);

    memset(&priv->xstats, 0, sizeof(struct geth_extra_stats));

    if (ndev->phydev)
    {
        phy_start(ndev->phydev);
    }

    sunxi_start_rx(priv->base, (unsigned long)((struct dma_desc *)priv->dma_rx_phy + priv->rx_dirty));
    sunxi_start_tx(priv->base, (unsigned long)((struct dma_desc *)priv->dma_tx_phy + priv->tx_clean));

    napi_enable(&priv->napi);

    /**
     * NAPI启用以后才允许自适应机制启动RX timer，
     * 防止初始化ring期间timer提前调度NAPI。
     */
    WRITE_ONCE(priv->rx_timer_ready, true);

    netif_start_queue(ndev);
    sunxi_mac_enable(priv->base);

    return 0;

desc_err:
    geth_phy_release(ndev);

err:
    geth_clk_disable(priv);

    if (priv->is_suspend)
    {
        napi_enable(&priv->napi);
    }

    geth_power_off(priv);

    return ret;
}

/**
 * @brief 停止GMAC网络设备并释放运行期PHY、DMA和定时器资源。
 */
static int geth_stop(struct net_device *ndev)
{
    struct geth_priv *priv = netdev_priv(ndev);
    unsigned long flags;

    netif_stop_queue(ndev);

    /**
     * 首先禁止自适应RX逻辑重新启动timer，
     * 再同步删除已经存在的RX timer。
     */
    spin_lock_irqsave(&priv->rx_coal_lock, flags);
    priv->rx_timer_ready = false;
    priv->rx_timer_enabled = false;
    priv->rx_drain_timer_enabled = false;
    spin_unlock_irqrestore(&priv->rx_coal_lock, flags);

    del_timer_sync(&priv->rx_timer);
    hrtimer_cancel(&priv->rx_drain_timer);

    napi_disable(&priv->napi);
    del_timer_sync(&priv->tx_timer);

    netif_carrier_off(ndev);

    geth_phy_release(ndev);

    sunxi_mac_disable(priv->base);

    geth_clk_disable(priv);
    geth_power_off(priv);

    netif_tx_lock_bh(ndev);

    geth_free_rx_sk(priv);
    geth_free_tx_sk(priv);

    netif_tx_unlock_bh(ndev);

    if (!priv->is_suspend)
    {
        geth_free_dma_desc(priv);
    }

    return 0;
}

/****************************** 数据发送 ******************************/

/**
 * @brief 回收已经完成发送的TX DMA描述符。
 *
 * 从tx_clean开始检查已经提交的TX描述符，回收DMA已经处理完成的
 * 描述符及其SKB，并在TX环空间恢复后重新唤醒发送队列。
 *
 * 如果仍存在尚未完成回收的TX描述符，则启动TX回收定时器，
 * 保证中断聚合场景下剩余描述符最终能够被再次检查和回收。
 */
static void geth_tx_complete(struct geth_priv *priv)
{
    unsigned int entry = 0;
    struct sk_buff *skb = NULL;
    struct dma_desc *desc = NULL;
    int tx_stat;

    spin_lock(&priv->tx_lock);

    while (circ_cnt(priv->tx_dirty, priv->tx_clean, dma_desc_tx) > 0)
    {
        entry = priv->tx_clean;
        desc = priv->dma_tx + entry;

        // DMA仍然持有当前描述符，说明发送尚未完成。
        if (desc_get_own(desc))
        {
            break;
        }

        // 只在帧的最后一个描述符上统计完整帧的发送结果。
        if (desc_get_tx_ls(desc))
        {
            tx_stat = desc_get_tx_status(desc, (void *)(&priv->xstats));

            if (likely(!tx_stat))
            {
                priv->ndev->stats.tx_packets++;
            }
            else
            {
                priv->ndev->stats.tx_errors++;
            }
        }

        // 解除当前描述符对应的数据DMA映射。
        dma_unmap_single(priv->dev, (u32)desc_buf_get_addr(desc), desc_buf_get_len(desc), DMA_TO_DEVICE);

        skb = priv->tx_sk[entry];
        priv->tx_sk[entry] = NULL;

        // 清理当前描述符，重新变为空闲状态。
        desc_init(desc);

        // 推进TX回收位置。
        priv->tx_clean = circ_inc(entry, dma_desc_tx);

        if (unlikely(skb == NULL))
        {
            continue;
        }

        dev_kfree_skb(skb);
    }

    // TX环恢复足够空间后重新启动上层发送队列。
    if (unlikely(netif_queue_stopped(priv->ndev)) && circ_space(priv->tx_dirty, priv->tx_clean, dma_desc_tx) > TX_THRESH)
    {
        netif_wake_queue(priv->ndev);
    }

    spin_unlock(&priv->tx_lock);

    // 仍有TX描述符等待完成时启动定时器，作为中断聚合的回收兜底。
    if (READ_ONCE(priv->tx_dirty) != READ_ONCE(priv->tx_clean) && !timer_pending(&priv->tx_timer))
    {
        mod_timer(&priv->tx_timer, jiffies + msecs_to_jiffies(TX_COAL_TIMER_MS));
    }
}

/**
 * @brief 回滚指定范围内已经建立的TX DMA映射。
 *
 * 回滚范围为[start, end)，end对应尚未成功完成mapping的描述符位置。
 */
static void geth_tx_mapping_rollback(struct geth_priv *priv, unsigned int start, unsigned int end, bool map_as_page)
{
    struct dma_desc *desc;

    while (start != end)
    {
        desc = priv->dma_tx + start;

        /**
         * 当前skb尚未提交首描述符，因此DMA不会越过首描述符
         * 使用后续已经预置OWN的描述符，可以安全撤销OWN状态。
         */
        desc->desc0.all &= ~0x80000000;

        if (map_as_page)
        {
            dma_unmap_page(priv->dev, (u32)desc_buf_get_addr(desc), desc_buf_get_len(desc), DMA_TO_DEVICE);
        }
        else
        {
            dma_unmap_single(priv->dev, (u32)desc_buf_get_addr(desc), desc_buf_get_len(desc), DMA_TO_DEVICE);
        }

        priv->tx_sk[start] = NULL;
        desc_init(desc);

        start = circ_inc(start, dma_desc_tx);
    }
}
/**
 * @brief 将待发送SKB映射为TX DMA描述符并提交给GMAC发送。
 *
 * 函数负责检查TX描述符环空间、建立DMA映射、配置TX描述符、
 * 更新发送环状态并将描述符所有权交给DMA。
 */
static netdev_tx_t geth_xmit(struct sk_buff *skb, struct net_device *ndev)
{
    struct geth_priv *priv = netdev_priv(ndev);
    struct dma_desc *desc;
    struct dma_desc *first;
    dma_addr_t paddr;
    unsigned int entry;
    unsigned int first_entry;
    unsigned int frag_entry;
    unsigned int len;
    unsigned int tmp_len = 0;
    unsigned int coal_frames;
    int nfrags = skb_shinfo(skb)->nr_frags;
    int csum_insert;
    int i;
    bool set_ic;

    // 检查TX描述符环是否有足够空间容纳当前SKB。
    spin_lock(&priv->tx_lock);

    if (unlikely(circ_space(priv->tx_dirty, priv->tx_clean, dma_desc_tx) < (nfrags + 1)))
    {
        if (!netif_queue_stopped(ndev))
        {
            netdev_err(ndev, "%s: BUG! Tx Ring full when queue awake\n", __func__);
            netif_stop_queue(ndev);
        }

        spin_unlock(&priv->tx_lock);

        return NETDEV_TX_BUSY;
    }

#ifdef CONFIG_RTL8363_NB
    // rtk_stat_port_get(EXT_PORT0, STAT_IfInOctets, &cntr);
    // pr_info("%s->%d ======DATA:%llu ============\n", __func__, __LINE__, cntr);
#endif

    // 初始化当前SKB的TX描述符构造状态。
    csum_insert = (skb->ip_summed == CHECKSUM_PARTIAL);
    entry = priv->tx_dirty;
    first_entry = entry;

    first = priv->dma_tx + entry;
    desc = priv->dma_tx + entry;

    len = skb_headlen(skb);
    priv->tx_sk[entry] = skb;

#ifdef PKT_DEBUG
    pr_info("======TX PKT DATA: ============\n");
    print_hex_dump(KERN_DEBUG, "skb->data: ", DUMP_PREFIX_NONE, 16, 1, skb->data, 64, true);
#endif

    // 将SKB线性数据映射到一个或多个TX DMA描述符。
    while (len != 0)
    {
        desc = priv->dma_tx + entry;
        tmp_len = (len > MAX_BUF_SZ) ? MAX_BUF_SZ : len;

        paddr = dma_map_single(priv->dev, skb->data, tmp_len, DMA_TO_DEVICE);

        if (dma_mapping_error(priv->dev, paddr))
        {
            goto linear_mapping_error;
        }

        desc_buf_set(desc, paddr, tmp_len);

        // 首描述符最后提交，其余描述符可以先交给DMA。
        if (first != desc)
        {
            priv->tx_sk[entry] = NULL;
            desc_set_own(desc);
        }

        entry = circ_inc(entry, dma_desc_tx);
        len -= tmp_len;
    }

    /**
     * 记录frag描述符的起始位置。
     * [first_entry, frag_entry)为linear mapping，
     * [frag_entry, entry)为frag mapping。
     */
    frag_entry = entry;

    // 将SKB非线性分片映射到后续TX DMA描述符。
    for (i = 0; i < nfrags; i++)
    {
        const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

        len = skb_frag_size(frag);
        desc = priv->dma_tx + entry;

        paddr = skb_frag_dma_map(priv->dev, frag, 0, len, DMA_TO_DEVICE);

        if (dma_mapping_error(priv->dev, paddr))
        {
            goto frag_mapping_error;
        }

        desc_buf_set(desc, paddr, len);
        desc_set_own(desc);

        priv->tx_sk[entry] = NULL;
        entry = circ_inc(entry, dma_desc_tx);
    }

    // 提交当前SKB占用的TX描述符范围。
    ndev->stats.tx_bytes += skb->len;
    priv->tx_dirty = entry;

    // 根据TX中断合并阈值决定当前帧是否请求完成中断。
    coal_frames = clamp_t(unsigned int, READ_ONCE(tx_coal_frames), 1, TX_THRESH);
    set_ic = ++priv->tx_count_frames >= coal_frames;

    if (set_ic)
    {
        priv->tx_count_frames = 0;
    }

    // TX环接近耗尽时强制请求完成中断，保证后续描述符能够及时回收。
    if (circ_space(priv->tx_dirty, priv->tx_clean, dma_desc_tx) <= (MAX_SKB_FRAGS + 1))
    {
        set_ic = true;
    }

    // 完成当前帧的TX描述符链配置。
    desc_tx_close(first, desc, csum_insert, set_ic);

    // 确保描述符内容先于OWN状态对DMA可见。
    dma_wmb();
    // 最后提交首描述符，使DMA能够开始处理完整帧。
    desc_set_own(first);

    spin_unlock(&priv->tx_lock);

    // TX环空间不足时停止上层发送队列。
    if (circ_space(priv->tx_dirty, priv->tx_clean, dma_desc_tx) <= (MAX_SKB_FRAGS + 1))
    {
        netif_stop_queue(ndev);

        if (circ_space(priv->tx_dirty, priv->tx_clean, dma_desc_tx) > TX_THRESH)
        {
            netif_wake_queue(ndev);
        }
    }

#ifdef DEBUG
    pr_info("=======TX Descriptor DMA: 0x%08llx\n", priv->dma_tx_phy);
    pr_info("Tx pointor: dirty: %d, clean: %d\n", priv->tx_dirty, priv->tx_clean);
    desc_print(priv->dma_tx, dma_desc_tx);
#endif

    // 通知TX DMA处理新提交的描述符。
    sunxi_tx_poll(priv->base);

    // 顺带回收此前已经发送完成的TX描述符。
    geth_tx_complete(priv);

    return NETDEV_TX_OK;

frag_mapping_error:
    // 撤销当前SKB已经成功建立的frag DMA映射。
    geth_tx_mapping_rollback(priv, frag_entry, entry, true);

    // 撤销当前SKB前面已经成功建立的linear DMA映射。
    geth_tx_mapping_rollback(priv, first_entry, frag_entry, false);

    goto mapping_error;

linear_mapping_error:
    // 撤销当前SKB已经成功建立的linear DMA映射。
    geth_tx_mapping_rollback(priv, first_entry, entry, false);

mapping_error:
    /**
     * 第一次mapping本身就失败时rollback范围为空，
     * 因此这里仍需要明确清除预先保存的SKB指针。
     */
    priv->tx_sk[first_entry] = NULL;

    ndev->stats.tx_dropped++;

    spin_unlock(&priv->tx_lock);

    dev_kfree_skb(skb);

    return -EIO;
}

/****************************** 数据接收 ******************************/

/**
 * @brief 处理已经由DMA接收完成的RX描述符。
 *
 * 从rx_dirty开始依次检查RX描述符，将DMA已经完成接收的
 * 数据转换为SKB并交给Linux网络栈处理。
 *
 * 每轮最多处理limit个RX描述符，并使用实际处理的RX描述符
 * 数量更新自适应RX中断聚合统计。
 */
static int geth_rx(struct geth_priv *priv, int limit)
{
    unsigned int rxcount = 0;
    unsigned int entry;
    struct dma_desc *desc;
    struct sk_buff *skb;
    int status;
    int frame_len;

    while (rxcount < limit)
    {
        entry = priv->rx_dirty;
        desc = priv->dma_rx + entry;

        if (desc_get_own(desc))
        {
            break;
        }

        /**
         * rxcount统计DMA实际完成的RX描述符数量。
         * 相比只统计成功交给协议栈的包，它更准确地反映
         * RX中断和descriptor处理压力。
         */
        rxcount++;
        priv->rx_dirty = circ_inc(priv->rx_dirty, dma_desc_rx);

        /**
         * DRAIN只关心是否越过HIGH->LOW时冻结的提交边界。
         * 正常路径只进行一次unlikely快速判断；只有真正命中target
         * 时才获取rx_coal_lock并关闭DRAIN hrtimer。
         */
        if (unlikely(READ_ONCE(priv->rx_drain_timer_enabled) &&
            priv->rx_dirty == READ_ONCE(priv->rx_drain_target)))
        {
            geth_rx_drain_check_complete(priv);
        }

        // 获取DMA接收到的帧长度和接收状态。
        frame_len = desc_rx_frame_len(desc);
        status = desc_get_rx_status(desc, (void *)(&priv->xstats));

        netdev_dbg(priv->ndev, "Rx frame size %d, status: %d\n", frame_len, status);

        // 获取当前RX描述符对应的SKB。
        skb = priv->rx_sk[entry];

        if (unlikely(skb == NULL))
        {
            netdev_err(priv->ndev, "Skb is null\n");
            priv->ndev->stats.rx_dropped++;
            break;
        }

#ifdef PKT_DEBUG
        pr_info("======RX PKT DATA: ============\n");
        print_hex_dump(KERN_DEBUG, "skb->data: ", DUMP_PREFIX_NONE, 16, 1, skb->data, 64, true);
#endif

        // 丢弃硬件判断为异常的接收帧。
        if (status == discard_frame)
        {
            netdev_dbg(priv->ndev, "Get error pkt\n");
            priv->ndev->stats.rx_errors++;
            continue;
        }

        // 普通Ethernet帧去除FCS长度。
        if (unlikely(status != llc_snap))
        {
            frame_len -= ETH_FCS_LEN;
        }

        // 当前SKB即将交给Linux，不再属于RX DMA ring。
        priv->rx_sk[entry] = NULL;

        skb_put(skb, frame_len);

        // 解除当前SKB的RX DMA映射。
        dma_unmap_single(priv->dev, (u32)desc_buf_get_addr(desc), desc_buf_get_len(desc), DMA_FROM_DEVICE);

        // 设置上层协议类型。
        skb->protocol = eth_type_trans(skb, priv->ndev);

        skb->ip_summed = CHECKSUM_UNNECESSARY;

        // 将接收到的SKB交给Linux网络栈。
        napi_gro_receive(&priv->napi, skb);

        priv->ndev->stats.rx_packets++;
        priv->ndev->stats.rx_bytes += frame_len;
    }

#ifdef DEBUG
    if (rxcount > 0)
    {
        pr_info("======RX Descriptor DMA: 0x%08llx=\n", priv->dma_rx_phy);
        pr_info("RX pointor: dirty: %d, clean: %d\n", priv->rx_dirty, priv->rx_clean);
        desc_print(priv->dma_rx, dma_desc_rx);
    }
#endif

    /**
     * 根据本轮实际RX数量更新PPS和聚合模式。
     * HIGH->LOW时只冻结当前rx_clean作为DRAIN结束边界，
     * 后续由rx_dirty推进到该位置时自然完成排空。
     */
    geth_rx_coal_update(priv, rxcount);

    /**
     * DRAIN尚未完成时，只要本轮确实收到RX，
     * 就立即取消7~20ms退避并恢复2ms快速检查。
     */
    if (rxcount > 0)
    {
        geth_rx_drain_activity(priv);
    }

    /**
     * 最后执行refill，使新的聚合配置立即作用于
     * 本轮重新提交的descriptor。
     */
    geth_rx_refill(priv->ndev);

    return rxcount;
}

/**
 * @brief 执行GMAC的NAPI轮询处理。
 *
 * 每轮首先回收已经完成的TX描述符，然后处理RX数据。
 * 当RX处理量小于本轮budget时认为当前工作已经处理完成，
 * 退出NAPI并重新开启GMAC中断。
 */
static int geth_poll(struct napi_struct *napi, int budget)
{
    struct geth_priv *priv = container_of(napi, struct geth_priv, napi);
    int work_done;

    // 回收已经完成发送的TX描述符。
    geth_tx_complete(priv);

    // 处理已经由DMA接收完成的RX描述符。
    work_done = geth_rx(priv, budget);

    // 本轮未耗尽budget，说明当前RX ring已经基本处理完成。
    if (work_done < budget)
    {
        napi_complete(napi);
        sunxi_int_enable(priv->base);
    }

    return work_done;
}

/****************************** Netdev接口 ******************************/

/**
 * @brief 在网卡关闭状态下修改MTU。
 */
static int geth_change_mtu(struct net_device *ndev, int new_mtu)
{
    int max_mtu;

    if (netif_running(ndev))
    {
        pr_err("%s: must be stopped to change its MTU\n", ndev->name);
        return -EBUSY;
    }

    max_mtu = SKB_MAX_HEAD(NET_SKB_PAD + NET_IP_ALIGN);

    if ((new_mtu < 46) || (new_mtu > max_mtu))
    {
        pr_err("%s: invalid MTU, max MTU is: %d\n", ndev->name, max_mtu);
        return -EINVAL;
    }

    ndev->mtu = new_mtu;
    netdev_update_features(ndev);

    return 0;
}

/**
 * @brief 返回GMAC最终启用的网络特性集合。
 */
static netdev_features_t geth_fix_features(struct net_device *ndev, netdev_features_t features)
{
    return features;
}

/**
 * @brief 根据网卡标志和地址列表配置MAC接收过滤模式。
 */
static void geth_set_rx_mode(struct net_device *ndev)
{
    struct geth_priv *priv = netdev_priv(ndev);
    unsigned int value = 0;

    pr_debug("%s: # mcasts %d, # unicast %d\n", __func__, netdev_mc_count(ndev), netdev_uc_count(ndev));

    spin_lock(&priv->lock);
    if (ndev->flags & IFF_PROMISC)
    {
        value = GETH_FRAME_FILTER_PR;
    }
    else if ((netdev_mc_count(ndev) > HASH_TABLE_SIZE) || (ndev->flags & IFF_ALLMULTI))
    {
        value = GETH_FRAME_FILTER_PM; // 接收全部组播报文
        sunxi_hash_filter(priv->base, ~0UL, ~0UL);
    }
    else if (!netdev_mc_empty(ndev))
    {
        u32 mc_filter[2];
        struct netdev_hw_addr *ha;

        // 配置组播地址哈希过滤。
        value = GETH_FRAME_FILTER_HMC;

        memset(mc_filter, 0, sizeof(mc_filter));
        netdev_for_each_mc_addr(ha, ndev)
        {
            /** The upper 6 bits of the calculated CRC are used to
             *  index the contens of the hash table
             */
            int bit_nr = bitrev32(~crc32_le(~0, ha->addr, 6)) >> 26;
            /** The most significant bit determines the register to
             * use (H/L) while the other 5 bits determine the bit
             * within the register.
             */
            mc_filter[bit_nr >> 5] |= 1 << (bit_nr & 31);
        }
        sunxi_hash_filter(priv->base, mc_filter[0], mc_filter[1]);
    }

    // 配置多个单播地址的精确过滤。
    if (netdev_uc_count(ndev) > 16)
    {
        // 单播地址数量超过硬件过滤能力时切换到混杂模式。
        value |= GETH_FRAME_FILTER_PR;
    }
    else
    {
        int reg = 1;
        struct netdev_hw_addr *ha;

        netdev_for_each_uc_addr(ha, ndev)
        {
            sunxi_set_umac(priv->base, ha->addr, reg);
            reg++;
        }
    }

#ifdef FRAME_FILTER_DEBUG
    // 调试过滤失败时启用接收全部报文模式。
    value |= GETH_FRAME_FILTER_RA;
#endif
    sunxi_set_filter(priv->base, value);
    spin_unlock(&priv->lock);
}

/**
 * @brief 处理网络设备TX看门狗超时。
 */
static void geth_tx_timeout(struct net_device *ndev)
{
    struct geth_priv *priv = netdev_priv(ndev);

    geth_tx_err(priv);
}

/**
 * @brief 转发网络设备MII ioctl到PHY层。
 */
static int geth_ioctl(struct net_device *ndev, struct ifreq *rq, int cmd)
{
    if (!netif_running(ndev))
        return -EINVAL;

    if (!ndev->phydev)
        return -EINVAL;

    return phy_mii_ioctl(ndev->phydev, rq, cmd);
}

// 处理ifconfig传入的硬件配置变更。
/**
 * @brief 校验ifconfig传入的网络设备硬件配置变更。
 */
static int geth_config(struct net_device *ndev, struct ifmap *map)
{
    if (ndev->flags & IFF_UP) // 运行中的接口不允许修改硬件配置。
        return -EBUSY;

    // 不允许修改I/O基地址。
    if (map->base_addr != ndev->base_addr)
    {
        pr_warn("%s: can't change I/O address\n", ndev->name);
        return -EOPNOTSUPP;
    }

    // 不允许修改IRQ。
    if (map->irq != ndev->irq)
    {
        pr_warn("%s: can't change IRQ number %d\n", ndev->name, ndev->irq);
        return -EOPNOTSUPP;
    }

    return 0;
}

/**
 * @brief 校验并设置网络设备MAC地址。
 */
static int geth_set_mac_address(struct net_device *ndev, void *p)
{
    struct geth_priv *priv = netdev_priv(ndev);
    struct sockaddr *addr = p;

    if (!is_valid_ether_addr(addr->sa_data))
        return -EADDRNOTAVAIL;

    memcpy(ndev->dev_addr, addr->sa_data, ndev->addr_len);
    sunxi_set_umac(priv->base, ndev->dev_addr, 0);

    return 0;
}

/**
 * @brief 根据网络特性配置MAC回环状态。
 */
int geth_set_features(struct net_device *ndev, netdev_features_t features)
{
    struct geth_priv *priv = netdev_priv(ndev);

    if (features & NETIF_F_LOOPBACK && netif_running(ndev))
        sunxi_mac_loopback(priv->base, 1);
    else
        sunxi_mac_loopback(priv->base, 0);

    return 0;
}

#if IS_ENABLED(CONFIG_NET_POLL_CONTROLLER)
/**
 * @brief 在关闭中断场景下主动轮询GMAC中断处理。
 */
static void geth_poll_controller(struct net_device *dev)
{
    disable_irq(dev->irq);
    geth_interrupt(dev->irq, dev);
    enable_irq(dev->irq);
}
#endif

static const struct net_device_ops geth_netdev_ops =
{
    .ndo_init            = NULL,
    .ndo_open            = geth_open,
    .ndo_start_xmit      = geth_xmit,
    .ndo_stop            = geth_stop,
    .ndo_change_mtu      = geth_change_mtu,
    .ndo_fix_features    = geth_fix_features,
    .ndo_set_rx_mode     = geth_set_rx_mode,
    .ndo_tx_timeout      = geth_tx_timeout,
    .ndo_do_ioctl        = geth_ioctl,
    .ndo_set_config      = geth_config,
#if IS_ENABLED(CONFIG_NET_POLL_CONTROLLER)
    .ndo_poll_controller = geth_poll_controller,
#endif
    .ndo_set_mac_address = geth_set_mac_address,
    .ndo_set_features    = geth_set_features,
};

/****************************** Ethtool接口 ******************************/

/**
 * @brief 检查网络设备是否处于运行状态。
 */
static int geth_check_if_running(struct net_device *ndev)
{
    if (!netif_running(ndev))
        return -EBUSY;
    return 0;
}

/**
 * @brief 返回ethtool指定字符串集合的元素数量。
 */
static int geth_get_sset_count(struct net_device *netdev, int sset)
{
    int len;

    switch (sset)
    {
    case ETH_SS_STATS:
        len = 0;
        return len;
    default:
        return -EOPNOTSUPP;
    }
}

/**
 * 旧版ethtool settings接口当前保持禁用，保留原实现代码：
 *
 * static int geth_ethtool_getsettings(struct net_device *ndev, struct ethtool_cmd *cmd)
 * {
 *     struct geth_priv *priv = netdev_priv(ndev);
 *     struct phy_device *phy = ndev->phydev;
 *     int rc;
 *
 *     if (phy == NULL)
 *     {
 *         netdev_err(ndev, "%s: %s: PHY is not registered\n", __func__, ndev->name);
 *         return -ENODEV;
 *     }
 *
 *     if (!netif_running(ndev))
 *     {
 *         pr_err("%s: interface is disabled: we cannot track " "link speed / duplex setting\n", ndev->name);
 *         return -EBUSY;
 *     }
 *
 *     cmd->transceiver = XCVR_INTERNAL;
 *     spin_lock_irq(&priv->lock);
 *     // rc = phy_ethtool_gset(phy, cmd);
 *     spin_unlock_irq(&priv->lock);
 *
 *     return rc;
 * }
 *
 * static int geth_ethtool_setsettings(struct net_device *ndev, struct ethtool_cmd *cmd)
 * {
 *     struct geth_priv *priv = netdev_priv(ndev);
 *     struct phy_device *phy = ndev->phydev;
 *     int rc;
 *
 *     spin_lock(&priv->lock);
 *     rc = phy_ethtool_sset(phy, cmd);
 *     spin_unlock(&priv->lock);
 *
 *     return rc;
 * }
 */

/**
 * @brief 填充GMAC ethtool驱动信息。
 */
static void geth_ethtool_getdrvinfo(struct net_device *ndev, struct ethtool_drvinfo *info)
{
    strlcpy(info->driver, "sunxi_geth", sizeof(info->driver));

#define DRV_MODULE_VERSION              "SUNXI Gbgit driver V1.1"                 // 驱动版本字符串

    strcpy(info->version, DRV_MODULE_VERSION);
    info->fw_version[0] = '\0';
}

/**
 * @brief 读取GMAC TX/RX Pause流控状态。
 */
static void geth_ethtool_get_pauseparam(struct net_device *ndev, struct ethtool_pauseparam *epause)
{
    struct geth_priv *priv = netdev_priv(ndev);

    // TODO: 后续补充Pause流控自动协商支持。
    epause->tx_pause = sunxi_read_tx_flowctl(priv->base);
    epause->rx_pause = sunxi_read_rx_flowctl(priv->base);
}

/**
 * @brief 设置GMAC TX/RX Pause流控状态。
 */
static int geth_ethtool_set_pauseparam(struct net_device *ndev, struct ethtool_pauseparam *epause)
{
    struct geth_priv *priv = netdev_priv(ndev);

    sunxi_write_tx_flowctl(priv->base, !!epause->tx_pause);
    netdev_info(ndev, "Tx flow control %s\n", epause->tx_pause ? "ON" : "OFF");

    sunxi_write_rx_flowctl(priv->base, !!epause->rx_pause);
    netdev_info(ndev, "Rx flow control %s\n", epause->rx_pause ? "ON" : "OFF");

    return 0;
}

static const struct ethtool_ops geth_ethtool_ops =
{
    .begin             = geth_check_if_running,
    // .get_settings   = geth_ethtool_getsettings,
    // .set_settings   = geth_ethtool_setsettings,
    .get_link          = ethtool_op_get_link,
    .get_pauseparam    = geth_ethtool_get_pauseparam,
    .set_pauseparam    = geth_ethtool_set_pauseparam,
    .get_ethtool_stats = NULL,
    .get_strings       = NULL,
    .get_wol           = NULL,
    .set_wol           = NULL,
    .get_sset_count    = geth_get_sset_count,
    .get_drvinfo       = geth_ethtool_getdrvinfo,
};

/****************************** 硬件资源 ******************************/

/**
 * @brief 获取并初始化GMAC平台硬件资源。
 */
static int geth_hw_init(struct platform_device *pdev)
{
    struct net_device *ndev = platform_get_drvdata(pdev);
    struct geth_priv *priv = netdev_priv(ndev);
    struct device_node *np = pdev->dev.of_node;
    int ret = 0;
    struct resource *res;
    u32 value;
    enum of_gpio_flags flag;
    const char *gmac_power;
    char power[20];
    int i;

#ifdef CONFIG_SUNXI_EXT_PHY
    priv->phy_ext = EXT_PHY;
#else
    priv->phy_ext = INT_PHY;
#endif

    // 获取并映射GMAC和PHY寄存器资源。
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (unlikely(!res))
    {
        pr_err("%s: ERROR: get gmac memory failed", __func__);
        return -ENODEV;
    }

    priv->base = devm_ioremap_resource(&pdev->dev, res);
    if (!priv->base)
    {
        pr_err("%s: ERROR: gmac memory mapping failed", __func__);
        return -ENOMEM;
    }

    res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
    if (unlikely(!res))
    {
        pr_err("%s: ERROR: get phy memory failed", __func__);
        ret = -ENODEV;
        goto mem_err;
    }

    priv->base_phy = devm_ioremap_resource(&pdev->dev, res);
    if (unlikely(!priv->base_phy))
    {
        pr_err("%s: ERROR: phy memory mapping failed", __func__);
        ret = -ENOMEM;
        goto mem_err;
    }

    // 获取并申请GMAC中断。
    ndev->irq = platform_get_irq_byname(pdev, "gmacirq");
    if (ndev->irq == -ENXIO)
    {
        pr_err("%s: ERROR: MAC IRQ not found\n", __func__);
        ret = -ENXIO;
        goto irq_err;
    }

    ret = request_irq(ndev->irq, geth_interrupt, IRQF_SHARED, dev_name(&pdev->dev), ndev);
    if (unlikely(ret < 0))
    {
        pr_err("Could not request irq %d, error: %d\n", ndev->irq, ret);
        goto irq_err;
    }

    // 获取GMAC复位控制器。
    priv->reset = devm_reset_control_get(&pdev->dev, NULL);
    if (IS_ERR(priv->reset))
    {
        pr_err("%s: Get gmac reset control failed!\n", __func__);
        return PTR_ERR(priv->reset);
    }

    // 获取GMAC及EPHY时钟。
    priv->geth_clk = of_clk_get_by_name(np, "gmac");
    if (unlikely(!priv->geth_clk || IS_ERR(priv->geth_clk)))
    {
        pr_err("Get gmac clock failed!\n");
        ret = -EINVAL;
        goto clk_err;
    }

    if (INT_PHY == priv->phy_ext)
    {
        priv->ephy_clk = of_clk_get_by_name(np, "ephy");
        if (unlikely(IS_ERR_OR_NULL(priv->ephy_clk)))
        {
            pr_err("Get ephy clock failed!\n");
            ret = -EINVAL;
            goto clk_err;
        }
    }
    else
    {
        if (!of_property_read_u32(np, "use_ephy25m", &(priv->use_ephy_clk)) && priv->use_ephy_clk)
        {
            priv->ephy_clk = of_clk_get_by_name(np, "ephy");
            if (unlikely(IS_ERR_OR_NULL(priv->ephy_clk)))
            {
                pr_err("Get ephy clk failed!\n");
                ret = -EINVAL;
                goto clk_err;
            }
        }
    }

    // 获取外部PHY电源Regulator。
    if (EXT_PHY == priv->phy_ext)
    {
        for (i = 0; i < POWER_CHAN_NUM; i++)
        {
            snprintf(power, 15, "gmac-power%d", i);
            ret = of_property_read_string(np, power, &gmac_power);
            if (ret)
            {
                priv->gmac_power[i] = NULL;
                pr_info("gmac-power%d: NULL\n", i);
                continue;
            }
            priv->gmac_power[i] = regulator_get(NULL, gmac_power);
            if (IS_ERR(priv->gmac_power[i]))
            {
                pr_err("gmac-power%d get error!\n", i);
                ret = -EINVAL;
                goto clk_err;
            }
        }
    }
    // 读取PHY接口模式和时钟延迟参数。
    priv->phy_interface = of_get_phy_mode(np);
    if (priv->phy_interface != PHY_INTERFACE_MODE_MII && priv->phy_interface != PHY_INTERFACE_MODE_RGMII && priv->phy_interface != PHY_INTERFACE_MODE_RMII)
    {
        pr_err("Not support phy type!\n");
        priv->phy_interface = PHY_INTERFACE_MODE_MII;
    }

    if (!of_property_read_u32(np, "tx-delay", &value))
        priv->tx_delay = value;

    if (!of_property_read_u32(np, "rx-delay", &value))
        priv->rx_delay = value;

    // 配置外部PHY复位GPIO和Pinctrl。
    if (EXT_PHY == priv->phy_ext)
    {
        priv->phyrst = of_get_named_gpio_flags(np, "phy-rst", 0, &flag);
        priv->rst_active_low = (flag == OF_GPIO_ACTIVE_LOW) ? 1 : 0;

        if (gpio_is_valid(priv->phyrst))
        {
            if (gpio_request(priv->phyrst, "phy-rst") < 0)
            {
                pr_err("gmac gpio request fail!\n");
                ret = -EINVAL;
                goto pin_err;
            }
        }

        priv->pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
        if (IS_ERR_OR_NULL(priv->pinctrl))
        {
            pr_err("gmac pinctrl error!\n");
            priv->pinctrl = NULL;
            ret = -EINVAL;
            goto pin_err;
        }
    }

    return 0;

pin_err:
    if (EXT_PHY == priv->phy_ext)
    {
        for (i = 0; i < POWER_CHAN_NUM; i++)
        {
            if (IS_ERR_OR_NULL(priv->gmac_power[i]))
                continue;
            regulator_put(priv->gmac_power[i]);
        }
    }
clk_err:
    free_irq(ndev->irq, ndev);
irq_err:
    devm_iounmap(&pdev->dev, priv->base_phy);
mem_err:
    devm_iounmap(&pdev->dev, priv->base);

    return ret;
}

/**
 * @brief 释放GMAC平台硬件资源。
 */
static void geth_hw_release(struct platform_device *pdev)
{
    struct net_device *ndev = platform_get_drvdata(pdev);
    struct geth_priv *priv = netdev_priv(ndev);
    int i;

    devm_iounmap(&pdev->dev, (priv->base_phy));
    devm_iounmap(&pdev->dev, priv->base);
    free_irq(ndev->irq, ndev);
    if (priv->geth_clk)
        clk_put(priv->geth_clk);

    if (EXT_PHY == priv->phy_ext)
    {
        for (i = 0; i < POWER_CHAN_NUM; i++)
        {
            if (IS_ERR_OR_NULL(priv->gmac_power[i]))
                continue;
            regulator_put(priv->gmac_power[i]);
        }

        if (!IS_ERR_OR_NULL(priv->pinctrl))
            devm_pinctrl_put(priv->pinctrl);

        if (gpio_is_valid(priv->phyrst))
            gpio_free(priv->phyrst);
    }

    if (!IS_ERR_OR_NULL(priv->ephy_clk))
        clk_put(priv->ephy_clk);
}

/****************************** 驱动生命周期 ******************************/

/**
 * @brief 探测并注册Sunxi GMAC网络设备。
 */
static int geth_probe(struct platform_device *pdev)
{
    int ret = 0;
#ifdef CONFIG_RTL8363_NB
    // 使用net_device和geth_priv全局变量。
#else
    struct net_device *ndev = NULL;
    struct geth_priv *priv;
#endif

#if IS_ENABLED(CONFIG_OF)
    pdev->dev.dma_mask = &geth_dma_mask;
    pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
#endif

    ndev = alloc_etherdev(sizeof(struct geth_priv));
    if (!ndev)
    {
        dev_err(&pdev->dev, "could not allocate device.\n");
        return -ENOMEM;
    }
    SET_NETDEV_DEV(ndev, &pdev->dev);

    priv = netdev_priv(ndev);
    platform_set_drvdata(pdev, ndev);

    // 调用硬件初始化前先保存platform私有数据。
    ret = geth_hw_init(pdev);
    if (0 != ret)
    {
        pr_err("geth_hw_init fail!\n");
        goto hw_err;
    }
#ifdef CONFIG_RTL8363_NB
    rtl8363nb_vb_init();
#endif

    // 初始化net_device能力和操作接口。
    ether_setup(ndev);
    ndev->netdev_ops = &geth_netdev_ops;
    netdev_set_default_ethtool_ops(ndev, &geth_ethtool_ops);
    ndev->base_addr = (unsigned long)priv->base;

    priv->ndev = ndev;
    priv->dev = &pdev->dev;
    timer_setup(&priv->tx_timer, geth_tx_timer, 0);
    timer_setup(&priv->rx_timer, geth_rx_timer, 0);
    hrtimer_init(&priv->rx_drain_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    priv->rx_drain_timer.function = geth_rx_drain_timer;

    // TODO: 后续补充VLAN帧硬件能力支持。
    ndev->hw_features = NETIF_F_SG | NETIF_F_HIGHDMA | NETIF_F_IP_CSUM |
                NETIF_F_IPV6_CSUM | NETIF_F_RXCSUM;

    ndev->features |= ndev->hw_features;
    ndev->hw_features |= NETIF_F_LOOPBACK;
    ndev->priv_flags |= IFF_UNICAST_FLT;

    ndev->watchdog_timeo = msecs_to_jiffies(watchdog);

    netif_napi_add(ndev, &priv->napi, geth_poll,  BUDGET);

    spin_lock_init(&priv->lock);
    spin_lock_init(&priv->tx_lock);
    spin_lock_init(&priv->rx_coal_lock);

    // 最后一个参数为MDC时钟分频比。
    sunxi_geth_register((void *)ndev->base_addr, HW_VERSION, 0x03);

    ret = register_netdev(ndev);
    if (ret)
    {
        netif_napi_del(&priv->napi);
        pr_err("Error: Register %s failed\n", ndev->name);
        goto reg_err;
    }

    // 打开网卡前完成MAC地址校验和设置。
    geth_check_addr(ndev, mac_str);

#ifdef CONFIG_GETH_ATTRS
    geth_create_attrs(ndev);
#endif
    device_create_file(&pdev->dev, &dev_attr_gphy_test);
    device_create_file(&pdev->dev, &dev_attr_mii_read);
    device_create_file(&pdev->dev, &dev_attr_mii_write);
    device_create_file(&pdev->dev, &dev_attr_loopback_test);
    device_create_file(&pdev->dev, &dev_attr_extra_tx_stats);
    device_create_file(&pdev->dev, &dev_attr_extra_rx_stats);

    device_enable_async_suspend(&pdev->dev);

#if IS_ENABLED(CONFIG_PM)
    INIT_WORK(&priv->eth_work, geth_resume_work);
#endif

    netdev_dbg(ndev, "[gmac] probe success\n");
    return 0;

reg_err:
    geth_hw_release(pdev);
hw_err:
    platform_set_drvdata(pdev, NULL);
    free_netdev(ndev);

    return ret;
}

/**
 * @brief 注销并释放Sunxi GMAC网络设备。
 */
static int geth_remove(struct platform_device *pdev)
{
    struct net_device *ndev = platform_get_drvdata(pdev);
    struct geth_priv *priv = netdev_priv(ndev);

    device_remove_file(&pdev->dev, &dev_attr_gphy_test);
    device_remove_file(&pdev->dev, &dev_attr_mii_read);
    device_remove_file(&pdev->dev, &dev_attr_mii_write);
    device_remove_file(&pdev->dev, &dev_attr_loopback_test);
    device_remove_file(&pdev->dev, &dev_attr_extra_tx_stats);
    device_remove_file(&pdev->dev, &dev_attr_extra_rx_stats);

    unregister_netdev(ndev);
    netif_napi_del(&priv->napi);
    geth_hw_release(pdev);
    platform_set_drvdata(pdev, NULL);
    free_netdev(ndev);

    return 0;
}

/****************************** 驱动注册 ******************************/

static const struct of_device_id geth_of_match[] =
{
    {
        .compatible = "allwinner,sunxi-gmac",
    },
    {},
};
MODULE_DEVICE_TABLE(of, geth_of_match);

static struct platform_driver geth_driver =
{
    .probe  = geth_probe,
    .remove = geth_remove,
    .driver =
    {
        .name           = "sunxi-gmac",
        .owner          = THIS_MODULE,
        .pm             = &geth_pm_ops,
        .of_match_table = geth_of_match,
    },
};
module_platform_driver(geth_driver);

#ifndef MODULE
/**
 * @brief 解析内核启动参数中的GMAC MAC地址。
 */
static int __init set_mac_addr(char *str)
{
    char *p = str;

    if (str && strlen(str))
        memcpy(mac_str, p, 18);

    return 0;
}
__setup("mac_addr=", set_mac_addr);
#endif

MODULE_DESCRIPTION("Allwinner Gigabit Ethernet driver");
MODULE_AUTHOR("fuzhaoke <fuzhaoke@allwinnertech.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0.1");

# LinkG C 语言代码格式规范

> 适用于 LinkG 项目全部 `.c` / `.h` 文件。

> 后续新增、修改、重构、Code Review、Codex 自动修改均必须遵循本规范。

> 本规范优先约束****书写格式、注释格式、分类方式和可读性****，不得在不同模块中自行发明新的风格。

---

## 1. 核心原则

LinkG C 代码统一遵循以下原则：

1. 文件结构清晰，按职责分类；

2. 头文件简洁，只表达接口和类型；

3. C 文件负责实现和功能说明；

4. 类型、变量、宏和右侧注释必须工整对齐；

5. 单行说明统一使用 `//`；

6. 多行说明统一使用 `/** ... */`；

7. 禁止使用普通 `/* ... */` 作为代码说明；

8. 函数声明优先紧凑，头文件中的函数声明必须保持单行；

9. 不增加无意义的 `@param`、`@return` 注释；

10. 同一种格式必须贯穿整个工程，不允许一个文件一种风格。

---

# 2. 通用排版

## 2.1 缩进

统一使用 **4 个空格**。

禁止使用 Tab 进行代码缩进或人为对齐。

```c

if (ret != 0)

{

    return ret;

}

```

---

## 2.2 大括号

函数、`if`、`for`、`while`、`switch` 的左大括号统一单独一行。

```c

if (channel == NULL)

{

    return -EINVAL;

}

```

禁止：

```c

if (channel == NULL) {

    return -EINVAL;

}

```

---

## 2.3 空行

不同逻辑阶段之间保留一个空行。

禁止连续堆叠多个无意义空行。

```c

ret = _linkg_xxx_prepare(context);

if (ret != 0)

{

    return ret;

}

ret = _linkg_xxx_start(context);

if (ret != 0)

{

    return ret;

}

```

---

# 3. 文件头

每个 `.c` 文件必须使用统一文件头：

```c

/**

 * @file linkg_example.c

 * @brief LinkG示例模块实现

 * @author Dawn

 * @version 1.0.0

 * @date 2026-08-25

 */

```

头文件使用简化形式：

```c

/**

 * @file linkg_example.h

 * @brief LinkG示例模块接口

 */

```

文件头属于多行说明，因此必须使用 `/** ... */`。

---

# 4. Include 顺序

固定顺序：

1. 当前模块对应头文件；

2. C 标准库 / POSIX / Linux 系统头文件；

3. LinkG 公共头文件；

4. 当前模块内部头文件。

不同类别之间空一行。

```c

#include "linkg_example.h"

#include <errno.h>

#include <pthread.h>

#include <stdbool.h>

#include <stdint.h>

#include <string.h>

#include "linkg_log.h"

#include "linkg_thread.h"

#include "linkg_example_internal.h"

```

头文件仅包含接口实际需要的依赖。

禁止为了方便把大量无关头文件放进公共 `.h`。

---

# 5. 功能分类分隔符

所有 `.c` / `.h` 文件都统一使用以下形式进行功能分类：

```c

/****************************** 类型定义 ******************************/

/****************************** 模块常量 ******************************/

/****************************** 内部类型 ******************************/

/****************************** 全局上下文 ******************************/

/****************************** 内部辅助 ******************************/

/****************************** 生命周期 ******************************/

/****************************** 配置接口 ******************************/

/****************************** 状态查询 ******************************/

/****************************** 数据发送 ******************************/

/****************************** 数据接收 ******************************/

```

要求：

- 所有分类分隔符使用****完全相同的宽度和格式****；

- 分类名称简洁；

- 按职责排列；

- 没有内容的分类不要保留；

- 禁止自行使用其他长度的星号分隔符；

- 禁止混用 `// ======`、`/* ----- */` 等其他分隔风格。

---

# 6. 头文件规范

## 6.1 头文件职责

头文件只负责：

- 宏定义；

- 枚举；

- `typedef`；

- 对外结构体；

- opaque type；

- 对外函数声明。

头文件不负责解释实现过程。

---

## 6.2 函数声明必须单行并对齐返回类型列

**所有头文件函数声明必须保持单行。**

同一功能分类中的函数声明必须让函数名起始列纵向对齐。

返回类型较短时通过补空格对齐，不改变类型本身。

最常见规则：

```c

int  linkg_thread_init(linkg_thread_t *thread, const char *name, linkg_thread_func_t function, void *user_data);

int  linkg_thread_start(linkg_thread_t *thread);

int  linkg_thread_stop(linkg_thread_t *thread);

void linkg_thread_deinit(linkg_thread_t *thread);

```

即：

```text

int   长度3，后补2个空格后进入函数名列

void  长度4，后补1个空格后进入函数名列

```

核心要求不是固定某个空格数量，而是：

> **同一组函数的函数名首字符必须纵向对齐。**

例如存在更长返回类型时，应按该组最长返回类型统一调整：

```c

bool     linkg_text_equal(const char *left, const char *right);

uint64_t linkg_time_monotonic_us(void);

int      linkg_time_sleep_ms(uint32_t timeout_ms);

```

正确：

```c

int  rg255_cmd_query_pdp_context(rg255_t *module, char *response, int response_size);

void rg255_cmd_reset(rg255_t *module);

```

禁止：

```c

int rg255_cmd_query_pdp_context(rg255_t *module,

                                char *response,

                                int response_size);

```

即使参数较多，公共头文件中的函数声明也不主动拆行。

---

## 6.3 头文件函数声明不写逐函数注释

函数只按照职责分类摆放。

正确：

```c

/****************************** 生命周期 ******************************/

int rg255_init(const linkg_cellular_config_t *config);

int rg255_start(void);

int rg255_stop(void);

int rg255_deinit(void);

/****************************** 状态查询 ******************************/

int rg255_get_status(rg255_status_t *status);

```

禁止：

```c

/**

 * @brief 启动RG255模块。

 */

int rg255_start(void);

```

公共接口的业务说明应集中在模块设计文档中，不在头文件重复大量注释。

---

## 6.4 Opaque Type

优先隐藏内部结构：

```c

typedef struct rg255 rg255_t;  // RG255模块实例

```

内部结构定义放到模块内部头文件。

---

# 7. 宏定义规范

宏必须：

- 名称包含模块前缀；

- 数值有明确单位时在名称中体现；

- 右侧统一使用 `//` 中文注释；

- `.h` 和 `.c` 文件中的宏定义均遵守全文件统一对齐规则。

## 7.1 宏定义全文件统一对齐

同一个 `.h` 或 `.c` 文件中的所有 `#define` 必须采用****全文件统一对齐规则****。

要求：

1. 不同功能分类之间可以使用统一分类分隔符，但****不得按分类分别计算宏定义对齐宽度****；

2. 以当前文件中****最长的宏名称****作为宏值列的统一对齐基准；

3. 文件内所有宏值的起始列必须纵向对齐；

4. 文件内所有右侧 `//` 注释的起始列应尽量纵向对齐；

5. 新增更长的宏名称后，应重新调整当前文件其他宏定义的对齐；

6. 对齐统一使用空格，禁止使用 Tab；

7. `.h` 和 `.c` 文件中的宏定义均遵守本规则；

8. 不允许出现某一分类单独对齐、另一分类使用不同宏值列的情况；

9. 对齐仅调整空格，不得为了排版修改宏名称、宏值或业务语义。

正确：

```c

/****************************** 接口配置 ******************************/

#define WAL_RADIO_STATUS_IOCTL              (0x89F0 + 5) // 无线状态私有ioctl命令
#define WAL_RADIO_STATUS_ABI_VERSION        1U           // 接口ABI版本
#define WAL_RADIO_STATUS_MAC_LENGTH         6U           // 无线MAC地址长度
#define WAL_RADIO_STATUS_MAX_PEERS          16U          // 单次返回最大对端数量
#define WAL_RADIO_STATUS_REFRESH_MS         250U         // 状态缓存刷新周期，单位毫秒

/****************************** 整体状态标志 ******************************/

#define WAL_RADIO_FLAG_NOISE_VALID          (1U << 0)    // 噪声强度有效
#define WAL_RADIO_FLAG_PEERS_TRUNCATED      (1U << 1)    // 对端列表已截断
#define WAL_RADIO_FLAG_PARTIAL              (1U << 2)    // 部分状态查询失败

/****************************** 对端状态标志 ******************************/

#define WAL_RADIO_PEER_FLAG_VALID           (1U << 0)    // 对端基础信息有效
#define WAL_RADIO_PEER_FLAG_QUERY_FAILED    (1U << 1)    // 对端详细状态查询失败

/****************************** 无线角色 ******************************/

#define WAL_RADIO_ROLE_UNKNOWN              0U           // 未知角色
#define WAL_RADIO_ROLE_STA                  1U           // STA角色
#define WAL_RADIO_ROLE_AP                   2U           // AP角色

```

以上代码虽然属于多个不同功能分类，但所有宏定义仍以整个文件最长的宏名称：

```text

WAL_RADIO_PEER_FLAG_QUERY_FAILED

```

作为统一对齐基准。

核心要求：

> **宏定义按整个文件统一对齐，不按功能分类分别对齐。**

禁止：

```c

/****************************** 整体状态标志 ******************************/

#define WAL_RADIO_FLAG_NOISE_VALID     (1U << 0) // 噪声强度有效
#define WAL_RADIO_FLAG_PARTIAL         (1U << 2) // 部分状态查询失败

/****************************** 对端状态标志 ******************************/

#define WAL_RADIO_PEER_FLAG_VALID        (1U << 0) // 对端基础信息有效
#define WAL_RADIO_PEER_FLAG_QUERY_FAILED (1U << 1) // 对端详细状态查询失败

/****************************** 无线角色 ******************************/

#define WAL_RADIO_ROLE_UNKNOWN 0U // 未知角色
#define WAL_RADIO_ROLE_STA     1U // STA角色
#define WAL_RADIO_ROLE_AP      2U // AP角色

```

上述写法虽然各分类内部进行了局部对齐，但不同分类之间的宏值列不一致，不符合 LinkG 全文件统一对齐规则。

禁止：

```c

#define DATA_PORT 5003 /* data */

#define VIDEO_PORT 5005

```

---

# 8. 枚举规范

枚举成员必须：

- 命名完整；

- 值按需要显式定义；

- 等号和值尽量对齐；

- 右侧使用 `//` 注释说明语义。

```c

typedef enum

{

    RG255_STATE_STOPPED      = 0, // 已停止

    RG255_STATE_STARTING,         // 正在启动

    RG255_STATE_SIM_READY,        // SIM已就绪

    RG255_STATE_REGISTERING,      // 正在驻网

    RG255_STATE_CONNECTING,       // 正在建立数据连接

    RG255_STATE_ONLINE,           // IPv4和Global IPv6均可用

    RG255_STATE_DEGRADED,         // 链路可用但能力不完整

    RG255_STATE_RECONNECTING,     // 正在恢复连接

    RG255_STATE_PIN_REQUIRED,     // SIM需要PIN

    RG255_STATE_FAILED            // 当前连接失败

} rg255_state_t;

```

---

# 9. 结构体规范

## 9.1 成员必须右侧注释

结构体成员必须：

- 类型对齐；

- 成员名对齐；

- 右侧注释对齐；

- 注释统一使用 `//`；

- 注释说明所有权、引用、生命周期等重要语义。

```c

typedef struct

{

    linkg_packet_t *packet;     // 待发送数据包，Queue持有一个引用

    linkg_path_t   *path;       // 发送路径，Queue持有一个引用

    uint64_t        enqueue_us; // 入队时间

} linkg_cellular_tx_item_t;

```

你指定的引用说明统一采用这种形式：

```c

linkg_packet_t *packet; // 待发送数据包，Queue持有一个引用

```

不要改写成：

```c

linkg_packet_t *packet; /* Queue owns packet ref */

```

---

## 9.2 Context 类型

模块运行上下文成员必须表达用途：

```c

typedef struct

{

    pthread_mutex_t lock;          // 模块状态锁

    linkg_thread_t  worker;        // 后台工作线程

    int             socket_fd;     // UDP Socket

    bool            initialized;   // 是否已经初始化

    bool            running;       // 是否正在运行

} linkg_example_context_t;

```

禁止使用：

```c

int fd;

void *data;

bool flag;

```

这种缺少语义的成员名。

---

# 10. 全局变量

文件内部全局变量必须：

- 使用 `static`；

- 名称有模块前缀；

- 初始化项对齐；

- 初始化项右侧使用 `//` 注释。

```c

static linkg_example_context_t g_example =

{

    .socket_fd   = -1,    // Socket尚未创建

    .initialized = false, // 尚未初始化

    .running     = false  // 尚未运行

};

```

---

# 11. C 文件函数规范

## 11.1 每个函数必须有功能描述

`.c` 文件中的所有函数，包括 `static` 内部函数，都必须在函数前使用简洁的 `/** ... */` 描述功能。

```c

/**

 * @brief 获取当前蜂窝网络接口索引。

 */

static int _cellular_link_get_interface_index(uint32_t *interface_index)

{

    ...

}

```

公开函数同样如此：

```c

/**

 * @brief 启动蜂窝数据链路。

 */

int linkg_cellular_link_start(linkg_link_t *link)

{

    ...

}

```

---

## 11.2 函数注释保持简洁

普通函数只写 `@brief`。

禁止机械添加：

```text

@param

@return

```

只有以下情况才补充正文说明：

- 参数可为 `NULL`；

- 参数具有所有权；

- Queue/模块会持有引用；

- 调用方必须持锁；

- 必须按特定生命周期顺序调用；

- 存在容易误解的重要约束。

正确：

```c

/**

 * @brief 将数据包加入等待队列。

 *

 * 成功入队后Queue分别持有Packet和Path的一个引用，

 * 出队、丢弃或停止清理时必须对称释放。

 */

static int _cellular_link_enqueue(...)

{

    ...

}

```

---

# 12. 函数内部注释规范

## 12.1 单行说明统一使用 `//`

正确：

```c

// Queue持有Packet和Path引用。

linkg_packet_retain(packet);

linkg_path_acquire(path);

```

正确：

```c

// Cellular Link允许晚于RG255生命周期启动获得Global IPv6。

if (interface_index == 0U)

{

    return -ENODEV;

}

```

禁止：

```c

/* Queue持有Packet和Path引用。 */

```

---

## 12.2 多行说明统一使用 `/** ... */`

**多行注释开头必须是两个星号 `/**`。**

正确：

```c

/**

 * REALTIME数据不进入等待队列。

 * Socket出现EAGAIN或ENOBUFS时立即向上层返回拥塞状态，

 * 避免实时数据因为排队而失去时效性。

 */

```

禁止使用普通 C 块注释：

```c

/*

 * REALTIME数据不进入等待队列。

 */

```

整个 LinkG 工程统一只保留两种说明注释：

```text

// 单行说明

/**

 * 多行说明

 */

```

不得混用第三种形式。

---

# 13. 函数定义与调用换行

## 13.1 函数定义

`.c` 文件函数定义在可读的情况下优先单行：

```c

static int _cellular_link_send_batch(linkg_link_t *link, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)

```

只有明显过长并严重影响阅读时才允许拆分。

头文件声明仍然必须单行。

---

## 13.2 函数调用

简单调用保持单行：

```c

ret = linkg_network_interface_get_global_ipv6(LINKG_RESOURCE_INTERFACE_CELLULAR, &address);

```

过长时按参数语义整齐换行：

```c

ret = linkg_route_set_ipv4(&report->node.ethernet,

                           &report->node.node_address);

```

禁止为了“看起来整齐”机械做到一参数一行。

---

# 14. 条件判断

简单条件直接表达。

```c

if (module == NULL)

{

    return -EINVAL;

}

```

复杂条件应拆分，避免一条 `if` 同时承担多个业务判断。

推荐：

```c

if (peer == NULL)

{

    return -EINVAL;

}

if (!peer->online)

{

    return -ENETDOWN;

}

```

不推荐：

```c

if (peer == NULL || !peer->online || peer->path == NULL || !peer->used)

{

    return -EINVAL;

}

```

---

# 15. 错误处理

统一使用负 `errno`。

```text

0    成功

< 0  错误

```

错误处理优先提前返回：

```c

ret = _linkg_xxx_prepare(context);

if (ret != 0)

{

    return ret;

}

```

不得把多个无关错误统一转换成一个模糊错误码。

---

# 16. 锁与引用注释

涉及锁、引用和对象所有权时必须明确说明。

示例：

```c

pthread_mutex_t send_lock; // 发送锁，保护共享发送缓冲区和发送统计

```

```c

linkg_packet_t *packet; // 待发送数据包，Queue持有一个引用

linkg_path_t   *path;   // 发送路径，Queue持有一个引用

```

函数内部：

```c

// 获取Queue持有的Packet和Path引用。

linkg_packet_retain(packet);

linkg_path_acquire(path);

```

释放：

```c

// 释放Queue持有的Packet和Path引用。

linkg_packet_release(packet);

linkg_path_release(path);

```

锁保护范围必须尽量小，不允许在没有说明的情况下跨阻塞调用持锁。

---

# 17. 头文件标准模板

```c

/**

 * @file linkg_example.h

 * @brief LinkG示例模块接口

 */

#ifndef LINKG_EXAMPLE_H

#define LINKG_EXAMPLE_H

#include <stdbool.h>

#include <stdint.h>

/****************************** 宏定义 ******************************/

#define LINKG_EXAMPLE_QUEUE_CAPACITY 64U // 队列容量

/****************************** 类型定义 ******************************/

typedef enum

{

    LINKG_EXAMPLE_STATE_STOPPED = 0, // 已停止

    LINKG_EXAMPLE_STATE_RUNNING      // 正在运行

} linkg_example_state_t;

typedef struct linkg_example linkg_example_t; // 示例模块实例

/****************************** 生命周期 ******************************/

int  linkg_example_create(linkg_example_t **example);

int  linkg_example_start(linkg_example_t *example);

int  linkg_example_stop(linkg_example_t *example);

void linkg_example_destroy(linkg_example_t *example);

/****************************** 状态查询 ******************************/

int linkg_example_get_state(linkg_example_t *example, linkg_example_state_t *state);

#endif

```

---

# 18. C 文件标准模板

```c

/**

 * @file linkg_example.c

 * @brief LinkG示例模块实现

 * @author Dawn

 * @version 1.0.0

 * @date 2026-08-25

 */

#include "linkg_example.h"

#include <errno.h>

#include <pthread.h>

#include <stdbool.h>

#include <stdint.h>

#include <string.h>

/****************************** 模块常量 ******************************/

#define LINKG_EXAMPLE_TIMEOUT_MS 3000U // 默认超时时间

/****************************** 内部类型 ******************************/

typedef struct

{

    void    *data;      // 待处理数据

    uint64_t timestamp; // 入队时间

} linkg_example_item_t;

/****************************** 全局上下文 ******************************/

static linkg_example_context_t g_example =

{

    .socket_fd   = -1,    // Socket尚未创建

    .initialized = false, // 尚未初始化

    .running     = false  // 尚未运行

};

/****************************** 内部辅助 ******************************/

/**

 * @brief 重置模块运行状态。

 */

static void _linkg_example_reset_locked(void)

{

    // 调用方已经持有模块状态锁。

    g_example.running = false;

}

/****************************** 生命周期 ******************************/

/**

 * @brief 初始化示例模块。

 */

int linkg_example_init(void)

{

    return 0;

}

/**

 * @brief 启动示例模块。

 */

int linkg_example_start(void)

{

    return 0;

}

/**

 * @brief 停止示例模块。

 */

int linkg_example_stop(void)

{

    return 0;

}

```

---

# 19. 禁止事项

整个 LinkG 工程禁止：

```c

/* 单行说明 */

```

禁止：

```c

/*

 * 多行说明

 */

```

禁止头文件逐函数写：

```c

/**

 * @brief ...

 */

int xxx(...);

```

禁止头文件函数声明人为拆成多行。

禁止结构体成员无注释。

禁止宏定义无注释。

禁止同一文件中混用不同分隔符。

禁止不同模块自行定义新的注释格式。

禁止为了形式大量增加没有信息量的注释。

---

# 20. 重构执行要求

后续 LinkG 重构每处理一个文件必须执行：

1. 按本规范整理文件结构；

2. 修正分类分隔；

3. 头文件函数声明恢复单行；

4. 按全文件统一规则对齐宏定义，并整理枚举、结构体和右侧注释；

5. 右侧与单行说明统一改成 `//`；

6. 多行说明统一改成 `/** ... */`；

7. C 文件每个函数添加简洁功能描述；

8. 删除无意义和重复注释；

9. 不在格式整理过程中偷偷改变业务行为；

10. 格式整理完成后再进行模块逻辑重构。

---

# 21. 提交前检查清单

- [ ] 文件头格式统一；

- [ ] Include 顺序正确；

- [ ] 分类分隔符完全统一；

- [ ] 头文件函数声明全部单行；

- [ ] 同一功能分类中的函数名起始列纵向对齐；

- [ ] 头文件函数没有逐函数说明注释；

- [ ] 当前文件所有宏定义均以全文件最长宏名为基准统一对齐；

- [ ] 所有宏值起始列纵向对齐；

- [ ] 所有宏定义右侧 `//` 注释尽量纵向对齐；

- [ ] 枚举全部右侧 `//` 注释并工整对齐；

- [ ] 结构体成员全部右侧 `//` 注释并工整对齐；

- [ ] C 文件所有函数都有 `/** @brief ... */`；

- [ ] 函数内部单行说明全部使用 `//`；

- [ ] 多行说明全部使用 `/** ... */`；

- [ ] 没有普通 `/* ... */` 说明注释；

- [ ] 引用所有权有明确说明；

- [ ] 锁用途和保护对象有明确说明；

- [ ] 无无意义注释；

- [ ] 无与当前任务无关的格式改动；

- [ ] 格式调整没有改变业务行为。

---

# 22. 最终原则

所有 LinkG C 文件必须做到：

> **打开文件即可快速知道：这个文件属于什么模块、有哪些类型、有哪些状态、有哪些接口、每一块代码负责什么。**

格式本身服务于工程阅读，不追求花哨。

统一标准：

```text

.h：接口简洁、声明单行、分类明确、类型工整。

.c：职责清晰、函数有描述、内部说明统一、锁和引用明确。

宏：.h / .c 均按全文件最长宏名统一对齐宏值列和右侧注释列。

注释：单行只用 //，多行只用 /** ... */。

```

后续任何重构如果与本规范冲突，应先更新本规范，再修改代码，禁止在单个文件中自行例外。

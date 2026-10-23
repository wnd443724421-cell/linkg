# LinkG 配置模块
## 1. 模块概述
LinkG 配置模块负责管理系统运行所需的全局静态配置，提供配置加载、JSON 解析、默认值填充、完整校验、线程安全读取、整体替换和原子保存能力。
公共头文件位于 `include/linkg/config/`，模块实现位于 `infra/config/`。
默认配置文件路径：
```text
/app/current/config/linkg.json
```
配置模块只管理配置数据，不负责启动、停止或重启 Wi-Fi、蜂窝网络、TUN、以太网等业务模块。
## 2. 核心目的
配置模块需要保证：
- 业务模块不直接解析 JSON 文件；
- 加载失败不破坏上一份有效配置；
- 配置替换前完成完整校验；
- 配置读取只返回结构副本；
- 配置保存使用原子替换；
- 配置修改成功后立即持久化；
- 反初始化只释放资源，不隐式保存。
## 3. 职责边界
### 3.1 模块负责
配置模块负责文件读取、JSON 解析、默认值填充、字段校验、跨字段校验、当前配置管理、快照读取、分类读取、整体替换、原子保存和并发保护。
### 3.2 模块不负责
配置模块不负责业务模块启停、网络接口配置、TUN 创建、链路切换、运行状态保存、配置应用通知、父目录创建和退出时自动保存。
## 4. 配置结构
顶层配置类型：
```c
typedef struct
{
    linkg_device_config_t  device;  /* 设备配置 */
    linkg_network_config_t network; /* 网络配置 */
    linkg_links_config_t   links;   /* 链路配置 */
} linkg_config_t;
```
配置文件包含 `device`、`network` 和 `links`：
```json
{
    "device": { "role": "ap" },
    "network": {
        "tun": { "ip": "10.8.0.1", "netmask": "255.255.255.0" },
        "ethernet": { "ip": "192.168.1.1", "netmask": "255.255.255.0" }
    },
    "links": {
        "wifi": {
            "enabled": true,
            "ap": {
                "ssid": "LinkG",
                "password": "12345678",
                "channel": 149,
                "security": "wpa2-psk"
            },
            "sta": {
                "ssid": "LinkG",
                "password": "12345678",
                "security": "wpa2-psk"
            },
            "wideband": {
                "work_mode": "narrow",
                "narrow_params": { "mode": "fixed", "bandwidth": 10, "manual_rate": 1 },
                "wide_params": { "ap_bandwidth": 40 }
            }
        },
        "cellular": { "enabled": false }
    }
}
```
## 5. 设备角色
```c
typedef enum
{
    LINKG_DEVICE_ROLE_UNKNOWN = 0, /* 未知设备角色 */
    LINKG_DEVICE_ROLE_AP,          /* AP 管理节点 */
    LINKG_DEVICE_ROLE_STA          /* STA 接入节点 */
} linkg_device_role_t;
typedef struct
{
    linkg_device_role_t role; /* 设备运行角色 */
} linkg_device_config_t;
```
角色规则：
- `device.role` 支持 `ap` 和 `sta`，是系统唯一全局角色；
- AP 角色必须提供有效的 `wifi.ap`；
- STA 角色必须提供有效的 `wifi.sta`；
- 非当前角色配置允许缺失，并由默认值补全；
- `links.wifi` 不读取、不校验、也不输出 `role`；
- `LINKG_DEVICE_ROLE_UNKNOWN` 不能作为有效运行角色。
## 6. JSON 与字段校验
JSON 规则：
- 根节点必须是对象，字段名称区分大小写；
- 同一对象内重复键会被拒绝；
- 根对象后的非空白内容会被拒绝；
- 未知字段暂时忽略，但不保证保存时完整回写；
- 布尔字段必须使用 JSON 的 `true` 或 `false`；
- 必需字段缺失或字段类型错误返回 `CONFIG_ERR_PARSE`。
网络规则：
- IPv4 地址必须是合法点分十进制地址；
- 地址不能为 `0.0.0.0` 或 `255.255.255.255`；
- 子网掩码必须非零且二进制位连续。
Wi-Fi 规则：
- SSID 长度为 1～32 字节；
- 安全模式支持 `open` 和 `wpa2-psk`；
- `open` 模式密码必须为空；
- WPA2 密码长度为 8～63 个允许字符；
- AP 信道必须属于当前工作模式支持范围；
- 窄带带宽固定为 10 MHz，手动速率范围为 0～5；
- 宽带 AP 带宽支持 20、40 和 80 MHz。
蜂窝配置当前只预留基础结构和启用状态，不解析 APN、SIM、认证和拨号字段，也不保证未知蜂窝字段完整保存回读。
## 7. 生命周期
生命周期：
```text
未初始化
    ↓ linkg_config_load()
已初始化并运行
    ↓ linkg_config_deinit()
未初始化
```
加载流程：
```text
确定路径
    ↓
读取文件并检查 JSON
    ↓
设置默认值并解析临时结构
    ↓
完整校验
    ↓
获取写锁并替换当前配置
```
加载和校验全部成功前不得修改全局配置。加载失败时，上一份有效配置保持不变。
`linkg_config_deinit()` 只负责清空当前配置、清除初始化状态和释放内部运行状态，不保存配置、不修改文件、不通知业务模块。
## 8. 并发模型
配置模块使用读写锁保护当前配置和初始化状态。
读锁接口：
- `linkg_config_create_snapshot()`；
- `linkg_config_get_device()`；
- `linkg_config_get_network()`；
- `linkg_config_get_links()`；
- `linkg_config_get_wifi()`；
- `linkg_config_get_cellular()`。
写锁操作包括加载成功后的替换、`linkg_config_replace()` 和 `linkg_config_deinit()`。
所有读取接口只返回结构副本，不返回内部指针。跨分类读取应使用完整快照，避免连续调用多个 getter 时读到不同配置版本。
持有配置锁期间禁止执行文件读写、JSON 格式化、网络通信、外部进程调用、业务模块启停或可能回调配置模块的操作。
## 9. 配置修改与持久化
推荐修改流程：
```text
创建完整快照
    ↓
修改副本并完整校验
    ↓
原子保存
    ↓
替换当前配置
    ↓
通知业务模块应用
```
推荐顺序为“校验、保存、替换”，这样保存失败时当前运行配置不会变化。
配置修改成功后应立即保存，不能依赖应用退出时统一保存。多个线程同时更新配置时，上层必须串行化“保存并替换”事务。
原子保存流程：
```text
生成 JSON
    ↓
写入同目录临时文件
    ↓
刷新并关闭
    ↓
rename 替换目标文件
```
目标文件不存在时，只要父目录存在且可写，就可以重新创建；父目录不存在时返回 `CONFIG_ERR_FILE`。
## 10. 错误码
| 错误码 | 说明 |
|---|---|
| `CONFIG_OK` | 操作成功 |
| `CONFIG_ERR_PARAM` | 接口参数无效 |
| `CONFIG_ERR_FILE` | 文件读取或保存失败 |
| `CONFIG_ERR_PARSE` | JSON、字段类型、必需字段、重复键或尾随内容错误 |
| `CONFIG_ERR_VALIDATE` | 数值范围或业务规则校验失败 |
| `CONFIG_ERR_MEMORY` | 内存分配失败 |
底层文件和 JSON 错误码必须转换为统一配置错误码。
## 11. 顶层对外接口
### 11.1 `LINKG_CONFIG_DEFAULT_PATH`
```c
#define LINKG_CONFIG_DEFAULT_PATH "/app/current/config/linkg.json"
```
定义默认用户配置文件路径，接口路径参数为 `NULL` 时使用该路径。
### 11.2 `linkg_config_load`
```c
int linkg_config_load(const char *path);
```
**设计意图：** 从文件加载完整配置，并在解析和校验全部成功后事务性替换当前配置。
**参数：** `path` 为配置文件路径；传入 `NULL` 时使用默认路径。
**返回值：** 成功返回 `CONFIG_OK`；文件、解析、校验或内存失败返回对应错误码。
**约束：**
- 加载失败不能破坏旧配置；
- 不负责创建缺失文件；
- 不负责启动或重启业务模块。
### 11.3 `linkg_config_deinit`
```c
void linkg_config_deinit(void);
```
**设计意图：** 清空当前配置并释放模块状态。
**约束：**
- 应在业务模块停止后调用；
- 不保存配置；
- 不执行文件操作；
- 调用后 getter 不可继续使用。
### 11.4 `linkg_config_create_snapshot`
```c
int linkg_config_create_snapshot(linkg_config_t *out);
```
**设计意图：** 获取完整配置副本，保证所有分类来自同一配置版本。
**约束：**
- `out` 不能为空；
- 返回副本可在锁外使用和修改；
- 修改副本不会自动更新全局配置。
### 11.5 分类 getter
```c
int linkg_config_get_device(linkg_device_config_t *out);
int linkg_config_get_network(linkg_network_config_t *out);
int linkg_config_get_links(linkg_links_config_t *out);
int linkg_config_get_wifi(linkg_wifi_config_t *out);
int linkg_config_get_cellular(linkg_cellular_config_t *out);
```
**设计意图：** 获取对应分类的当前配置副本。
**约束：**
- 输出参数不能为空；
- 不返回内部指针；
- 同时需要设备角色和 Wi-Fi 配置时应使用完整快照；
- 返回成功不代表业务模块已经应用该配置。
### 11.6 `linkg_config_replace`
```c
int linkg_config_replace(const linkg_config_t *config);
```
**设计意图：** 完整校验并替换当前内存配置。
**约束：**
- `config` 不能为空；
- 替换前必须完整校验；
- 只修改内存配置；
- 不自动保存文件；
- 不自动通知业务模块；
- 需要持久化时应先调用 `linkg_config_save()`。
### 11.7 `linkg_config_save`
```c
int linkg_config_save(const linkg_config_t *config, const char *path);
```
**设计意图：** 将指定配置完整校验后原子保存为 JSON 文件。
**参数：** `config` 为待保存配置；`path` 为保存路径，传入 `NULL` 时使用默认路径。
**约束：**
- 文件 I/O 必须在配置锁外执行；
- 保存成功不代表业务配置已经生效；
- 未知字段不会被保留；
- 父目录必须已经存在。
## 12. 设备配置接口
### 12.1 `linkg_device_config_set_default`
```c
void linkg_device_config_set_default(linkg_device_config_t *out);
```
设置设备配置默认值。默认角色应为 `LINKG_DEVICE_ROLE_UNKNOWN`，不能因默认值跳过必需字段校验。
### 12.2 `linkg_device_config_parse`
```c
int linkg_device_config_parse(const cJSON *node, linkg_device_config_t *out);
```
从 `device` JSON 对象解析设备配置，只负责字段读取和类型转换，不负责 Wi-Fi 角色关联校验。
### 12.3 `linkg_device_config_validate`
```c
int linkg_device_config_validate(const linkg_device_config_t *config);
```
校验设备角色是否合法，`LINKG_DEVICE_ROLE_UNKNOWN` 必须返回 `CONFIG_ERR_VALIDATE`。
### 12.4 `linkg_device_config_to_json`
```c
int linkg_device_config_to_json(cJSON *parent, const char *key, const linkg_device_config_t *config);
```
将设备配置序列化到指定 JSON 父对象，只输出当前版本支持的合法字段。
## 13. 分类配置接口规范
网络、链路、Wi-Fi 和蜂窝配置模块应统一提供：
```c
void <module>_set_default(<type> *out);
int  <module>_parse(const cJSON *node, <type> *out);
int  <module>_validate(const <type> *config);
int  <module>_to_json(cJSON *parent, const char *key, const <type> *config);
```
| 接口 | 职责 |
|---|---|
| `set_default` | 设置确定的默认值 |
| `parse` | JSON 到结构体转换 |
| `validate` | 校验本分类字段和规则 |
| `to_json` | 将合法配置序列化到 JSON |
跨分类规则集中在顶层完整配置校验中，避免子模块互相依赖。
## 14. 启动与退出顺序
建议启动顺序：
```text
时间模块 → 日志模块 → 生命周期模块 → 配置模块 → 业务模块
```
建议退出顺序：
```text
业务模块 stop/deinit → linkg_config_deinit() → 生命周期和日志模块释放
```
配置修改已即时保存，因此退出前不需要再次保存。
## 15. 单元测试
测试目录：
```text
tests/config/
```
执行：
```sh
cd tests/config && make clean test
```
```sh
cd tests/config && make sanitize
```
测试应覆盖：
- 正常 AP 和 STA 配置；
- 必需字段缺失、字段类型错误、重复键和尾随垃圾；
- IPv4、子网掩码、SSID、密码、信道和带宽边界；
- 非当前角色默认值补全；
- 加载失败不破坏旧配置；
- 并发快照和整体替换；
- 保存到指定路径和默认路径；
- 目标文件不存在时创建；
- 父目录不存在时返回错误；
- 保存后重新加载；
- Sanitizer 下无内存错误和数据竞争。
蜂窝模块正式实现前，不测试具体字段语义和完整保存回读。
## 16. 后续扩展
后续可增加配置版本迁移、恢复出厂配置、统一更新事务、变更通知、差异比较、备份回滚、蜂窝配置完整实现和敏感字段保护。
扩展时必须保持加载和替换的事务性，不暴露内部可变对象，并继续分离持久化与反初始化职责。

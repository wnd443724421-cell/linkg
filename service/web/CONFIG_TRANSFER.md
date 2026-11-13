# 配置导入与导出

页面入口：`#/system/config`，菜单为“系统管理 → 配置导入与导出”。

## 模块边界

| 模块 | 职责 |
|---|---|
| `infra/config/config_codec.c` | 复用各配置域解析器，完整校验与统一 JSON 序列化 |
| `infra/config/linkg_config.c` | 快照、原子保存、导入提交后的写入保护 |
| `service/web/handlers/web_handler_config.c` | Web 参数、实例校验、成功应答与应答后重载 |
| `www/js/api/config_file.js` | 1031/1032 JSON 协议封装 |
| `www/js/pages/system_config.js` | 文件选择、下载、用户确认、重连与配置回读 |

不修改 `app/linkg_main.c`。导入成功后使用现有 `RESTART_SERVICES` 循环，正常执行 stop/deinit/load/init/start，保留当前进程；若现有生命周期清理或重新启动失败，继续使用原有失败处理。

## 接口

- 1031：导出全局配置快照，返回 `data.config` 和建议文件名；浏览器将配置对象下载为 JSON，不创建公共下载目录中的配置文件。
- 1032：`param` 包含原始文件文本 `content`、当前服务 `instance` 和严格布尔值 `confirm: true`。成功返回 `apply: restarting`、新配置的 `management_ip` 和规范化的 `expected_config`。

导入文件最多 64 KiB，必须完整包含 `device`、`network`、`links`、`paths` 和 `uart` 五个配置域。后端处理原始文本，拒绝重复键、尾随垃圾和非法业务参数。配置解析复用启动时的校验规则，不要求另一个产品的备份格式兼容。

CGI 与 Web 控制服务的 JSON 请求上限同步为 128 KiB。文件经过 JSON 字符串编码后仍需满足请求上限，前端在发送前检查编码后的字节数。发布时需同步更新 LinkG、`cmain` CGI 和 `www`。

## 提交与生效

1. Dispatcher 独占配置事务；与已有异步 UART 应用互斥。
2. 解析、校验并构造成功响应，尚不修改运行快照。
3. 使用现有原子写接口保存 `/app/linkgUser.json`，权限为 `0600`。
4. 成功提交后阻止旧配置再次保存；完成响应并关闭后端连接后请求全服务重载。响应发送失败也需重载已提交的配置。
5. 新生命周期重新加载用户文件。前端等待服务实例变化，再导出并逐项比较规范化配置；只有实例变化及配置回读一致才显示确认信息。

原子写失败时尝试恢复之前的完整配置；恢复失败明确返回持久化状态无法确认，不伪报成功。网络地址改变或重连超时不等于导入失败，页面显示目标地址并要求检查设备后再操作。

备份包含 Wi-Fi 密码、SIM PIN 等敏感字段，浏览器不将备份内容写入 localStorage/sessionStorage。当前全局配置可能包含已保存、等待服务重载的设置，导出不采集驱动运行统计或出厂身份信息。

## 验证

`test_config_transfer` 在临时目录使用真实文件原子写实现，覆盖三路串口共 48 条规则与 SBUS 往返、损坏 JSON、重复键、确认和实例保护、写前与替换后失败恢复、文件权限、提交后防覆盖、应答失败后重载及重新加载结果。

```bash
cmake -S test -B build/config-transfer-tests -DLINKG_TEST_SANITIZERS=ON
cmake --build build/config-transfer-tests --parallel 4
ctest --test-dir build/config-transfer-tests --output-on-failure
```

该测试可通过 `test_config_transfer --gateway` 提供 JSON 行测试桥，用于浏览器集成验证。桥接模式中网络、生命周期和服务实例为隔离替身，配置解析、Web 处理器及临时目录中的保存和重新加载为真实实现。此结果不代表实板外设重初始化或地址切换验收。

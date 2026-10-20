# OTA 后端与 Web 快速参考

入口：`#/system/upgrade`。上传接口为 `POST /cgi-bin/upgrade` 原始文件字节流，Content-Length 最大 64 MiB，不能使用 multipart/form-data。CGI 成功只表示上传到 `/tmp/upgrade.tar.gz`。

WebCmd 1021 需参数 `{"confirm":true}`，返回 accepted 只表示任务已接收。1022 每次读取 `/app/version_state.json` 的 upgrade_state；文件损坏或不可读返回明确错误，不使用内存 idle 冒充结果。

## 后台顺序

```text
CGI锁下接管文件 → 持久化verifying → eventfd唤醒linkg_thread_t
  → archive_extract → manifest_verify
  → 持久化installing → 备用App/Web安装并同步
  → 比较kernel/boot.img的SHA-256
  → 确有差异时持久化flashing → 写备用分区 → 重新打开读回校验
  → 持久化installing → 可选system_install
  → 重新核对完整状态和实际Kernel
  → JSON候选及rebooting → ENV批处理/读回 → 请求应用清理和整机重启
  → Supervisor testing → completed或rolling_back / rolled_back
```

空闲 worker 使用 poll(-1) 等待 eventfd。已接受任务在停止时等待完成，不因普通清理中断 Flash/ENV 写工具。拒绝请求不消费上传包；单任务租约与状态文件 CAS 防止跨进程覆盖。

安装期间不修改稳定 App/Web 或两条 current。压缩包与解压 Kernel 始终在 tmpfs，Kernel 不复制到 `/app`。App-only 或相同 Kernel 复用稳定槽位，ENV已匹配时不重复擦写。

失败先恢复稳定 JSON 目标，再修复 ENV。无法确定的结果保留给启动恢复，不请求依赖成功的 reboot，也不虚假确认稳定版本。共享系统文件不随 App/Web 回滚。

## 文件地图

| 文件 | 职责 |
|---|---|
| linkg_upgrade.c | 线程生命周期、单任务互斥、上传包接管及流水线 |
| upgrade_archive.c / upgrade_manifest.c | 归档目录及 FILES.txt 校验 |
| upgrade_install.c | 只安装非稳定 Release 的 App/Web |
| upgrade_kernel.c | 活动槽位保护、比较、备用 NOR 写入及读回 |
| upgrade_system.c | 可选系统声明预检查、临时文件和原子替换 |
| upgrade_commit.c | 安装租约、阶段 CAS、JSON→ENV 事务及失败恢复 |
| infra/base/linkg_release_state.c | 共用严格状态格式及跨进程锁 |
| infra/base/linkg_atomic_file.c | 有界读取及 fsync/rename 持久化 |
| infra/base/linkg_boot_env.c | 共用 ENV 锁、读写工具生命周期及读回 |
| service/web/handlers/web_handler_upgrade.c | 1021/1022 |
| www/js/pages/system_upgrade.js | 上传、确认、状态、重启重连和退出清理 |

## 发布

先构建本版 LinkG 和 Supervisor，将对应 ARM 程序与最新 www 放入 `upgrade/app/bin/linkg`、`upgrade/app/usr/sbin/linkg-supervisor`、`upgrade/www`。更新管理程序时保留 `upgrade/system_install.json` 固定路径声明。不更新 Kernel 时从待打包目录移除 kernel/boot.img；保留时按需比较和刷写。

```bash
./upgrade/build.sh
```

包名读取仓库 VERSION。打包脚本在临时 staging 中生成 Web 缓存指纹，指纹不是软件版本；最终包顶层只有 upgrade/。首次必须先部署兼容的 Supervisor 并让它实际启动，再用新 LinkG 执行 OTA。详见 `supervisor/README.md` 和 `upgrade/SUPERVISOR_UPDATE.md`。

## 隔离测试

```bash
python3 test/upgrade/build_tests.py
python3 test/upgrade/test_ota.py
python3 test/upgrade/test_system_install.py
```

测试目标固定放在 build-native/ota。使用普通文件模拟 Flash，链接包装限制系统安装根目录，原有 ENV 替身及重启标记模拟持久化与复位；不触及真实开发板。

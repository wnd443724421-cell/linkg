# Supervisor 系统文件更新约定

Supervisor 统一安装到 `/usr/sbin/linkg-supervisor`，由现有 rc/procd 启动入口运行。App/Web 继续使用原有 A/B Release；不通过 `/app/current/bin/linkg-supervisor` 启动管理进程。

## 包内路径

```text
upgrade/
├── system_install.json
├── app/
│   ├── bin/linkg
│   └── usr/sbin/linkg-supervisor
└── www/
```

`system_install.json`：

```json
{
  "schema_version": 1,
  "files": [
    {
      "source": "usr/sbin/linkg-supervisor",
      "destination": "/usr/sbin/linkg-supervisor"
    }
  ]
}
```

`source` 默认相对于包内 `app/`，因此上面的文件实际位于 `upgrade/app/usr/sbin/linkg-supervisor`。兼容已有带 `app/` 前缀的声明，两种形式都限定在包内 `app/`。源文件必须为可执行普通文件，路径及其父目录不能是符号链接。

升级声明只允许固定的 `/usr/sbin/linkg-supervisor` 以及既有 `/vendor/firmware/`、`/lib/firmware/` 文件，不能任意覆盖系统目录。打包端与安装端遵循同一范围。声明缺失时跳过系统文件安装。

## 生效时机

1. 现有 LinkG 完成归档、Manifest 和安装校验。
2. 系统安装模块将新 Supervisor 复制到目标同目录临时文件，设置权限并同步文件。
3. 用 `rename` 替换 `/usr/sbin/linkg-supervisor`，再同步父目录。
4. 正在运行的旧 Supervisor 继续使用旧 inode，不在安装中途重启或热切换。
5. OTA 候选提交完成后由原有重启流程重启系统。
6. rc/procd 从固定路径启动新 Supervisor，新 Supervisor 再选择并启动 LinkG。

这里规定的是安装与启动关系；系统文件替换成功不等于整个 OTA 成功。JSON、ENV、候选提交及确认仍需各自完成并检查结果。

## 兼容边界

- 新 Supervisor 必须兼容候选 LinkG，也必须兼容可能回滚到的稳定 LinkG。共享 Supervisor 不随 App/Web A/B 回滚。
- 修改 READY/HEARTBEAT/RESTARTING/STOPPING 协议时，需要同时验证前后两个 LinkG 版本。
- 磁盘上的文件替换不会升级正在运行的旧 Supervisor。首次增加 `upgrade_state` 前，运行中的 Supervisor 也必须已经支持扩展格式；需先部署兼容的过渡版本，或由 OTA 接受请求的兼容性检查拒绝旧运行版本。不能仅凭包内带有新 Supervisor，就允许旧运行版本期间写入它不能解析的 JSON。
- 原子替换避免读到半个程序，但不证明一个 ABI 或行为错误的新 Supervisor 可以启动。共享系统文件不属于本次 App/Web 回滚范围。
- 不要用 `cp` 直接覆盖正在运行的目标，也不要在安装过程中先杀死旧 Supervisor。

## 隔离测试

在 Ubuntu 仓库根目录执行：

```bash
python3 test/upgrade/test_system_install.py
```

测试把所有系统目标重定向到临时目录。包括截图中的相对路径、旧前缀兼容、执行权限、路径及符号链接限制、声明预检查、原子替换运行中的程序，以及打包端校验；不访问真实 Flash、ENV 或开发板，不执行 reboot。

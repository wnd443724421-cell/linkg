# LinkG AT临时维护

- UDP 29610，允许以太网、LinkG虚拟接口和回环入口。
- 指令名称不区分大小写；密码和on/off参数区分大小写。
- 使用与LQ10相同形式的固定随机工厂密码：32字节随机数，以64个十六进制字符表示。两产品不要复用同一密码。
- 工厂密码不编译进固件或提交仓库。设备从 /etc/linkg/factory_password 读取，要求root所有、普通文件且组/其他用户无权限。缺失或无效时拒绝授权；不影响基础AT服务。

## 指令

| 指令 | 功能 | 权限 |
| --- | --- | --- |
| AT | 连通性检测 | 普通 |
| AT+FACTORY=<password> | 获取60秒临时工厂权限 | 普通 |
| AT+SSH=on | 开启60分钟SSH维护 | 工厂 |
| AT+SSH=off | 关闭SSH | 工厂 |
| AT+SSH? | 查询Dropbear进程状态 | 工厂 |
| AT+SSH=? | 查询支持的参数 | 工厂 |

使用同一个UDP socket发送认证和后续指令。工厂授权绑定客户端源IP、源端口、本机目的地址和入口接口；只保留一个授权会话。认证失败限频1秒。会话到期后须重新认证；SSH维护期限独立计时，重复on不延长。LinkG服务停止会清理临时授权和本模块启动的SSH。

## 密码配置

在可信研发电脑上执行一次（已有密码文件时不要覆盖）：

    umask 077
    mkdir -p ~/.config/linkg
    if [ ! -e ~/.config/linkg/factory_password ]; then
        python3 -c 'import secrets; print(secrets.token_hex(32))' > ~/.config/linkg/factory_password
    fi

在设备仍可通过SSH登录时，将密码文件安全传入设备：

    ssh root@192.168.1.1 'umask 077; mkdir -p /etc/linkg && chmod 700 /etc/linkg && cat > /etc/linkg/factory_password && chmod 600 /etc/linkg/factory_password' < ~/.config/linkg/factory_password

也可在生产阶段通过可信烧录流程写入该文件。需验证目标系统的/etc持久化策略。不要将密码放入公开发布包、用户配置导出或Git。升级前先确认密码已配置，保留串口控制台等恢复手段。

## 验证和限制

默认编译配置关闭SSH。维护状态仅保存在内存。SSH启停复用/etc/init.d/dropbear，进程状态通过procfs确认，与LQ10的pidof语义相近；该状态不等价于端口可达性。关闭失败时重试并报告ERROR。现有系统root密码和Dropbear认证配置保持不变。

固定密码是明文UDP凭据，客户端绑定不是加密认证，不能防止抓包、重放或来源伪造。仅在可信维护网络中使用。正式发布前必须验证设备开机自启动、SSH监听范围、登录认证及停止脚本能否回收已建立会话。超时由AT服务线程管理，进程被SIGKILL时依赖Supervisor重新启动LinkG并执行默认关闭策略；此版本不包含独立的SSH守护进程或Supervisor维护协议。不得将其视为已完成独立故障安全验证的远程维护系统。

# RG255 Query 解析层测试报告

## 1. 结论

当前 **rg255_query 不建议定版**。

正常驻网和数据连接场景已经通过真实调用链验证，但仍有 3 个解析边界问题：

1. AT+QNETDEVCTL? 的实机断开态返回 +QNETDEVCTL: 0,0,0,0，当前解析器因 CID 为 0 返回 -EBADMSG，无法输出“已断开”状态。
2. AT+CGPADDR=1 按手册允许在无可用地址时省略 PDP_addr，当前解析器要求至少两个字段，因此把 +CGPADDR: 1 判为 -EBADMSG。
3. AT+CEREG? / AT+C5GREG? 的 n 按手册只允许 0、1、2，当前解析器解析后未校验，+CEREG: 9,1 会被当成已注册。

除上述问题外，两组 Fixture 共 75 项通过；开发板当前正常链路、LTE、NR5G-SA、AUTO、网卡 IPv4/IPv6 参数、PDP 双栈、URC 隔离和恢复流程均通过。

## 2. 测试依据

- 模组：RG255AA
- 开发板：172.28.2.1
- AT 串口：/dev/ttyUSB1，115200 8N1，无硬件流控，独占打开
- 项目：/home/dawn/project/linkg_v3
- 项目基线：main，测试时 HEAD 为 fe29bf8
- 被测文件：
  - modules/cellular/rg255/rg255_cmd.h
  - modules/cellular/rg255/rg255_cmd.c
  - modules/cellular/rg255/rg255_query.h
  - modules/cellular/rg255/rg255_query.c
- 手册：Quectel_5G(A)系列_RTOS_AT命令手册_V1.0.0_Preliminary_20260728.pdf

采用的手册章节：

| 功能 | 手册章节 | 关键约束 |
|---|---|---|
| USB 接口协议 | 4.3.4 | usbnet: 1=ECM、2=MBIM、3=RNDIS |
| 网卡工作模式 | 4.3.6 | nat: 0=路由、1=网卡 |
| 网卡网络参数 | 4.3.7 | netmaskset opt=2 返回 IPv4/掩码/网关；opt=3 返回 IPv6 前缀/网关/DNS |
| 网络搜索模式 | 4.4.4 | AUTO、LTE、NR5G-SA、LTE:NR5G-SA、NR5G-SA:LTE |
| EPS 注册 | 6.4 | n 为 0..2，stat 为 0..5 |
| 5GS 注册 | 6.5 | n 为 0..2，stat 为 0..5 |
| 服务小区 | 6.13 | LTE 与 NR5G-SA 使用不同字段布局；SEARCH/LIMSRV/NOCONN/CONNECT |
| PDP 配置 | 7.2 | CID 1..11；IP/IPV6/IPV4V6；APN 最大 99 字节 |
| PDP 激活 | 7.5 | state 0/1；查询可能返回多行 CID |
| PDP 地址 | 7.7 | 地址可省略；FE80 IPv6 不是可路由全局地址 |
| USB 网卡连接 | 7.12 | 查询字段为 type、cid、URC_en、state |

## 3. 测试文件

测试代码只放在 test/cellular/：

- rg255_query_network_card_fixture_test.c
- rg255_query_fixture_test.c
- rg255_query_test.c
- Makefile
- RG255_QUERY_TEST_REPORT.md

未修改业务源码。

## 4. Fixture 测试

执行：

~~~sh
cd /home/dawn/project/linkg_v3/test/cellular
make fixture-run
~~~

结果：

RG255 NETWORK CARD FIXTURE TEST: 7 PASS / 0 FAIL
~~~text
RG255 QUERY FIXTURE TEST: 68 PASS / 2 FAIL
~~~

覆盖范围：

- CPIN READY、PIN、PUK、NOT INSERTED、NOT READY、数字和文本 CME 错误
- mode_pref 的 AUTO、LTE、NR5G-SA、NR5G-SA:LTE、LTE:NR5G-SA 和非法值
- CEREG/C5GREG stat 0..5、AUTO 合并、按当前 RAT 选择注册域、单域失败
- QENG LTE/NR5G-SA 精确字段位置
- QENG SEARCH、LIMSRV、NOCONN、CONNECT、短横线无效指标、非法状态和不支持 RAT
- USBNET ECM/MBIM/RNDIS
- netmaskset IPv4 地址/掩码/网关、IPv6 前缀归一化/网关和非法字段
- 网卡 ROUTER/NIC 模式
- CGDCONT、CGACT 多行响应及 CID 1 选择
- CGPADDR IPv4/IPv6、零地址、FE80 地址、缺失地址和非法地址
- QNETDEV type/cid/URC/state 全字段及非法值
- 非目标 URC 混入 QENG 响应时的隔离

### 4.1 Fixture 失败 1：注册 n 未校验

输入：

~~~text
+CEREG: 9,1
~~~

手册定义 n 只能是 0、1、2。测试期望 -EBADMSG，实际解析成功并输出 REGISTERED。

影响：异常或不兼容响应会被当作有效注册状态。当前实机未复现非法 n，属于健壮性缺口。

建议：在 _rg255_query_parse_registration_response() 中校验 n 的范围。

### 4.2 Fixture 失败 2：CGPADDR 省略地址

输入：

~~~text
+CGPADDR: 1
~~~

手册 7.7 明确说明无可用地址时省略 PDP_addr。测试期望成功且 IPv4/全局 IPv6 均为无效，实际返回 -EBADMSG。

影响：PDP 切换、刚激活或地址尚未分配时，合法的“暂无地址”会被上层当成解析错误。

建议：允许只有 CID 的合法响应，并返回两个地址有效位均为 false。

## 5. 开发板真实调用链测试

真实链路：

~~~text
linkg_uart
  -> at_channel
  -> rg255_cmd
  -> rg255_query
  -> enum / bool / struct
~~~

构建：

~~~sh
cd /home/dawn/project/linkg_v3/test/cellular
make arm
~~~

板端执行：

~~~sh
/root/rg255_query_test --full
~~~

总结果：

~~~text
RG255 QUERY INTEGRATION TEST: 31 PASS / 1 FAIL
RG255 QUERY LATEST BASELINE: 15 PASS / 0 FAIL
~~~

| 场景 | 结果 | 观测 |
|---|---|---|
| 当前 NR5G-SA 基线 | 通过 | n1；SIM READY；注册成功；IPV4V6；QNETDEV 3,1,1,1 |
| netmaskset IPv4/IPv6 | 通过 | IPv4 地址/掩码/网关及 IPv6 前缀/网关解析正确 |
| LTE | 通过 | 第 2 次轮询完成驻网、PDP 双栈和网卡恢复 |
| NR5G-SA | 通过 | 第 3 次轮询完成驻网、PDP 双栈和网卡恢复 |
| AUTO | 通过 | 查询归一化为 AUTO；实接 NR5G-SA；第 3 次轮询完成 |
| QNETDEV 断开 | 失败 | 实机原始响应为 +QNETDEVCTL: 0,0,0,0，解析返回 -74 |
| QNETDEV 自动恢复 | 通过 | 恢复为 type=3、cid=1、URC=1、connected=1 |
| URC 隔离 | 通过 | 捕获 10 条 +QNETDEVSTATUS，并发查询未被污染 |
| 原网络模式恢复 | 通过 | 恢复为 NR5G-SA |
| 恢复后完整基线 | 通过 | SIM、模式、驻网、QENG、网卡参数、PDP、双栈、QNETDEV、IMSI 全通过 |

### 5.1 实机失败：QNETDEV 断开态 CID 为 0

实机原始响应：

~~~text
+QNETDEVSTATUS: 0
+QNETDEVCTL: 0,0,0,0
~~~

解析结果：

~~~text
ret=-74  # -EBADMSG
~~~

手册 7.12 将 cid 写为 1..11，但 RG255AA 实机在 type=0 的断开态返回 cid=0。当前解析器先校验 CID 1..11，因此无法把这个真实响应转换为：

~~~text
type=RG255_NETDEV_TYPE_DISCONNECT
connected=false
~~~

影响：拨号断开、切换网络和恢复过程会把正常的断开过渡态上报为解析错误，可能干扰状态机。

建议：当 type == RG255_NETDEV_TYPE_DISCONNECT 时接受实机的 cid=0；连接类型 1/3 仍严格要求 CID 1..11。该兼容规则应在源码中注明“实机行为与手册范围不一致”。


### 5.2 netmaskset IPv6 尾字段观测

实机 IPv6 查询的前缀和网关解析正确，两个 DNS 字段不属于当前接口输出。一次响应中，最后一个 DNS 续行通过 URC 回调出现：

~~~text
,"240E:56:4000:8000::69"
~~~

它没有污染当前查询结果，因此不计为 rg255_query 失败；若上层 URC 回调要求每一行必须是完整的 +XXX URC，建议后续单独检查 at_channel 对厂家多行续行的归属规则。
## 6. 实机有效响应摘要

最终恢复后的当前响应解析结果：

~~~text
mode=NR5G-SA
network_type=NR5G-SA
band=1
RSRP=-97 dBm
RSRQ=-12 dB
SINR=5 dB
registration=REGISTERED
usbnet=ECM
network_card_mode=NIC
cid=1
pdp_type=IPV4V6
apn=ctnet
pdp_active=true
modem_ipv4=10.180.78.142
modem_global_ipv6=240e:476:8c5:f8d1::1
network_card_ipv4=10.180.78.142
network_card_netmask=255.0.0.0
network_card_ipv4_gateway=10.0.0.1
network_card_ipv6_prefix=240e:476:8c5:f8d1::/64
network_card_ipv6_gateway=fe80::1234
netdev=3,1,1,1
~~~

## 7. 恢复与环境检查

测试前保存的状态：

~~~text
network mode: NR5G-SA
QNETDEVCTL: 3,1,1,1
PDP: CID 1, IPV4V6, ctnet
~~~

测试后：

- 网络模式恢复为 NR5G-SA。
- QNETDEV 恢复为 3,1,1,1。
- 使用 rg255_dial --no-mode-change 重新同步 PDP 与主机 DHCP。
- usb0 IPv4 恢复为 10.180.78.142/8。
- IPv4 公网 ping：3/3 成功。
- IPv6 公网 ping：3/3 成功。
- /dev/ttyUSB1 无残留占用。
- 无测试进程残留。

## 8. 定版判断

当前正常连接主路径可用，但不满足“所有 Fixture、断开过渡态和手册合法响应全部通过”的定版条件。

建议修复或明确接受上述 3 个问题后，原样重新执行：

~~~sh
make fixture-run
make arm
/root/rg255_query_test --full
~~~

定版门槛应为：

- Fixture 0 FAIL
- 开发板完整测试 0 FAIL
- LTE/NR5G-SA/AUTO 均通过
- QNETDEV 断开和自动恢复均通过
- 恢复后 IPv4/IPv6 公网连通

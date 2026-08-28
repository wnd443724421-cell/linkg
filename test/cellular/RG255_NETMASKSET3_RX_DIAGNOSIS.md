# RG255AA `AT+QCFG="netmaskset",3` RX 分行与 URC 归属诊断

## 1. 结论

本问题由两个连续事实共同造成：

1. **模块真实多行响应**：RG255AA 在 DNS1 与 DNS2 之间真实发送了 `0D 0A`。
2. **URC 分类错误**：`at_channel` 正确按该 `LF` 形成第二条物理行，但当前事务只接受 `+QCFG:` 前缀，且 `accept_plain_text=false`，所以把合法的 DNS2 command-response continuation 错分到 URC callback。

已排除：

- UART 分包被误当换行；
- line buffer 长度不足造成截断；
- response buffer 长度不足造成截断；
- TX 命令内部自带 DNS 间换行。

当前不修改业务代码，也不采用“逗号开头就拼接上一行”等未经泛化验证的规则。

## 2. 测试方法

- 设备：`/dev/ttyUSB1`，115200 8N1，无硬件流控，独占打开。
- 查询：`AT+QCFG="netmaskset",3`，只读，不改变模块配置。
- TX：22 字节命令正文 + 2 字节终止符 `0D 0A`，总计 24 字节；命令正文内部没有 CR/LF。
- 旁路探针：`test/cellular/rg255_at_rx_probe.c`。
- 探针直接 `read()` 串口，记录每次 read 的长度、原始十六进制和全局偏移；随后逐字节复刻当前 `_at_channel_feed_rx()` 规则：忽略 `CR/NUL`，只在收到 `LF` 时完成一行。
- 分类参数复刻真实命令配置：`transaction=WAITING`、`expected_prefix="+QCFG:"`、`accept_plain_text=false`。
- 重复旁路抓取 3 次，并运行现有 `/root/rg255_query_test --query-only` 真实调用链交叉验证。

## 3. 原始 UART RX 与 read() 边界

三次旁路抓取均得到相同的 read 长度和字节布局：

| read | 长度 | 全局偏移 | 内容摘要 |
|---|---:|---:|---|
| 1 | 23 B | 0..22 | 命令 Echo，末尾为 `0D` |
| 2 | 118 B | 23..140 | Echo 行结束、完整 `+QCFG:` 行、DNS2 续行及其 CR/LF |
| 3 | 6 B | 141..146 | 空行和 `OK` |

关键的第二次 read 原始字节：

```text
0067 : 34 30 30 30 3A 3A 32 31 38 22 0D 0A 2C 22 32 34
       4  0  0  0  :  :  2  1  8  "  CR LF ,  "  2  4

0077 : 30 45 3A 35 36 3A 34 30 30 30 3A 38 30 30 30 3A
0087 : 3A 36 39 22 0D 0A
```

精确全局偏移：

- `0x70`：DNS1 结束引号 `22`；
- `0x71 0x72`：模块 RX 中的 `0D 0A`；
- `0x73 0x74`：DNS2 续行起始 `2C 22`，即 `,"`；
- `0x8B 0x8C`：DNS2 行末 `0D 0A`。

转义后的完整相关片段：

```text
+QCFG: "netmaskset",3,"dongle","240E:476:8C5:F8D1::/64","FE80::1234","240E:56:4000::218"<CR><LF>
,"240E:56:4000:8000::69"<CR><LF>
```

DNS1、分隔 `CR/LF` 和 DNS2 开头全部位于同一个 118 B 的 `read()` 中。因此该换行不是 UART read 返回边界制造的。

同时，Echo 本身跨越了 read #1/read #2：read #1 在 `CR` 后结束，read #2 才收到后续 `CR LF`；复刻的 line assembler 仍然只形成一条 Echo 行。这直接证明 line 状态会跨 read 保留，read 返回不会自动完成一行。

## 4. line assembly 结果

按当前 `_at_channel_feed_rx()` 规则得到：

| 行号 | 长度 | 完整行 |
|---:|---:|---|
| 1 | 22 | `AT+QCFG="netmaskset",3` |
| 2 | 88 | `+QCFG: "netmaskset",3,"dongle","240E:476:8C5:F8D1::/64","FE80::1234","240E:56:4000::218"` |
| 3 | 24 | `,"240E:56:4000:8000::69"` |
| 4 | 2 | `OK` |

这里第 2、3 行是模块真实发送的两条物理响应行，不是 assembler 人为切断。

## 5. 当前事务参数与最终路由

真实 `rg255_cmd_query_network_card_ipv6()` 调用：

```text
command           = AT+QCFG="netmaskset",3
expected_prefix   = +QCFG:
accept_plain_text = false
transaction       = WAITING
```

按当前 `_at_channel_process_line()` 路由顺序：

| 完整行 | 判定 | 最终去向 |
|---|---|---|
| 命令 Echo | 等于 `current_command` | 忽略 |
| `+QCFG: ... DNS1` | 匹配 `+QCFG:` | command response |
| `,"...DNS2"` | 不匹配 `+QCFG:`，plain text 未启用，也不是 terminal | URC callback |
| `OK` | success terminal | 完成事务 |

真实调用链再次复现：

```text
[URC] ,"240E:56:4000:8000::69"
card_ipv6_prefix=240e:476:8c5:f8d1::/64 gateway=fe80::1234 ret=0
[PASS] NETWORK_CARD_IPV6
URC_COUNT=1 QNETDEVSTATUS_URC_COUNT=0
RG255 QUERY INTEGRATION TEST: 15 PASS / 0 FAIL
```

所以 IPv6 前缀和网关解析成功只说明当前解析器所需字段都在第 2 行；它不改变第 3 行被错误归属的事实。

## 6. 缓冲区核对

当前业务实现常量：

| 缓冲区 | 大小 | 本次最大使用 | 是否截断/溢出 |
|---|---:|---:|---|
| UART 单次 read buffer | 512 B | 118 B | 否 |
| line buffer | 512 B，可保存 511 B + NUL | 88 B | 否 |
| transaction response buffer | 4096 B | 88 B（当前实际归入） | 否 |

如果把 24 B 续行和一个行分隔符也计入 command response，总长仅 113 B，仍远低于 4096 B。

三次探针均报告：

```text
line_overflow=false response_overflow=false
```

真实测试日志也没有出现 `AT_CHANNEL: response line too long, action=discard`。因此不是 line buffer 截断。

## 7. 对四类候选原因的最终判定

| 候选原因 | 判定 | 证据 |
|---|---|---|
| 模块真实多行响应 | **是** | DNS1/DNS2 间原始 RX 明确为 `22 0D 0A 2C 22`，三次一致 |
| UART 分包被误当换行 | **否** | DNS1、`0D 0A`、DNS2 位于同一次 read；代码只在字节值 `LF` 时完成行 |
| line buffer 截断 | **否** | 最大行 88 B，容量 511 B；无 overflow/discard |
| URC 分类错误 | **是** | 续行是合法 command response 的一部分，但不带 `+QCFG:`，当前 prefix-only 路由落入 URC |

综合结论：**物理层面是“模块真实多行响应”，软件问题发生在 AT channel 的 command-response continuation 归属能力，而不是 UART 读包或 line assembly。**

## 8. 后续修复约束（本轮不实现）

修复前应先定义通用的事务归属语义，再补测试，不能仅凭首字符是逗号就拼接：

- 验证 RG255AA 是否还有其他无重复前缀的 command response continuation；
- 验证 continuation 与真实异步 URC 交错时的归属规则；
- 为 transaction 增加显式、命令级 continuation 策略或判定器；
- 保留 line assembly 只由真实 CR/LF 驱动的现有原则；
- 增加“read 在任意字节处分包”的 fixture，证明分类与 read 边界无关。

在这些语义明确前，不建议开启全局 `accept_plain_text`，也不建议采用“逗号开头即 continuation”的全局规则。

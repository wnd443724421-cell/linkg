# Cellular V1 Runtime Design

## Owner循环

```text
consume Monitor events
    -> map event to Status Refresh Mask
    -> cellular_status_process()
    -> cellular_status_get_info()
    -> enforce SIM / Registration global prerequisites
    -> run one current-state step
    -> DONE: enter next state
    -> WAIT: poll until Monitor / Status / Runtime deadline
    -> FAILED: Owner invokes state failure policy
    -> FATAL: return from linkg_cellular_run()
```

Monitor、Status、Runtime 均不创建蜂窝控制线程。`network-cell` 是唯一控制面 Owner。

## 事件映射

| URC | Monitor Event | Status Refresh |
|---|---|---|
| `+QSIMSTAT` | `SIM_PRESENCE_CHANGED` | `SIM` |
| `+CPIN` | `SIM_STATE_CHANGED` | `SIM` |
| `+CEREG` / `+C5GREG` | `REGISTRATION_CHANGED` | `REGISTRATION + RADIO` |
| `+QCSQ` | `RADIO_CHANGED` | `RADIO`，由 QENG 获取 Truth |
| `+CGEV` | `PDP_CHANGED` | `PDP + PDP_ADDRESS` |
| `+QNETDEVSTATUS` | `NETDEV_CHANGED` | `NETDEV + EXPECTED_NETWORK + HOST` |
| `+CFUN` | `MODEM_FUNCTION_CHANGED` | V1按不可恢复运行事件结束Owner |
| `POWERED DOWN` | `MODEM_POWERED_DOWN` | V1按不可恢复运行事件结束Owner |

## 状态执行协议

```text
WAIT    当前状态仍在正常收敛
DONE    当前状态完成，Owner推进
FAILED  当前层级未通过，Owner调用对应失败策略
FATAL   AT/软件基础设施不可继续，Owner退出
```

## PIN安全

- `PIN_REQUIRED` 且未配置PIN：进入 `WAIT_PIN`，不发送 `AT+CPIN`。
- 配置PIN时，每个物理SIM插卡会话最多自动发送一次。
- `AT+CPIN` 返回失败或超时后，只重新查询 `CPIN?`，不自动重发。
- `PUK_REQUIRED` 进入 `WAIT_PUK`，绝不自动输入配置PIN。
- 只有SIM拔出并重新插入才建立新会话并清除 `pin_attempted`。

## Action / Truth

```text
AT+CGACT=1,1
    -> REFRESH_PDP
    -> AT+CGACT?

AT+QNETDEVCTL=1,1,1
    -> REFRESH_NETDEV
    -> AT+QNETDEVCTL?
```

动作返回超时不直接等价于动作失败，最终以 Status Query Truth 为准。

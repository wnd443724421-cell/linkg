# 10. M09：管理 API、asdkctl 与 Portal 适配

## 10.1 模块边界

M09 通过 `IControlService` 访问控制模型；Portal 后端还可调用 M01 定义的受限 `IRecoveryRequester` 提交对端 daemon 恢复请求。M09 不定义恢复协议，不直接调用 UnitManager、HostChannel、StateStore、Fast DDS、SHM、systemd D-Bus 或 `systemctl`。

```cpp
class IControlService {
public:
    virtual ~IControlService() = default;

    virtual std::shared_ptr<const StatusSnapshot> status() const = 0;
    virtual Result<OperationAccepted> submit(const ControlCommand&) = 0;
    virtual Result<OperationView> operation(const OperationId&) const = 0;
    virtual Result<ValidationReport> validate_config(ByteView) const = 0;
};
```

Portal 调用 `request_peer_restart(PeerRecoveryRequest)` 时不指定 target；恢复代理基于 `SO_PEERCRED` 和 cgroup 身份映射唯一对端。asdkd 的 Portal Health Monitor 使用同一 M01 契约，具体恢复代理实现属于 M03。

### 组件

```text
ControlServiceFacade
├── HttpListener
├── UnixHttpListener
├── RequestParser
├── PeerAuthenticator
├── Router
├── ResponseEncoder
├── RateLimiter
└── AuditLogger
```

## 10.2 API 端点

### 查询

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/healthz` | 只表示 `asdkd` 进程和 API 线程存活 |
| GET | `/readyz` | 表示 daemon 已完成配置加载和接管，可接受控制命令 |
| GET | `/v3/status` | 服务总体状态和配置代际 |
| GET | `/v3/plugins` | 插件列表和三维状态 |
| GET | `/v3/plugins/{id}` | 单插件详情、依赖、错误链和资源摘要 |
| GET | `/v3/hosts` | 宿主 unit、连接、PID、boot ID 和资源状态 |
| GET | `/v3/operations/{id}` | operation 进度和结果 |
| GET | `/v3/config` | 当前 active 配置摘要和 hash |

### 控制

| 方法 | 路径 | desired state / 行为 |
|---|---|---|
| POST | `/v3/plugins/{id}/start` | desired=`RUNNING` |
| POST | `/v3/plugins/{id}/stop` | desired=`STOPPED` |
| POST | `/v3/plugins/{id}/restart` | stop 后 start，单一复合 operation |
| POST | `/v3/plugins/{id}/unload` | desired=`ABSENT` |
| POST | `/v3/hosts/{id}/recover` | 清除 quarantine 并发起宿主恢复 |
| POST | `/v3/config/validate` | 只编译 candidate，不产生副作用 |
| PUT | `/v3/config` | 发起全量配置事务 |
| POST | `/v3/system/shutdown` | 按逆 DAG 停止全部插件和宿主 |

## 10.3 请求和响应

### 写操作请求

```json
{
  "request_id": "6f6a3320-62a4-4ca4-bccd-b8430c8e5421",
  "expected_state_generation": 42,
  "reason": "operator request"
}
```

### 接受响应

```json
{
  "request_id": "6f6a3320-62a4-4ca4-bccd-b8430c8e5421",
  "operation_id": "19bbd749-af83-45da-a5a7-f14f88c32220",
  "plugin_id": "camera",
  "accepted_generation": 42,
  "status": "ACCEPTED"
}
```

### 错误响应

```json
{
  "request_id": "...",
  "status": "ERROR",
  "error": {
    "code": "STALE_GENERATION",
    "module": "control_service",
    "plugin_id": "camera",
    "phase": "request_validation",
    "message": "expected generation 41, actual generation 42"
  }
}
```

### HTTP 状态码

| HTTP | ASDK 语义 |
|---:|---|
| 200 | 查询或同步校验成功 |
| 202 | 异步 operation 已持久化并接受 |
| 400 | 格式错误或缺少字段 |
| 403 | 身份或权限不足 |
| 404 | 资源不存在 |
| 409 | active operation 冲突或 host 冲突 |
| 412 | generation 前置条件失败 |
| 422 | 配置语义无效 |
| 429 | 限流或控制队列满 |
| 503 | daemon 未 ready、正在配置事务或控制面不可用 |



## 10.5 认证与权限

### UDS

- 默认路径 `/run/asdk/control.sock`；
- 通过 `SO_PEERCRED` 获取 UID/GID/PID；
- 只读和写操作可按 Unix group 区分；
- CLI 默认优先 UDS；
- UDS peer 身份直接进入审计日志。

### HTTP

- 默认只监听 `127.0.0.1`；
- Portal 使用独立 service credential；
- 不将浏览器用户字段直接当作 Linux 系统身份；
- 非 loopback 监听必须显式配置 TLS/mTLS；
- 默认禁止 CORS；
- request body、JSON 深度、数组元素和字符串长度均有限制。

## 10.6 asdkctl

```text
asdkctl status [--json]
asdkctl plugin list [--json]
asdkctl plugin show <id> [--json]
asdkctl plugin start <id> [--expected-generation N]
asdkctl plugin stop <id> [--expected-generation N]
asdkctl plugin restart <id>
asdkctl plugin unload <id>
asdkctl host list [--json]
asdkctl host recover <id>
asdkctl operation show <operation-id> [--watch]
asdkctl config validate <file>
asdkctl config apply <file>
asdkctl system shutdown
```

### 稳定退出码

| 退出码 | 含义 |
|---:|---|
| 0 | 成功 |
| 2 | 命令行参数错误 |
| 3 | daemon 不可达 |
| 4 | 权限不足 |
| 5 | 请求被拒绝（配置、generation、冲突） |
| 6 | operation 最终失败 |
| 7 | 请求超时但 operation 仍可能继续 |
| 8 | 输出/协议解析错误 |

`--json` 模式不得输出颜色、进度动画或额外说明文本。

## 10.7 Portal 适配

Portal 后端实现 `DaemonClient` 与 M01 的 `IRecoveryRequester`：

```text
Portal UI
→ DaemonClient
→ loopback HTTP / UDS proxy
→ asdkd API

Portal Health Monitor
→ 连续 3 次 `/healthz` 失败
→ IRecoveryRequester(request_peer_restart)
→ /run/asdk/recovery.sock
→ asdk-recoveryd
→ systemd RestartUnit(asdkd.service)
```

页面至少区分：

```text
DAEMON_UNAVAILABLE
ADOPTING
STARTING
RUNNING
DEGRADED
FAILED
UPDATING_CONFIG
SHUTTING_DOWN
```

`asdkd` 同时使用本地 Portal health endpoint 监视 Portal 后端；连续 3 次失败后，它经同一恢复代理请求 `RestartUnit(asdk-portal.service)`。双方只报告和请求恢复，不直接操作对方进程。

Portal 不再创建 V2 控制 DDS Topic，不直接查询 Host，不读取 文件状态存储，不执行 `systemctl`；它只能使用受限恢复 socket 请求恢复 `asdkd.service`。

首版状态刷新可使用短轮询；只有在状态量和并发证明需要时再增加 SSE/WebSocket，不作为 ASDK Core 前置条件。

## 10.8 M09 实现任务

- [ ] M09-01：实现 `IControlService` facade 和 immutable snapshot 查询。
- [ ] M09-02：实现 UDS HTTP listener、SO_PEERCRED 和权限映射。
- [ ] M09-03：实现 loopback HTTP、请求限制和审计日志。
- [ ] M09-04：实现所有 v3 查询、operation 和配置端点。
- [ ] M09-05：实现 `asdkctl` 稳定 JSON、退出码和 operation watch。
- [ ] M09-06：实现 Portal `DaemonClient`、Health Monitor 和受限 `IRecoveryRequester`，移除 V2 控制 Topic。
- [ ] M09-07：实现 Portal 侧恢复请求状态展示、审计关联和恢复后的 daemon re-ready 刷新。
- [ ] M09-08：实现 API contract tests 和 OpenAPI/JSON Schema 文档。

## 10.9 M09 验收条件

1. 使用 `FakeControlService` 可独立完成 API、CLI 和 Portal 适配开发。
2. 写操作在持久化接受后返回 202，不阻塞等待生命周期完成。
3. 旧 generation 返回 412，冲突返回 409。
4. daemon 未 ready 时查询可用、写操作明确返回 503。
5. Portal 停止后，asdkctl 和 daemon 原生 API 仍完整可用。
6. Portal 与 asdkctl 均不依赖 Fast DDS 或插件业务消息库。
7. Portal 连续 3 次探测不到 daemon 时只能请求恢复 `asdkd.service`；asdkd 连续 3 次探测不到 Portal 时只能请求恢复 `asdk-portal.service`。
8. 恢复代理必须拒绝伪造身份、越权 target、60 秒内重复请求和熔断期请求。

---

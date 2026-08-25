# LinkG Packet Pool 整理与审计 V1.0.0

基线：2026-08-25 最新 `Linkg_v2.tar(3).gz`。

## 本轮范围

仅整理：

- `include/linkg/core/packet/linkg_packet_pool.h`
- `core/packet/linkg_packet_pool.c`

不修改 Packet Pool 的业务语义、内存布局、引用计数模型或调用接口。

## 书写整理

- 头文件按类型、标志、配置、生命周期、申请、引用、数据访问、业务分类分区。
- 头文件宏、类型、结构体成员统一右侧 `//` 中文注释。
- 头文件函数声明保持单行，并在同一分类中按返回类型列整理。
- C 文件每个函数都有 `/** @brief ... */`。
- 锁、引用和生命周期约束使用 `@note`。
- 函数内部单行说明统一 `//`，多行说明统一 `/** ... */`。
- 清除普通 `/* ... */` 说明注释和原文件中的 Tab 缩进。

## Packet 所有权契约

1. `linkg_packet_pool_alloc()` 成功后，调用方获得 1 个 Packet 引用。
2. `linkg_packet_pool_alloc_batch()` 返回的每个 Packet 都带 1 个调用方引用。
3. 异步模块、Queue 或缓存需要在调用返回后继续保存 Packet 时必须 `linkg_packet_retain()`。
4. 每个有效引用必须对应一次 `linkg_packet_release()`。
5. 引用计数归零后 Packet 立即归还 Pool，不得继续访问。
6. `linkg_packet_pool_deinit()` 前必须停止所有访问 Pool 的线程，并确保所有 Packet 已归还。
7. `reference_count` 支持跨线程引用管理；`data_offset/data_length/flags` 等 Packet 可变元数据不支持无锁并发修改。

## 功能审计结论

当前实现能够完成项目描述的功能，未发现需要阻塞后续重构的确定性逻辑错误。

### 已确认正确

- 固定数量 Packet + 连续数据区的有界内存模型。
- 64 字节槽位起始地址对齐。
- `slot_size` 向 64 字节对齐后的 `slot_stride` 溢出检查。
- Pool free stack 由 mutex 串行保护。
- Packet 引用计数使用 C11 atomic。
- 单个申请、批量申请以及部分申请行为正确。
- 引用未归零时 `deinit` 返回 `-EBUSY`。
- Packet 重新申请时会恢复 headroom、长度和业务标志。
- `push/pull` 在当前数据布局不变量成立时保持 `data_offset + data_length` 边界一致。
- REALTIME / VIDEO 互斥，未设置两者时自然属于 DATA。
- Wi-Fi Queue、Cellular Queue、Transport Reassembly 当前对 retain/release 的使用与 Packet Pool 契约匹配。

### 当前工程实际配置

- Packet 数量：2048
- Slot 大小：2048 Byte
- Headroom：64 Byte
- TUN MTU：1500 Byte
- Transport 最大头部：28 Byte

因此当前单 Packet 可从默认 data pointer 使用 `1984 Byte`，满足 `1500 Byte` TUN MTU 及 Transport 头部需求；数据区约 4 MiB，内存使用有明确上限。

## 已知约束（本轮不改）

- Pool 对象首次 `init` 前必须清零。
- Pool 生命周期必须由管理线程串行控制；不能与 `alloc/release` 并发 `deinit`。
- `retain/release` 的调用正确性属于引用所有权契约；非法 double-release / retain-after-free 在 Debug 下依赖 assert 检测。
- `linkg_packet_data/capacity/headroom` 等高频 inline helper 以“传入有效 Packet”为前提，不额外增加运行时参数检查。
- `linkg_packet_t` / `linkg_packet_pool_t` 当前仍公开结构，因为 Transport、TUN、Link 等现有代码直接访问字段；是否 opaque 等到数据平面边界重构时再讨论。

## 验证结果

- 完整 x86 clean build + `-Werror`：PASS，`[100%] Built target linkg`
- ASan + UBSan Packet Pool 专项测试：PASS
- ThreadSanitizer 8 线程并发申请/retain/release 压力测试：PASS
- 64 Packet 全池申请、唯一性、耗尽、部分 batch、回收完整性：PASS
- `deinit -> -EBUSY -> release -> deinit` 生命周期测试：PASS
- push/pull、headroom/capacity、业务分类互斥和复用 reset：PASS

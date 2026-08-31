rg255_query.c.patch 基于：
wnd443724421-cell/linkg
commit 1aa7ea9d526d3ace09ccebe972cae39cd0d08b70
blob 6e903a0fde6badc8749a9e7f4e18fea933a231ce

应用：
    git apply patches/rg255_query.c.patch

该补丁只修改：
1. registration stat=3 映射为 DENIED；
2. AUTO 合并只有 EPS 和 5GS 都 DENIED 时才输出 DENIED；
3. rg255_query_registration() 移除 network_type 参数；
4. AUTO/UNKNOWN 网络模式直接并行查询 EPS 与 5GS，不再依赖 QENG 的 network_type。

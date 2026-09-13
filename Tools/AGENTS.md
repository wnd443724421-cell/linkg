# 独立工具规则

- 本目录由独立 `Tools/CMakeLists.txt` 以 C99 构建，不与主程序目标直接链接。
- `cmain` 是 Web 服务器 CGI 到 `127.0.0.1:45454` 的 JSON 桥；请求上限 8 KiB，响应累积上限 256 KiB。
- `cmain` 必须完整发送请求，并在收到完整 JSON 对象后立即返回；不要依赖后端关闭连接来判定响应结束。
- `upgrade` 从 CGI 标准输入读取 `CONTENT_LENGTH` 指定的原始上传体并写入临时升级文件；协议变更要同步 `www/view/upgrade.html` 和主程序升级入口。
- 工具向 Web 输出时必须先写 CGI `Content-Type`，错误也保持合法 JSON。
- ARM64 工具链路径由 `Tools/build.sh` 和根工具链文件提供；生成物留在 `Tools/output/`，不纳入代码分析。

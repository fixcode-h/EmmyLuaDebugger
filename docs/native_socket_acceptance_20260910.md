# Native 协议与运行时验收记录

更新：2026-09-10。入口为 `tests/native_socket_harness.cpp`；同一客户端可驱动 TCP 或 Windows named pipe，真实启动 Facade、Transporter 和 Lua VM。

## 已通过的真实命名管道链路

`emmy_native_pipe_harness` 已在 Windows x64/x86 Debug、Lua 5.4.6 source 模式通过，覆盖：

- 未认证 `agent.describe` 返回 `NOT_AUTHORIZED`。
- Init/token → Ready → VM snapshot → 断点完整替换 → 真实 Lua 暂停。
- `VALUE_PATH value.answer` 返回 42；缺少 policy 返回 `EVALUATION_DENIED`，连接继续可用。
- 相同 requestId/内容重放相同结果；不同内容复用 requestId 返回 `REQUEST_ID_REUSE`。
- Continue ACK 与 owner thread 发出的 resumed 允许任意到达顺序；继续后旧 pause 返回 `STALE_PAUSE_REFERENCE`。
- 断线重连保留 agentSessionId、递增 connectionEpoch，重新 Ready/snapshot；旧连接请求返回 `STALE_CONNECTION_EPOCH`。

`emmy_vm_lifecycle_harness` 另覆盖 Host-before-Agent、双 VM 独立 Ready、嵌套 owner callback、coroutine 单步隔离、raw table/字符串边界、不调用元方法、context reset 和暂停中关闭。`emmy_hook_dispatcher_test` 覆盖继承 coroutine 的宿主 hook 在主状态 detach 后继续运行。

这些是 Native 与真实 Lua 自动化证据，不是 IDEA/UE 进程注入验收，也不是 CLI→IDEA→UE 的一体化端到端测试。

## TCP 阻塞：保留失败

最终独立复验：

```powershell
ctest --test-dir build-runtime-20260910 `
  -R '^(emmy_transporter_concurrency_test|emmy_native_socket_harness)$' `
  --output-on-failure --timeout 12 --output-junit tcp-results.xml
```

结果为 **0/2，通过 0，失败 2**。并发测试报 `client connect error: connection timed out`；协议 harness 已 listen ready，但客户端有界重试后仍无法连接。失败文件保存在 `build-runtime-20260910/tcp-results.xml`。

本机对照诊断：.NET TcpListener/TcpClient 回环成功；独立纯 Winsock 程序不链接 Emmy 或 libuv，仍在 bind/listen 成功后 connect 超时、连接停留于 SYN_SENT；.NET 客户端连接 Native 服务端也超时。证据指向本机 Native 进程的网络访问环境限制，具体拦截组件尚未确认。本次没有修改防火墙、安全软件或网络策略。

本地其余测试使用显式 `-E` 排除 TCP；CI **不排除** TCP，仍要求其通过。不得将本地结果描述为“Native 全矩阵通过”。

## 分架构结果与复现

工具链：VS 18 / MSVC 14.51、Ninja、Windows SDK 10.0.26100.0，Lua 5.4 动态 API 或 5.4.6 source。

| 模式 | 配置 | 本地结果 |
| --- | --- | --- |
| 动态 API | x64 Debug / Release | 各 14/14；每配置显式排除 1 项 TCP |
| 动态 API | x86 Debug / Release | 各 14/14；每配置显式排除 1 项 TCP |
| Lua source | x64 Debug / x86 Debug | 各 17/17；每配置显式排除 2 项 TCP |

```powershell
ctest --test-dir <build-directory> `
  -E '^(emmy_transporter_concurrency_test|emmy_native_socket_harness)$' `
  --output-on-failure --no-tests=error --timeout 30 --output-junit final-results.xml
```

x64 动态目录为 `build-ninja4`、`build-release-20260910`；x86 动态目录为 `build-x86-debug-20260910`、`build-x86-release-20260910`；x64 source 为 `build-runtime-20260910`。x86 Debug 曾切换为 source 完成整套测试，其报告单独保存为 `build-x86-debug-20260910/source-results.xml`，随后恢复动态模式。

`protocol_fuzz_cases.jsonl` 的 12 条 fixture 已由测试实际读取并断言：3 条 framing case、9 条 envelope/target case；重复 requestId、epoch、取消等另由 ProtocolSession 测试和真实协议 harness 覆盖。fixture 文件存在本身不算测试通过。

Linux/macOS 的 Debug/Release、远端 CI、实际 EasyHook 注入/重复附加/Detach 后 PIE 周期仍未验证。

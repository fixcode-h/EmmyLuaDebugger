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

## TCP 对照：放行前失败，受限放行后通过

放行前独立复验：

```powershell
ctest --test-dir build-runtime-20260910 `
  -R '^(emmy_transporter_concurrency_test|emmy_native_socket_harness)$' `
  --output-on-failure --timeout 12 --output-junit tcp-results.xml
```

结果为 **0/2，通过 0，失败 2**。并发测试报 `client connect error: connection timed out`；协议 harness 已 listen ready，但客户端有界重试后仍无法连接。失败文件保存在 `build-runtime-20260910/tcp-results.xml`。

本机对照诊断：.NET TcpListener/TcpClient 回环成功；独立纯 Winsock 程序不链接 Emmy 或 libuv，仍在 bind/listen 成功后 connect 超时；.NET 客户端连接 Native 服务端也超时。

放行前已通过只读 WFP 事件确定拦截位置：独立诊断程序的 PID、临时端口、时间与入站 drop 完整对应，随后真实 `emmy_native_socket_harness.exe` 在端口 39547 的连接也命中同一 filter 70739。该 filter 的 provider 为 `FWPM_PROVIDER_MPSSVC_WF`、layer 为 `FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4`、action 为 `FWP_ACTION_BLOCK`，名称为 `Query User`，origin 为 `Query User Default`。这是本次未获得显式放行的连接落入 Windows 防火墙兜底过滤器的证据；截至该阶段尚未修改安全策略。

不能仅根据 `codex_sandbox_offline_*` 规则名称归因：本次 Native 进程 SID 与这些规则限定的离线用户 SID 不同，诊断进程没有 restricted SID。WFP 原始导出包含本机其它程序信息，仅保存在父仓库忽略的 `build/verification-tools`，不提交。

此前本地其余测试使用显式 `-E` 排除 TCP；CI **不排除** TCP，仍要求其通过。不得将本地结果描述为“Native 全矩阵通过”。

17:46 经用户授权，父仓库的 `tools/test-native-tcp-with-firewall.ps1` 仅为 `build-runtime-20260910` 内两个精确测试 exe 临时允许 `127.0.0.1 → 127.0.0.1` 入站 TCP。相同二进制复验 **2/2 通过，0.63 秒**，报告为 `tcp-approved-results.xml`。脚本 finally 已删除本次规则，再次查询规则数为 0；未留下持久放行。该结果关闭了 x64 Lua source 的两项 TCP 阻塞，其他配置仍保留此前明确排除的统计范围。

后续只读诊断确认 PowerShell 与当前 Java 已有 Public 入站 TCP Allow，而 Native harness 无匹配规则；纯 IPv4/.NET 回环成功，未放行的独立 Winsock 程序仍超时。实际拦截为 Windows 防火墙默认应用授权路径；尚未确定本机为何没有通常的桌面回环豁免，不能仅以默认入站 Block 宣称查明最初策略来源。项目继续使用 TCP。

## 分架构结果与复现

工具链：VS 18 / MSVC 14.51、Ninja、Windows SDK 10.0.26100.0，Lua 5.4 动态 API 或 5.4.6 source。

| 模式 | 配置 | 本地结果 |
| --- | --- | --- |
| 动态 API | x64 Debug / Release | 各 14/14；每配置显式排除 1 项 TCP |
| 动态 API | x86 Debug / Release | 各 14/14；每配置显式排除 1 项 TCP |
| Lua source | x64 Debug / x86 Debug | 各 17/17；每配置显式排除 2 项 TCP |

上述报告保留原统计范围；x64 Lua 5.4 source 另有 `tcp-approved-results.xml` 的 2/2 TCP 通过记录，不能将其外推为其他配置的 TCP 结果。

```powershell
ctest --test-dir <build-directory> `
  -E '^(emmy_transporter_concurrency_test|emmy_native_socket_harness)$' `
  --output-on-failure --no-tests=error --timeout 30 --output-junit final-results.xml
```

x64 动态目录为 `build-ninja4`、`build-release-20260910`；x86 动态目录为 `build-x86-debug-20260910`、`build-x86-release-20260910`；x64 source 为 `build-runtime-20260910`，x86 source 使用父仓库独立目录 `build/native-x86-runtime54`。四个动态配置与两个 source 配置均已在本轮源码上重新构建验证，报告为各目录 `final-results.xml`。

`protocol_fuzz_cases.jsonl` 的 12 条 fixture 已由测试实际读取并断言：3 条 framing case、9 条 envelope/target case；重复 requestId、epoch、取消等另由 ProtocolSession 测试和真实协议 harness 覆盖。fixture 文件存在本身不算测试通过。

## 本轮新增版本与平台验证

- Lua 5.1.5 / 5.2.4 / 5.3.5：Windows x64 source 模式各 3/3，通过真实 pipe 协议、hook dispatcher、VM lifecycle。目录为父仓库 `build/native-lua51`、`build/native-lua52`、`build/native-lua53`；报告为各目录 `version-results.xml`。Lua 5.4.6 的相同场景在上述 17 项中覆盖。
- 修复 Lua 5.1 的类型计数常量、Lua 5.2 静态库误用 DLL 导入标记；生命周期 fixture 根据实际版本选择不匹配 ABI。
- Lua 5.1 使用 `lua_getfenv` raw 读取函数环境，普通 `_ENV` 局部变量不再遮蔽函数环境。Lua 5.2+ 保留真正的 `_ENV` 语义。
- Lua 5.2+ 的 nil/非表 `_ENV` 返回 `VALUE_NOT_FOUND`，不会回退默认全局作用域。真实暂停帧回归覆盖将环境改为 nil、查询拒绝和环境恢复。
- 新增 `emmy_dynamic_lua_loader_test <DLL路径> <版本号>`，真实加载独立构建的 Lua 5.1/5.4 DLL，调用生产 `SetupLuaAPI` 并 raw 读取 42；两版本均通过。`lua_getfenv` 为可选符号，不能让没有此 API 的 5.2+ 加载失败。
- 使用官方 Zig 0.14.1 在 Windows 交叉编译 Linux x86_64/glibc 2.17：source 与 dynamic API 两模式全部目标编译和链接通过。目录为父仓库 `build/native-linux-zig`、`build/native-linux-dynamic-zig`。这只证明 Linux 目标可编译，未执行 Linux ELF。
- `native_ide_fixture` 是用于 IDEA 平台集成测试的独立 Host，使用现有 pipe、认证和生命周期 API。stdin 的 `stop`/EOF 会唤醒暂停并关闭 VM；`reset` 在 owner thread 上重建 source epoch；`close-vm` 只关闭 VM，Agent 与 pipe 继续在线，便于区分正常 VM 关闭与传输断线；90 秒 watchdog 防止测试异常后无限等待。已单独验证 ready → close-vm → VM 关闭且进程存活 → stop → exit 0。它不覆盖 EasyHook 注入。

## IDEA 联调补出的协议回归

真实 IDEA 使用 Gson 发送断点时会省略可选字段。Native 原先通过 const JSON 下标读取缺少的 `sourceIdentity.contextGeneration` 以及 contribution 的条件/日志字段，会触发 nlohmann JSON 断言，导致断点替换 ACK 丢失。现已在这些字段访问前检查存在性和类型；只有 revision 而缺少 breakpoints 的请求返回 `INVALID_BREAKPOINT_SNAPSHOT`。

真实 pipe harness 已加入 IDE 风格的稀疏 source/composite contribution，以及非法快照后连接仍可使用的回归。该修复对应 Native `31247a4`；六个 Windows 非 TCP 配置已重新构建并保持 14/14 或 17/17。这是跨语言实际报文验证补出的缺陷，不能用两个语言各自的 DTO 单元测试代替。

Linux/macOS 实际运行、远端 CI、实际 EasyHook 注入/重复附加/Detach 后 PIE 周期仍未验证。Native CI 保留 Windows TCP，并扩展到 Lua 51/52/53/54 source 版本；LuaJIT 目录目前只有说明文件，没有虚构运行证据。

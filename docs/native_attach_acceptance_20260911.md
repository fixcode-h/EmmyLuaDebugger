# 真实 Lua Attach 验收记录

更新：2026-09-11。

## 验收目标

`tests/native_attach_lua_fixture.cpp` 启动一个只链接共享 `lua54.dll` 的 Lua 5.4 进程。进程启动后不加载 Emmy，由生产 `emmy_tool attach` 注入 `emmy_hook.dll`；IDEA 侧使用生产 `EmmyAttachDebugProcess`，CLI 只通过已发布的 Gateway 访问调试目标。

## Fixture 构建

```powershell
cmake -S . -B build-attach-fixture -G Ninja `
  -DEMMY_USE_LUA_SOURCE=ON -DEMMY_VM_REGISTRY_TEST=ON `
  -DEMMY_LUA_VERSION=54 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-attach-fixture --target emmy_attach_lua_fixture --parallel 4
```

产物目录应同时包含 `emmy_attach_lua_fixture.exe` 和 `lua54.dll`。宿主支持 `reset`、`close-vm`、`stop` 三条 stdin 命令，并分别用于验证上下文代次、独立 VM 关闭和进程优雅退出。

## 证据分级

| 层级 | 证明内容 | 当前记录 |
| --- | --- | --- |
| 静态 | Fixture 不链接 Emmy，CMake 依赖共享 Lua DLL | 已检查构建目标和 DLL 部署 |
| Native | Lua ABI、hook、VM lifecycle、v2 snapshot、断点和受限求值 | 由 `ctest` 和独立 attach 命令记录 |
| IDEA 集成 | 生产 Attach bootstrap、v2 VM 注册、CLI lease/变量/Probe | 由 `EmmyNativeAttachIntegrationTest` 记录 |
| 真实运行时 | Windows EasyHook 注入、TCP 连接、目标线程暂停/恢复 | 必须在未阻断 loopback 的 Windows 环境执行 |

## 复现命令

```powershell
$env:EMMY_ATTACH_FIXTURE_EXE = (Resolve-Path build-attach-fixture/emmy_debugger/emmy_attach_lua_fixture.exe).Path
./gradlew.bat --no-daemon :test --tests '*EmmyNativeAttachIntegrationTest'
```

测试覆盖：v2 snapshot、断点 ACK 和 line 3 暂停；`value.answer`、特殊 key 和嵌套 table 展开；Probe 条件命中、采集值、自动继续和后端断点清理；旧 pause/frame/变量引用失效；reset 后新 VM 代次；`close-vm` 后 Agent 仍 ready 而 VM 不再 ready；最后 `stop` 和 `closed`。

## 环境边界

Attach Agent 的调试端口由目标 PID 派生并监听 `127.0.0.1`/`::1`。Windows 防火墙或 Codex 沙箱若阻断 loopback，可能出现“注入成功、监听成功、IDEA 连接超时”；这不能归因于 DLL 注入失败，也不能把该环境下的 IDEA 集成测试标记为通过。应保留 `emmy_tool` 的结构化 attach 状态、目标进程监听信息和 IDEA 日志，并在允许 loopback 的 Windows CI/开发机复验。


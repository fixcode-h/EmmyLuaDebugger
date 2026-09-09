# Native 验证矩阵

## 纯 C++ harness

这些测试不启动 UE、不加载 EasyHook，适合在 CI 或开发机上验证跨模块契约：

| 测试 | 覆盖 |
| --- | --- |
| `emmy_hook_manager_test` | disable 后拒绝新回调、in-flight callback quiescence、句柄释放与 hook chain 替换保护 |
| `emmy_vm_registry_test` | VM teardown 生命周期、地址复用代次、混合 Lua ABI 与公共/私有 ABI 兼容性 |
| `emmy_native_contract_test` | teardown deadline、ABI major/minor/private mismatch、HostValue provider 缺失、过期 deadline、game-thread dispatch 前 deadline 重检 |

## Debug 验证命令（Windows）

```powershell
cmd /c "call C:\PROGRA~1\MICROS~1\18\Community\VC\Auxiliary\Build\vcvars64.bat >nul && cmake -S . -B build-check -DEMMY_VM_REGISTRY_TEST=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --config Debug"
ctest --test-dir build-check -C Debug --output-on-failure
```

`EMMY_VM_REGISTRY_TEST=ON` 才会注册 native harness；未初始化 MSVC `INCLUDE`/`LIB` 环境时，`cl.exe` 会连 `<atomic>` 等标准头都找不到，应先加载 `vcvars64.bat`。

## 证据边界

- CTest 通过只证明 C++ 生命周期、ABI 数据契约和 HostValueProvider deadline 行为。
- 真实 EasyHook 注入、UE/UnLua private layout、Lua owner thread/game thread 调度和 IDE attach 必须在目标运行时单独验收。
- `HostValueProvider` fixture 使用短 deadline 和 sleep 模拟超时，不代表 UE 调度器的实际排队延迟。

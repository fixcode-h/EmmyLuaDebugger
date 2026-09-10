#include "emmy_debugger/emmy_facade.h"
#include "emmy_debugger/debugger/emmy_debugger_lib.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>

namespace {
void Require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "native IDE fixture: " << message << std::endl;
        std::exit(1);
    }
}

struct Control {
    std::atomic<bool> stop{false};
    std::atomic<bool> reset{false};
    std::atomic<bool> closeVm{false};
    std::mutex mutex;
    std::condition_variable completed;
    bool done = false;
};
}

// 独立测试宿主：stdin 接受 reset/close-vm/stop，EOF 也会关闭；Lua 仅在主线程执行。
// 生产插件通过现有 pipe 协议连接，不提供测试专用调试协议或授权后门。
int main(int argc, char** argv) {
    std::string pipeName, sourcePath, sourceHash;
    for (int i = 1; i < argc; i += 2) {
        Require(i + 1 < argc, "missing option value");
        const std::string option = argv[i];
        if (option == "--pipe") pipeName = argv[i + 1];
        else if (option == "--source") sourcePath = argv[i + 1];
        else if (option == "--source-hash") sourceHash = argv[i + 1];
        else Require(false, "unknown option");
    }
    Require(!pipeName.empty() && !sourcePath.empty() && sourceHash.size() == 64,
        "--pipe, --source and --source-hash are required");
    const char* token = std::getenv("EMMY_IDE_FIXTURE_TOKEN");
    Require(token && *token, "EMMY_IDE_FIXTURE_TOKEN is required");
    std::ifstream input(sourcePath, std::ios::binary);
    Require(input.good(), "source opens");
    const std::string script((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::string chunkName = "@" + sourcePath;
    lua_State* state = luaL_newstate();
    Require(state != nullptr, "Lua state created");
    luaL_openlibs(state);
    auto& facade = EmmyFacade::Get();
    facade.SetExpectedAuthToken(token);
    std::string error;
    Require(facade.PipeListen(state, pipeName, error), "pipe listens: " + error);
    EmmyHostVmMetadata metadata{};
    metadata.size = sizeof(metadata);
    metadata.version = EMMY_HOST_API_VERSION;
    metadata.displayName = "CLI integration Lua VM";
    metadata.engineName = "TEST_HOST";
    metadata.engineContext = "native-ide-fixture";
    const auto vmId = Emmy_RegisterLuaVm(state, &metadata);
    Require(vmId != 0 && Emmy_NotifyLuaVmReady(vmId), "Host VM is ready");
    const auto registerSource = [&] {
        const auto vm = facade.GetHostVmRegistry().Find(vmId);
        Require(vm != nullptr, "Host VM registration exists");
        EmmyLuaSourceIdentity source{};
        source.size = sizeof(source);
        source.version = EMMY_HOST_API_VERSION;
        source.sourceEpoch = vm->sourceEpoch;
        source.chunkName = chunkName.c_str();
        source.canonicalPath = sourcePath.c_str();
        source.sha256 = sourceHash.c_str();
        Require(Emmy_RegisterLuaSource(vmId, &source) != 0, "Host source registered");
    };
    registerSource();
    const auto control = std::make_shared<Control>();
    // stdin 线程只改变元数据和唤醒暂停，不读取或销毁 Lua。
    std::thread([control, vmId] {
        std::string command;
        while (std::getline(std::cin, command)) {
            // PowerShell/.NET 的 UTF-8 writer 可能在第一条命令前发送 BOM。
            if (command.compare(0, 3, "\xEF\xBB\xBF") == 0) command.erase(0, 3);
            if (!command.empty() && command.back() == '\r') command.pop_back();
            if (command == "reset") control->reset.store(true);
            else if (command == "close-vm") {
                control->closeVm.store(true);
                // 这里只关闭请求入口并唤醒暂停；lua_close 仍由 owner 主线程调用。
                Emmy_BeginLuaVmClose(vmId, "fixture-close-vm");
            }
            else if (command == "stop") break;
        }
        control->stop.store(true);
        Emmy_BeginLuaVmClose(vmId, "fixture-stop");
    }).detach();
    std::thread watchdog([control, vmId] {
        std::unique_lock<std::mutex> lock(control->mutex);
        if (!control->completed.wait_for(lock, std::chrono::seconds(90), [&] { return control->done; })) {
            control->stop.store(true);
            Emmy_BeginLuaVmClose(vmId, "fixture-watchdog");
        }
    });
    const auto closeVm = [&] {
        if (state == nullptr) return;
        Require(Emmy_BeginLuaVmClose(vmId, "fixture-owner-close") != 0, "VM close begins");
        lua_close(state);
        state = nullptr;
        Require(Emmy_EndLuaVmClose(vmId) && Emmy_ReleaseLuaVmRegistration(vmId), "VM close completes");
        std::cout << "{\"vmClosed\":true}" << std::endl;
    };
    std::cout << "{\"ready\":true,\"vmId\":\"" << vmId << "\"}" << std::endl;
    while (!control->stop.load()) {
        if (control->closeVm.exchange(false)) closeVm();
        if (state != nullptr && control->reset.exchange(false)) {
            Require(Emmy_ResetLuaVmContext(vmId, "fixture-reset") != 0, "context reset");
            registerSource();
        }
        if (state != nullptr) {
            Require(luaL_loadbuffer(state, script.data(), script.size(), chunkName.c_str()) == 0,
                "Lua source compiles");
            Require(lua_pcall(state, 0, 0, 0) == 0, "Lua script completes");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    closeVm();
    facade.Destroy();
    {
        std::lock_guard<std::mutex> lock(control->mutex);
        control->done = true;
    }
    control->completed.notify_all();
    watchdog.join();
    std::cout << "{\"closed\":true}" << std::endl;
    return 0;
}

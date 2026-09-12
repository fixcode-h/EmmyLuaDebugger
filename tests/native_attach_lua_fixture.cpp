extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "native attach Lua fixture: " << message << std::endl;
        std::exit(1);
    }
}

struct Control {
    std::atomic<bool> stop{false};
    std::atomic<bool> reset{false};
    std::atomic<bool> closeVm{false};
};

int ProcessId() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return 0;
#endif
}

lua_State* CreateState() {
    lua_State* state = luaL_newstate();
    Require(state != nullptr, "Lua state creation failed");
    luaL_openlibs(state);
    return state;
}

void RunScript(lua_State* state, const std::string& script, const std::string& chunkName) {
    const int loadResult = luaL_loadbuffer(state, script.data(), script.size(), chunkName.c_str());
    Require(loadResult == LUA_OK, "Lua source compilation failed");
    const int callResult = lua_pcall(state, 0, LUA_MULTRET, 0);
    if (callResult != LUA_OK) {
        const char* error = lua_tostring(state, -1);
        std::string message = error == nullptr ? "unknown Lua error" : error;
        lua_settop(state, 0);
        Require(false, "Lua script execution failed: " + message);
    }
    lua_settop(state, 0);
}

} // namespace

// This process deliberately links only the shared Lua runtime. Emmy is loaded
// later by emmy_tool attach, so symbol discovery, hook installation, VM
// fallback registration, and lifecycle reporting all use the production path.
int main(int argc, char** argv) {
    std::string sourcePath;
    for (int index = 1; index < argc; index += 2) {
        Require(index + 1 < argc, "missing option value");
        const std::string option = argv[index];
        if (option == "--source") sourcePath = argv[index + 1];
        else Require(false, "unknown option: " + option);
    }
    Require(!sourcePath.empty(), "--source is required");

    std::ifstream input(sourcePath, std::ios::binary);
    Require(input.good(), "source file cannot be opened");
    const std::string script((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::string chunkName = "@" + sourcePath;

    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    Control control;
    std::thread([&control] {
        std::string command;
        while (std::getline(std::cin, command)) {
            if (command.compare(0, 3, "\xEF\xBB\xBF") == 0) command.erase(0, 3);
            if (!command.empty() && command.back() == '\r') command.pop_back();
            if (command == "reset") control.reset.store(true, std::memory_order_release);
            else if (command == "close-vm") control.closeVm.store(true, std::memory_order_release);
            else if (command == "stop") {
                control.stop.store(true, std::memory_order_release);
                return;
            }
        }
        control.stop.store(true, std::memory_order_release);
    }).detach();

    lua_State* state = CreateState();
    std::cout << "{\"ready\":true,\"pid\":" << ProcessId() << "}" << std::endl;

    bool vmClosedReported = false;
    while (!control.stop.load(std::memory_order_acquire)) {
        if (control.reset.exchange(false, std::memory_order_acq_rel)) {
            if (state != nullptr) {
                lua_close(state);
                state = nullptr;
                vmClosedReported = true;
            }
            state = CreateState();
            vmClosedReported = false;
            std::cout << "{\"reset\":true,\"ready\":true}" << std::endl;
        }
        if (control.closeVm.exchange(false, std::memory_order_acq_rel)) {
            if (state != nullptr) {
                lua_close(state);
                state = nullptr;
                vmClosedReported = true;
                std::cout << "{\"vmClosed\":true}" << std::endl;
            }
        }
        if (state != nullptr) RunScript(state, script, chunkName);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (state != nullptr) {
        lua_close(state);
        state = nullptr;
        vmClosedReported = true;
    }
    if (vmClosedReported) std::cout << "{\"vmClosed\":true}" << std::endl;
    std::cout << "{\"closed\":true}" << std::endl;
    return 0;
}

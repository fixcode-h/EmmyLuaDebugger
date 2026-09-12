#include "emmy_hook.h"
#include <cassert>
#include <set>
#include <vector>
#include <unordered_map>
#include <chrono>
#include "emmy_debugger/emmy_facade.h"
#include "emmy_debugger/api/lua_api.h"
#include <ShlObj.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include "easyhook.h"
#include "libpe/libpe.h"
#include "io.h"
#include "emmy_debugger/transporter/socket_server_transporter.h"
#include "shared/shme.h"
#include "hook_manager.h"


typedef TRACED_HOOK_HANDLE HOOK_HANDLE;
typedef NTSTATUS HOOK_STATUS;

HOOK_STATUS Hook(void* InEntryPoint,
                 void* InHookProc,
                 void* InCallback,
                 HOOK_HANDLE OutHandle);

HOOK_STATUS UnHook(HOOK_HANDLE InHandle);

typedef int (*_lua_pcall)(lua_State* L, int nargs, int nresults, int errfunc);

typedef int (*_lua_pcallk)(lua_State* L, int nargs, int nresults, int errfunc, lua_KContext ctx, lua_KFunction k);

typedef int (*_lua_resume_54)(lua_State* L, lua_State* from, int nargs, int* nresults);

typedef int (*_lua_resume_53_52)(lua_State* L, lua_State* from, int narg);

typedef int (*_lua_resume_51)(lua_State* L, int narg);

typedef void (*_lua_close)(lua_State* L);

typedef HMODULE (WINAPI *LoadLibraryExW_t)(LPCWSTR lpFileName, HANDLE hFile, DWORD dwFlags);

LoadLibraryExW_t LoadLibraryExW_dll = nullptr;

// 使用 Windows 原生的 SRWLOCK，它可以在编译时静态初始化（SRWLOCK_INIT）
// 完全不需要运行时初始化，避免了 std::mutex 在 DLL 加载场景下的初始化问题
// SRWLOCK 比 CRITICAL_SECTION 更轻量，且不需要调用 InitializeCriticalSection
static SRWLOCK g_srwPostLoadModule = SRWLOCK_INIT;
static std::set<std::string> g_loadedModules;
static HookManager g_hookManager;

void UninstallAllHooks();

HOOK_STATUS Hook(void* InEntryPoint,
                 void* InHookProc,
                 void* InCallback,
                 HOOK_HANDLE OutHandle)
{
	const auto hHook = OutHandle != nullptr ? OutHandle : new HOOK_TRACE_INFO();
	ULONG ACLEntries[1] = {0};
	HOOK_STATUS status = LhInstallHook(
		InEntryPoint,
		InHookProc,
		InCallback,
		hHook);
	if (status != 0) {
		delete hHook;
		return status;
	}
	status = LhSetExclusiveACL(ACLEntries, 0, hHook);
	if (status != 0) {
		LhUninstallHook(hHook);
		delete hHook;
		return status;
	}
	HookManager::HookChainRecord chain;
	chain.emmyHook = reinterpret_cast<void*>(InHookProc);
	chain.owner = "EmmyAttach";
	if (!g_hookManager.AddHook(hHook,
		[](void* handle) {
			const auto hook = reinterpret_cast<HOOK_HANDLE>(handle);
			if (LhUninstallHook(hook) != 0) return false;
			// EasyHook clears the trace handle synchronously; its trampoline is
			// reclaimed separately by LhWaitForPendingRemovals.
			delete hook;
			return true;
		}, chain)) {
		LhUninstallHook(hHook);
		delete hHook;
		return static_cast<HOOK_STATUS>(-1);
	}
	return status;
}

HOOK_STATUS UnHook(HOOK_HANDLE InHandle)
{
	ULONG ACLEntries[1] = {0};
	const HOOK_STATUS status = LhSetExclusiveACL(ACLEntries, 0, InHandle);
	return status;
}

// Lua calls (including pcallk continuations) may longjmp. Count only Emmy's
// work; a C++ scope must never remain active across the host's Lua execution.
// EasyHook owns the lifetime of the original-call trampoline independently.
void AttachFromHook(lua_State* L) {
	HookManager::CallbackScope callback(g_hookManager);
	if (callback) EmmyFacade::Get().Attach(L);
}

int lua_pcall_worker(lua_State* L, int nargs, int nresults, int errfunc)
{
	LPVOID lp;
	LhBarrierGetCallback(&lp);
	const auto pcall = (_lua_pcall)lp;
	AttachFromHook(L);
	return pcall(L, nargs, nresults, errfunc);
}

int lua_pcallk_worker(lua_State* L, int nargs, int nresults, int errfunc, lua_KContext ctx, lua_KFunction k)
{
	LPVOID lp;
	LhBarrierGetCallback(&lp);
	const auto pcallk = (_lua_pcallk)lp;
	AttachFromHook(L);
	return pcallk(L, nargs, nresults, errfunc, ctx, k);
}

int lua_error_worker(lua_State* L)
{
	typedef int (*dll_lua_error)(lua_State*);
	LPVOID lp;
	LhBarrierGetCallback(&lp);
	const auto error = (dll_lua_error)lp;
	AttachFromHook(L);
	// EmmyFacade::Get().BreakHere(L);
	return error(L);
}

int lua_resume_worker_54(lua_State* L, lua_State* from, int nargs, int* nresults)
{
	LPVOID lp;
	LhBarrierGetCallback(&lp);
	const auto luaResume = (_lua_resume_54)lp;
	AttachFromHook(L);
	return luaResume(L, from, nargs, nresults);
}

int lua_resume_worker_53_52(lua_State* L, lua_State* from, int nargs)
{
	LPVOID lp;
	LhBarrierGetCallback(&lp);
	const auto luaResume = (_lua_resume_53_52)lp;
	AttachFromHook(L);
	return luaResume(L, from, nargs);
}

int lua_resume_worker_51(lua_State* L, int nargs)
{
	LPVOID lp;
	LhBarrierGetCallback(&lp);
	const auto luaResume = (_lua_resume_51)lp;
	AttachFromHook(L);
	return luaResume(L, nargs);
}

void lua_close_worker(lua_State* L)
{
	LPVOID lp;
	LhBarrierGetCallback(&lp);
	const auto luaClose = (_lua_close)lp;
	HookManager::CallbackScope callback(g_hookManager);
	if (callback) {
		// The facade only records the close and clears debugger-owned hooks here;
		// the original Lua state is still alive and is closed immediately after
		// this callback returns.
		EmmyFacade::Get().OnLuaStateGC(L);
	}
	luaClose(L);
}

#define HOOK(FN, WORKER, REQUIRED) {\
	const auto it = symbols.find(""#FN"");\
	if (it != symbols.end()) {\
		const auto ptr = (void*)it->second; \
		const auto hHook = new HOOK_TRACE_INFO(); \
		Hook(ptr, (void*)(WORKER), ptr, hHook); \
	} else if (REQUIRED) {\
		printf("[ERR]function %s not found.\n", ""#FN"");\
		return;\
	}\
}

#define EXIST_SYMBOL(FN) (symbols.find(""#FN"") != symbols.end())

void HookLuaFunctions(std::unordered_map<std::string, DWORD64>& symbols)
{
	if (symbols.empty())
		return;
	// lua 5.1
	HOOK(lua_pcall, lua_pcall_worker, false);
	// All supported Lua versions expose lua_close with the same ABI. Notify the
	// facade before the host frees the state so VM lifecycle consumers receive a
	// deterministic CLOSING/CLOSED sequence.
	HOOK(lua_close, lua_close_worker, false);
	// lua 5.2
	HOOK(lua_pcallk, lua_pcallk_worker, false);
	// HOOK(lua_error, lua_error_worker, true);

	// lua5.4
	if (EXIST_SYMBOL(lua_newuserdatauv)) 
	{
		HOOK(lua_resume, lua_resume_worker_54, false);
	}
	else if(EXIST_SYMBOL(lua_rotate) || EXIST_SYMBOL(lua_callk)) //lua5.3 lua5.2
	{
		HOOK(lua_resume, lua_resume_worker_53_52, false);
	}
	else // lua5.1 or luajit
	{
		HOOK(lua_resume, lua_resume_worker_51, false);
	}
}

namespace {
// Image tables are read through object-free helpers: MSVC forbids __try in a
// function that also needs C++ object unwinding (C2712), so the SEH guards only
// touch raw pointers and PODs and the containers are filled afterwards.

const IMAGE_IMPORT_DESCRIPTOR* ReadImageImportTable(const uint8_t* imageBase)
{
	__try
	{
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imageBase + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
		const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (directory.VirtualAddress == 0 || directory.Size == 0) return nullptr;
		return reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(imageBase + directory.VirtualAddress);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

struct RawImageExport {
	const char* name;
	DWORD rva;
};

// Writes the exported lua* symbols of a loaded module image into out[] and
// returns how many were found. The export table is read straight from memory:
// the previous peOpenFile path read the whole PE from disk for every module
// (200+ modules in a UE editor, measured at ~17s per attach) and also blocked
// the handshake.
size_t ReadLuaExports(const uint8_t* imageBase, RawImageExport* out, size_t capacity)
{
	if (imageBase == nullptr || capacity == 0) return 0;
	size_t count = 0;
	__try
	{
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imageBase + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
		const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (directory.VirtualAddress == 0 || directory.Size == 0) return 0;
		const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
			imageBase + directory.VirtualAddress);
		const auto* names = reinterpret_cast<const DWORD*>(imageBase + exports->AddressOfNames);
		const auto* ordinals = reinterpret_cast<const WORD*>(imageBase + exports->AddressOfNameOrdinals);
		const auto* functions = reinterpret_cast<const DWORD*>(imageBase + exports->AddressOfFunctions);
		for (DWORD index = 0; index < exports->NumberOfNames && count < capacity; ++index)
		{
			const char* name = reinterpret_cast<const char*>(imageBase + names[index]);
			if (name[0] != 'l' || name[1] != 'u' || name[2] != 'a') continue;
			const DWORD ordinal = ordinals[index];
			if (ordinal >= exports->NumberOfFunctions) continue;
			const DWORD rva = functions[ordinal];
			// A forwarded export stores a name string inside the export directory
			// rather than code, so it has no hookable address.
			if (rva >= directory.VirtualAddress && rva < directory.VirtualAddress + directory.Size) continue;
			out[count].name = name;
			out[count].rva = rva;
			++count;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
	return count;
}

// Collects the imported module names of a loaded image as raw pointers. Kept
// free of C++ objects so the __try/__except above stays legal (C2712).
size_t ReadImportNames(const uint8_t* imageBase, const char** out, size_t capacity)
{
	if (imageBase == nullptr || out == nullptr || capacity == 0) return 0;
	const auto* descriptor = ReadImageImportTable(imageBase);
	if (descriptor == nullptr) return 0;
	size_t count = 0;
	__try
	{
		for (; descriptor->Name != 0 && count < capacity; ++descriptor)
			out[count++] = reinterpret_cast<const char*>(imageBase + descriptor->Name);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
	return count;
}

std::vector<std::string> ImageImportNames(const uint8_t* imageBase)
{
	constexpr size_t kMaxImports = 256;
	const char* names[kMaxImports];
	const size_t count = ReadImportNames(imageBase, names, kMaxImports);
	std::vector<std::string> modules;
	modules.reserve(count);
	for (size_t index = 0; index < count; ++index)
		if (names[index] != nullptr) modules.emplace_back(names[index]);
	return modules;
}
} // namespace

void LoadSymbolsRecursively(HANDLE hProcess, HMODULE hModule)
{
	if (hModule == nullptr) return;

	char moduleName[_MAX_PATH];
	ZeroMemory(moduleName, _MAX_PATH);
	DWORD nameLen = GetModuleBaseName(hProcess, hModule, moduleName, _MAX_PATH);
	if (nameLen == 0 || g_loadedModules.find(moduleName) != g_loadedModules.end())
		return;

	g_loadedModules.insert(moduleName);
	char modulePath[_MAX_PATH];
	// skip modules in c://WINDOWS
	{
		ZeroMemory(modulePath, _MAX_PATH);
		DWORD fileNameLen = GetModuleFileNameEx(hProcess, hModule, modulePath, _MAX_PATH);
		if (fileNameLen == 0)
			return;

		char windowsPath[MAX_PATH];
		if (SHGetFolderPath(nullptr, CSIDL_WINDOWS, nullptr, SHGFP_TYPE_CURRENT, windowsPath) == 0)
		{
			std::string module_path = modulePath;
			if (module_path.find(windowsPath) != std::string::npos)
			{
				return;
			}
		}
	}
	// skip emmy modules
	{
		static const char* emmyModules[] = {"emmy_hook.dll", "EasyHook.dll"};
		for (const char* emmyModuleName : emmyModules)
		{
			if (strcmp(moduleName, emmyModuleName) == 0)
				return;
		}
	}

	const auto* const imageBase = reinterpret_cast<const uint8_t*>(hModule);
	constexpr size_t kMaxLuaExports = 256;
	RawImageExport exports[kMaxLuaExports];
	const size_t exportCount = ReadLuaExports(imageBase, exports, kMaxLuaExports);
	std::unordered_map<std::string, DWORD64> symbols;
	for (size_t index = 0; index < exportCount; ++index)
	{
		symbols[exports[index].name] = reinterpret_cast<DWORD64>(hModule) + exports[index].rva;
		EmmyFacade::Get().SendLog(LogType::Debug, "\t[B]Lua symbol: %s (%s)",
			exports[index].name, moduleName);
	}

	HookLuaFunctions(symbols);

	// A module the loader pulls in as a dependency of another one never passes
	// through LoadLibraryExW, so its Lua symbols only surface through this walk.
	const auto imports = ImageImportNames(imageBase);
	for (const auto& imported : imports)
		LoadSymbolsRecursively(hProcess, GetModuleHandle(imported.c_str()));
}

void PostLoadLibrary(HMODULE hModule)
{
	extern HINSTANCE g_hInstance;
	if (hModule == g_hInstance)
	{
		return;
	}

	HANDLE hProcess = GetCurrentProcess();

	char moduleName[_MAX_PATH];
	GetModuleBaseName(hProcess, hModule, moduleName, _MAX_PATH);

	// 使用 SRWLOCK 进行同步，它在编译时初始化，不会有运行时初始化问题
	AcquireSRWLockExclusive(&g_srwPostLoadModule);
	LoadSymbolsRecursively(hProcess, hModule);
	ReleaseSRWLockExclusive(&g_srwPostLoadModule);
}

HMODULE WINAPI LoadLibraryExW_intercept(LPCWSTR fileName, HANDLE hFile, DWORD dwFlags)
{
	// We have to call the loader lock (if it is available) so that we don't get deadlocks
	// in the case where Dll initialization acquires the loader lock and calls LoadLibrary
	// while another thread is inside PostLoadLibrary.
	HMODULE hModule = LoadLibraryExW_dll(fileName, hFile, dwFlags);
	HookManager::CallbackScope callback(g_hookManager);
	if (!callback) return hModule;

	if (hModule != nullptr)
	{
		PostLoadLibrary(hModule);
	}
	return hModule;
}

void HookLoadLibrary()
{
	HMODULE hModuleKernel = GetModuleHandle("KernelBase.dll");
	if (hModuleKernel == nullptr)
		hModuleKernel = GetModuleHandle("kernel32.dll");
	if (hModuleKernel != nullptr)
	{
		// LoadLibraryExW is called by the other LoadLibrary functions, so we
		// only need to hook it.

		// TODO hook!!!
		LoadLibraryExW_dll = (LoadLibraryExW_t)GetProcAddress(hModuleKernel, "LoadLibraryExW");

		const auto hHook = new HOOK_TRACE_INFO();
		Hook((void*)LoadLibraryExW_dll, (void*)LoadLibraryExW_intercept,
			(PVOID)nullptr, hHook);
	}
}

void UninstallAllHooks()
{
	// Disable the fast path before asking EasyHook to remove anything. This
	// prevents a callback racing with module/transport destruction.
	if (!g_hookManager.DisableAndQuiesce(std::chrono::milliseconds(5000))) {
		EmmyFacade::Get().SendLog(LogType::Error,
			"Emmy hook teardown timed out; retaining hook handles for retry");
		return;
	}
	if (!g_hookManager.UnhookIfSafe()) {
		EmmyFacade::Get().SendLog(LogType::Warning,
			"Emmy hook teardown could not release every hook; retry is possible");
		return;
	}
	if (LhWaitForPendingRemovals() != 0) {
		EmmyFacade::Get().SendLog(LogType::Warning,
			"EasyHook retained an active trampoline; the Agent DLL remains loaded");
	}
	AcquireSRWLockExclusive(&g_srwPostLoadModule);
	g_loadedModules.clear();
	ReleaseSRWLockExclusive(&g_srwPostLoadModule);
}

void redirect(int port)
{
	HANDLE readStdPipe = NULL, writeStdPipe = NULL;

	SECURITY_ATTRIBUTES saAttr;
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = nullptr;

	CreatePipe(&readStdPipe, &writeStdPipe, &saAttr, 0);

	const auto oldStdout = _dup(_fileno(stdout));
	const auto oldStderr = _dup(_fileno(stderr));

	if (oldStdout == -1 || oldStderr == -1)
	{
		printf("stdout or stderr redirect error");
		if (oldStdout != -1)
		{
			_close(oldStdout);
		}
		if (oldStderr != -1)
		{
			_close(oldStderr);
		}

		return;
	}

	const auto stream = _open_osfhandle(reinterpret_cast<intptr_t>(writeStdPipe), 0);
	FILE* capture = nullptr;
	if (stream == -1)
	{
		printf("capture stream open fail");
		_dup2(oldStdout, _fileno(stdout));
		_dup2(oldStderr, _fileno(stderr));

		_close(oldStdout);
		_close(oldStderr);

		return;
	}
	capture = _fdopen(stream, "wt");

	// stdout now refers to file "capture" 
	if (_dup2(_fileno(capture), _fileno(stdout)) == -1)
	{
		printf("Can't _dup2 stdout to capture");

		_dup2(oldStdout, _fileno(stdout));
		_dup2(oldStderr, _fileno(stderr));

		_close(oldStdout);
		_close(oldStderr);

		return;
	}

	// stderr now refers to file "capture" 
	if (_dup2(_fileno(capture), _fileno(stderr)) == -1)
	{
		printf("Can't _dup2 stderr to capture");

		_dup2(oldStdout, _fileno(stdout));
		_dup2(oldStderr, _fileno(stderr));

		_close(oldStdout);
		_close(oldStderr);

		return;
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	// 1024 - 65535
	while (port > 0xffff) port -= 0xffff;
	while (port < 0x400) port += 0x400;
	port++;

	const auto transport = std::make_shared<SocketServerTransporter>();
	std::string err;
	// Same IPv4-loopback rationale as EmmyFacade::StartupHookMode: the log
	// channel must stay reachable under a loopback-scoped inbound exemption.
	const auto suc = transport->Listen("127.0.0.1", port, err);

	if (!suc)
	{
		printf("capture log error : %s", err.c_str());

		_dup2(oldStdout, _fileno(stdout));
		_dup2(oldStderr, _fileno(stderr));

		_close(oldStdout);
		_close(oldStderr);

		return;
	}

	std::thread thread([transport,readStdPipe,oldStdout]()
	{
		char buf[1024] = {0};
		while (true)
		{
			DWORD readWord;
			ZeroMemory(buf, 1024);
			const bool suc = ReadFile(readStdPipe, buf, 1024, &readWord, nullptr);

			if (suc && readWord > 0)
			{
				_write(oldStdout, buf, readWord);
				transport->Send(buf, readWord);
			}
		}
	});
	thread.detach();
}

int StartupHookMode(void* lpParam)
{
	// 在这里初始化 EmmyFacade，而不是在 DllMain 中
	// 因为 DllMain 在 loader lock 下执行，CRT 可能还没有完全初始化
	auto& facade = EmmyFacade::Get();
	facade.SetWorkMode(WorkMode::Attach);
	if (lpParam != nullptr) {
		const auto* params = static_cast<const RemoteThreadParam*>(lpParam);
		facade.SetExpectedAuthToken(params->authToken);
	} else {
		facade.SetExpectedAuthToken(std::string());
	}
	
	const int pid = (int)GetCurrentProcessId();
	if (!facade.StartupHookMode(pid)) {
		return 1;
	}
	if (!g_hookManager.IsEnabled() && g_hookManager.HookCount() != 0) {
		facade.Destroy();
		return 1;
	}
	// StartupHookMode may tear down a previous session;
	// install the callbacks only after the new transport is listening.
	facade.StartHook = FindAndHook;
	facade.StopHook = UninstallAllHooks;

	if (lpParam != nullptr && ((RemoteThreadParam*)lpParam)->bRedirect)
	{
		redirect(pid);
	}

	return 0;
}

void FindAndHook()
{
	if (g_hookManager.IsEnabled()) return;
	if (!g_hookManager.Enable()) {
		// A previous generation may still own EasyHook handles (LhUninstallHook
		// can fail while its trampoline is in use) and HookManager refuses to
		// enable in that state. Retry the teardown once, then report: a connected
		// agent without any Lua hook looks exactly like "attach worked but no
		// debug window and no breakpoints", which is impossible to diagnose from
		// the client side.
		g_hookManager.UnhookIfSafe();
		if (!g_hookManager.Enable()) {
			EmmyFacade::Get().SendLog(LogType::Error,
				"Emmy hook enable failed: the agent has no Lua hook (hook handles were retained)");
			return;
		}
	}
	// 重要：先处理现有模块，最后再安装钩子
	// 如果先安装钩子，LoadSymbolsRecursively 中的操作（如 SendLog、peOpenFile）
	// 可能触发新的 DLL 加载，导致 LoadLibraryExW_intercept 被调用，
	// 它会尝试获取已经被 PostLoadLibrary 持有的锁，造成死锁
	// （SRWLOCK 不支持递归锁定）

	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
	if (hSnapshot != INVALID_HANDLE_VALUE)
	{
		MODULEENTRY32 module;
		module.dwSize = sizeof(MODULEENTRY32);
		BOOL moreModules = Module32First(hSnapshot, &module);
		while (moreModules)
		{
			PostLoadLibrary(module.hModule);
			moreModules = Module32Next(hSnapshot, &module);
		}
	}
	else
	{
		HMODULE module = GetModuleHandle(nullptr);
		PostLoadLibrary(module);
	}
	CloseHandle(hSnapshot);

	// 最后安装钩子，拦截后续的 DLL 加载
	HookLoadLibrary();
}

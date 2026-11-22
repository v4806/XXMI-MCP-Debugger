#include <Windows.h>

#include <atomic>
#include <filesystem>

#include "DiagnosticLog.h"
#include "MCPBridge.h"
#include "MigotoAdapter.h"

namespace {

HMODULE g_module = nullptr;
std::atomic<bool> g_stopping{false};

std::filesystem::path ModulePath(HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return std::filesystem::path(path);
}

DWORD WINAPI StartThread(void*) {
    const auto module_path = ModulePath(g_module);
    const auto log_path = module_path.empty()
                              ? std::filesystem::path(L"xxmi_mcp.log")
                              : module_path.parent_path() / L"xxmi_mcp.log";
    DiagnosticLog::Initialize(log_path);
    DiagnosticLog::Info("standalone MCP DLL loaded");

    HMODULE pinned_module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN |
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&StartThread),
            &pinned_module) ||
        !pinned_module) {
        DiagnosticLog::Error(
            "could not pin mcp.dll until process exit; control hooks "
            "disabled, Win32 error " +
            std::to_string(GetLastError()));
        DiagnosticLog::Shutdown();
        return 0;
    }
    DiagnosticLog::Info(
        "mcp.dll pinned until process exit; game restart is required to "
        "update or remove it");

    static MigotoAdapter adapter;
    MCP_Start(&adapter);
    adapter.Run(g_stopping);
    if (!MCP_Stop()) {
        DiagnosticLog::Error(
            "standalone MCP shutdown is incomplete; retaining runtime "
            "state until process exit");
        return 0;
    }

    DiagnosticLog::Info("standalone MCP DLL stopping");
    DiagnosticLog::Shutdown();
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        g_stopping.store(false, std::memory_order_release);
        DisableThreadLibraryCalls(module);
        const HANDLE thread = CreateThread(nullptr, 0, StartThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        return thread ? TRUE : FALSE;
    }

    if (reason == DLL_PROCESS_DETACH) {
        g_stopping.store(true, std::memory_order_release);
    }
    return TRUE;
}

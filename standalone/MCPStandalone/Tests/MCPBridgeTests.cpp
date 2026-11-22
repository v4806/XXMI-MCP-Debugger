#include <Windows.h>

#include <chrono>
#include <string>
#include <thread>

#include "MCPBridge.h"
#include "TestHarness.h"

namespace {

std::wstring WaitForBridgePipe(std::uint32_t timeout_ms) {
    wchar_t pattern[256] = {};
    swprintf_s(
        pattern,
        L"\\\\.\\pipe\\xxmi-mcp-v1-%lu-*",
        static_cast<unsigned long>(GetCurrentProcessId()));

    const auto deadline =
        GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        WIN32_FIND_DATAW data = {};
        const HANDLE search = FindFirstFileW(pattern, &data);
        if (search != INVALID_HANDLE_VALUE) {
            FindClose(search);
            return std::wstring(L"\\\\.\\pipe\\") + data.cFileName;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {};
}

HANDLE ConnectToPipe(const std::wstring& path) {
    return CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
}

}  // namespace

TEST_CASE(MCPBridgeStopsWhileWaitingForClient) {
    MCP_Start(nullptr);
    REQUIRE(!WaitForBridgePipe(2000).empty());
    REQUIRE(MCP_Stop(500));
}

TEST_CASE(MCPBridgeStopsWithConnectedIdleClient) {
    MCP_Start(nullptr);
    const auto path = WaitForBridgePipe(2000);
    REQUIRE(!path.empty());

    const HANDLE client = ConnectToPipe(path);
    REQUIRE(client != INVALID_HANDLE_VALUE);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const bool stopped = MCP_Stop(500);
    CloseHandle(client);
    REQUIRE(stopped);
}

TEST_CASE(MCPBridgeUsesLongerRenderWaitForConfigReload) {
    REQUIRE_EQ(MCP_RenderWaitMsForCommand(3), 8000U);
    REQUIRE_EQ(MCP_RenderWaitMsForCommand(4), 1500U);
}

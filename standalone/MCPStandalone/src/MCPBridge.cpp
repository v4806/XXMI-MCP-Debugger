#include "MCPBridge.h"

#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "AdapterTypes.h"
#include "CommandQueue.h"
#include "DiagnosticLog.h"
#include "MigotoAdapter.h"

#pragma comment(lib, "advapi32.lib")

namespace {

constexpr std::uint32_t kMagic = 0x3150434D;  // MCP1
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::uint32_t kMaxPayload = 16U * 1024U;
constexpr DWORD kRenderWaitMs = 1500;
constexpr DWORD kConfigReloadWaitMs = 8000;

enum class Command : std::uint16_t {
    Status = 1,
    FrameAnalysis = 2,
    ReloadConfig = 3,
    ReloadShaders = 4,
    Ping = 5,
    LiveContext = 6,
    RuntimeVariables = 7,
    LiveDrawSnapshot = 8,
};

enum Status : std::uint32_t {
    Ok = 0,
    BadRequest = 1,
    Unsupported = 2,
    Busy = 3,
    NotReady = 4,
    Failed = 5,
    Timeout = 6,
    Stopping = 7,
};

struct RequestHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t command;
    std::uint32_t request_id;
    std::uint32_t payload_size;
};

struct ResponseHeader {
    std::uint32_t magic;
    std::uint32_t status;
    std::uint32_t request_id;
    std::uint32_t payload_size;
};

static_assert(sizeof(RequestHeader) == 16);
static_assert(sizeof(ResponseHeader) == 16);

SRWLOCK g_lock = SRWLOCK_INIT;
HANDLE g_pipe_thread = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_pipe = nullptr;
wchar_t g_pipe_name[256] = {};
std::string g_instance_id;
MigotoAdapter* g_adapter = nullptr;

std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (!size) {
        CloseHandle(token);
        return {};
    }
    std::vector<std::uint8_t> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);

    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid))
        return {};
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}

std::string Sanitize(std::string value) {
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    return value;
}

void Append(std::ostringstream& stream, const char* key, const std::string& value) {
    stream << key << '=' << Sanitize(value) << '\n';
}

template <typename T>
void Append(std::ostringstream& stream, const char* key, const T& value) {
    stream << key << '=' << value << '\n';
}

struct PipeCreateResult {
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD error = ERROR_SUCCESS;
};

PipeCreateResult CreatePipeInstance() {
    const auto sid = CurrentUserSid();
    if (sid.empty())
        return {INVALID_HANDLE_VALUE, ERROR_INVALID_SID};

    const std::wstring sddl = L"D:(A;;GA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return {INVALID_HANDLE_VALUE, GetLastError()};
    }

    SECURITY_ATTRIBUTES attributes = {
        sizeof(SECURITY_ATTRIBUTES),
        descriptor,
        FALSE,
    };
    HANDLE pipe = CreateNamedPipeW(
        g_pipe_name,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1,
        kMaxPayload + sizeof(ResponseHeader),
        kMaxPayload + sizeof(RequestHeader),
        0,
        &attributes);
    const auto error = GetLastError();
    LocalFree(descriptor);
    return {pipe, error};
}

}  // namespace

namespace {

bool ReadAll(HANDLE pipe, void* buffer, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    std::size_t offset = 0;
    while (offset < size) {
        DWORD transferred = 0;
        const DWORD remaining = static_cast<DWORD>(
            std::min<std::size_t>(size - offset, MAXDWORD));
        if (!ReadFile(pipe, bytes + offset, remaining, &transferred, nullptr) ||
            transferred == 0) {
            return false;
        }
        offset += transferred;
    }
    return true;
}

bool WriteAll(HANDLE pipe, const void* buffer, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    std::size_t offset = 0;
    while (offset < size) {
        DWORD transferred = 0;
        const DWORD remaining = static_cast<DWORD>(
            std::min<std::size_t>(size - offset, MAXDWORD));
        if (!WriteFile(pipe, bytes + offset, remaining, &transferred, nullptr) ||
            transferred == 0) {
            return false;
        }
        offset += transferred;
    }
    return true;
}

std::string BuildStatusPayload() {
    std::ostringstream stream;
    if (!g_adapter) {
        Append(stream, "adapter_mode", "read_only");
        Append(stream, "adapter_ready", "0");
        Append(stream, "capture_state", "not_started");
        Append(stream, "device_captured", "0");
        Append(stream, "present_hooked", "0");
        Append(stream, "capabilities", "");
        Append(stream, "adapter_capabilities", "");
        Append(stream, "capture_restore_hunting", "-1");
        return stream.str();
    }

    const auto snapshot = g_adapter->Snapshot();
    Append(stream, "protocol", kProtocolVersion);
    Append(stream, "instance", g_instance_id);
    Append(stream, "pid", GetCurrentProcessId());
    Append(stream, "dll_version", "standalone");
    Append(
        stream,
        "adapter_mode",
        snapshot.mode == AdapterSnapshotMode::Full
            ? "full"
            : snapshot.mode == AdapterSnapshotMode::Partial ? "partial"
                                                             : "read_only");
    Append(stream, "adapter_ready", snapshot.adapter_ready ? "1" : "0");
    Append(
        stream,
        "capture_state",
        AdapterCaptureStateName(snapshot.capture_state));
    Append(
        stream,
        "device_captured",
        snapshot.device_captured ? "1" : "0");
    Append(
        stream,
        "present_hooked",
        snapshot.present_hooked ? "1" : "0");

    std::string capabilities;
    for (const auto& capability : snapshot.capabilities) {
        if (!capabilities.empty())
            capabilities += ',';
        capabilities += capability;
    }
    Append(stream, "capabilities", capabilities);
    Append(stream, "adapter_capabilities", capabilities);
    Append(stream, "process_path", snapshot.process_path);
    Append(stream, "dll_path",
           snapshot.dll_path.empty() ? "mcp.dll" : snapshot.dll_path);
    Append(stream, "config_path", snapshot.config_path);
    Append(stream, "log_path", snapshot.log_path);
    Append(
        stream,
        "log_enabled",
        !snapshot.log_path.empty() &&
                std::filesystem::is_regular_file(
                    std::filesystem::path(snapshot.log_path))
            ? "1"
            : "0");
    Append(stream, "analysis_root", snapshot.analysis_root);
    if (snapshot.globals_resolved)
        Append(stream, "analysis_path", snapshot.analysis_path);
    Append(stream, "shader_path", snapshot.shader_path);
    Append(stream, "frame", snapshot.frame);
    Append(stream, "fps", snapshot.fps);
    Append(stream, "frame_time_ms", snapshot.frame_time_ms);
    Append(stream, "uptime_s", snapshot.uptime_s);
    if (snapshot.hunting)
        Append(stream, "hunting", *snapshot.hunting);
    if (snapshot.analysis_active)
        Append(stream, "analysis_active", *snapshot.analysis_active ? "1" : "0");
    if (snapshot.analysis_number)
        Append(stream, "analysis_number", *snapshot.analysis_number);
    Append(stream, "screen_width", snapshot.screen_width);
    Append(stream, "screen_height", snapshot.screen_height);
    Append(stream, "adapter_error_count", snapshot.adapter_error_count);
    Append(stream, "adapter_log_path", snapshot.adapter_log_path);
    Append(
        stream,
        "adapter_module_sha256",
        snapshot.adapter_module_sha256);
    Append(
        stream,
        "capture_restore_hunting",
        snapshot.capture_restore_hunting);
    return stream.str();
}

std::string ErrorPayload(
    const std::string& code,
    const std::string& message) {
    std::ostringstream stream;
    Append(stream, "error", code);
    Append(stream, "error_message", message);
    return stream.str();
}

bool SendResponse(
    HANDLE pipe,
    std::uint32_t request_id,
    std::uint32_t status,
    const std::string& payload) {
    ResponseHeader header = {
        kMagic,
        status,
        request_id,
        static_cast<std::uint32_t>(payload.size()),
    };
    return WriteAll(pipe, &header, sizeof(header)) &&
           WriteAll(pipe, payload.data(), payload.size());
}

std::optional<ControlCommand> ToControlCommand(std::uint16_t command) {
    switch (command) {
    case static_cast<std::uint16_t>(Command::FrameAnalysis):
        return ControlCommand::FrameAnalysis;
    case static_cast<std::uint16_t>(Command::ReloadConfig):
        return ControlCommand::ReloadConfig;
    case static_cast<std::uint16_t>(Command::ReloadShaders):
        return ControlCommand::ReloadShaders;
    case static_cast<std::uint16_t>(Command::LiveContext):
        return ControlCommand::LiveContext;
    case static_cast<std::uint16_t>(Command::RuntimeVariables):
        return ControlCommand::RuntimeVariables;
    case static_cast<std::uint16_t>(Command::LiveDrawSnapshot):
        return ControlCommand::LiveDrawSnapshot;
    default:
        return std::nullopt;
    }
}

void HandleClient(HANDLE pipe) {
    RequestHeader request = {};
    if (!ReadAll(pipe, &request, sizeof(request)))
        return;

    if (request.magic != kMagic) {
        DiagnosticLog::Error(
            "MCP request " + std::to_string(request.request_id) +
            " rejected: bad_magic");
        SendResponse(
            pipe,
            request.request_id,
            BadRequest,
            ErrorPayload("bad_magic", "request magic is not MCP1"));
        return;
    }
    if (request.version != kProtocolVersion) {
        DiagnosticLog::Error(
            "MCP request " + std::to_string(request.request_id) +
            " rejected: unsupported protocol");
        SendResponse(
            pipe,
            request.request_id,
            BadRequest,
            ErrorPayload(
                "unsupported_protocol",
                "request protocol version is unsupported"));
        return;
    }
    if (request.payload_size > kMaxPayload) {
        DiagnosticLog::Error(
            "MCP request " + std::to_string(request.request_id) +
            " rejected: payload too large (" +
            std::to_string(request.payload_size) + ")");
        SendResponse(
            pipe,
            request.request_id,
            BadRequest,
            ErrorPayload(
                "payload_too_large",
                "request payload exceeds the configured limit"));
        return;
    }
    if (request.payload_size != 0) {
        std::vector<std::uint8_t> payload(request.payload_size);
        if (!ReadAll(pipe, payload.data(), payload.size()))
            return;
    }

    switch (static_cast<Command>(request.command)) {
    case Command::Ping: {
        std::ostringstream stream;
        Append(stream, "pong", "1");
        SendResponse(pipe, request.request_id, Ok, stream.str());
        return;
    }
    case Command::Status:
        SendResponse(
            pipe,
            request.request_id,
            Ok,
            BuildStatusPayload());
        return;
    case Command::FrameAnalysis:
    case Command::ReloadConfig:
    case Command::ReloadShaders:
    case Command::LiveContext:
    case Command::RuntimeVariables:
    case Command::LiveDrawSnapshot: {
        const auto control = ToControlCommand(request.command);
        if (!control || !g_adapter) {
            DiagnosticLog::Error(
                "MCP command " + std::to_string(request.command) +
                " request " + std::to_string(request.request_id) +
                " failed: adapter not ready");
            SendResponse(
                pipe,
                request.request_id,
                NotReady,
                ErrorPayload(
                    "adapter_not_ready",
                    "XXMI adapter has not captured a live device"));
            return;
        }

        std::string detail;
        if (!g_adapter->QueueCommand(
                request.request_id, *control, detail)) {
            const bool busy =
                detail.find("another native command") != std::string::npos ||
                detail.find("abandoned") != std::string::npos;
            const bool adapter_not_ready =
                detail.rfind("adapter is not ready", 0) == 0;
            DiagnosticLog::Error(
                "MCP command " + std::to_string(request.command) +
                " request " + std::to_string(request.request_id) +
                " rejected: " + detail);
            SendResponse(
                pipe,
                request.request_id,
                busy ? Busy : NotReady,
                ErrorPayload(
                    busy
                        ? "queue_busy"
                        : adapter_not_ready
                              ? "adapter_not_ready"
                              : "capability_unavailable",
                    detail));
            return;
        }

        CommandResult result;
        const auto wait = g_adapter->WaitCommand(
            request.request_id,
            MCP_RenderWaitMsForCommand(request.command),
            result);
        if (wait == QueueWaitResult::Completed) {
            if (result.status != Ok) {
                DiagnosticLog::Error(
                    "MCP command " + std::to_string(request.command) +
                    " request " + std::to_string(request.request_id) +
                    " completed with status " +
                    std::to_string(result.status) + ": " + result.payload);
            }
            SendResponse(
                pipe,
                request.request_id,
                result.status,
                result.payload);
        } else if (wait == QueueWaitResult::TimedOut) {
            DiagnosticLog::Warn(
                "native command timed out waiting for render thread");
            SendResponse(
                pipe,
                request.request_id,
                Timeout,
                ErrorPayload(
                    "render_timeout",
                    "game did not process the command before the timeout"));
        } else {
            SendResponse(
                pipe,
                request.request_id,
                BadRequest,
                ErrorPayload(
                    "invalid_request",
                    "command request ID is not pending"));
        }
        return;
    }
    default:
        DiagnosticLog::Error(
            "MCP command " + std::to_string(request.command) +
            " request " + std::to_string(request.request_id) +
            " rejected: unsupported command");
        SendResponse(
            pipe,
            request.request_id,
            Unsupported,
            ErrorPayload(
                "unsupported_command",
                "requested native command is unsupported"));
        return;
    }
}

DWORD WINAPI PipeThread(void*) {
    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        auto created = CreatePipeInstance();
        if (created.handle == INVALID_HANDLE_VALUE) {
            DiagnosticLog::Error(
                "CreateNamedPipeW failed with error " +
                std::to_string(created.error));
            WaitForSingleObject(g_stop_event, 500);
            continue;
        }

        {
            AcquireSRWLockExclusive(&g_lock);
            g_pipe = created.handle;
            ReleaseSRWLockExclusive(&g_lock);
        }

        const BOOL connected = ConnectNamedPipe(created.handle, nullptr);
        const auto connect_error = GetLastError();
        if (connected ||
            connect_error == ERROR_PIPE_CONNECTED) {
            HandleClient(created.handle);
            FlushFileBuffers(created.handle);
            DisconnectNamedPipe(created.handle);
        } else if (connect_error != ERROR_OPERATION_ABORTED &&
                   connect_error != ERROR_INVALID_HANDLE) {
            DiagnosticLog::Warn(
                "ConnectNamedPipe failed with error " +
                std::to_string(connect_error));
        }

        {
            AcquireSRWLockExclusive(&g_lock);
            g_pipe = nullptr;
            ReleaseSRWLockExclusive(&g_lock);
        }
        CloseHandle(created.handle);
    }
    return 0;
}

}  // namespace

void MCP_Start(MigotoAdapter* adapter) {
    g_adapter = adapter;
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) {
        DiagnosticLog::Error("could not create MCP stop event");
        return;
    }

    const auto seed =
        GetTickCount64() ^
        (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32) ^
        reinterpret_cast<std::uintptr_t>(&g_pipe_name);
    char instance[9] = {};
    std::snprintf(
        instance, sizeof(instance), "%08x",
        static_cast<std::uint32_t>(seed));
    g_instance_id = instance;
    std::swprintf(
        g_pipe_name,
        std::size(g_pipe_name),
        L"\\\\.\\pipe\\xxmi-mcp-v1-%lu-%hs",
        static_cast<unsigned long>(GetCurrentProcessId()),
        instance);

    g_pipe_thread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    if (!g_pipe_thread) {
        DiagnosticLog::Error("could not create MCP pipe thread");
        return;
    }
    DiagnosticLog::Info(
        "MCP pipe endpoint started");
}

std::uint32_t MCP_RenderWaitMsForCommand(std::uint16_t command) {
    if (command == static_cast<std::uint16_t>(Command::ReloadConfig))
        return kConfigReloadWaitMs;
    if (command == static_cast<std::uint16_t>(Command::LiveDrawSnapshot))
        return 3000;
    return kRenderWaitMs;
}

bool MCP_Stop(std::uint32_t timeout_ms) {
    if (g_stop_event)
        SetEvent(g_stop_event);

    if (!g_pipe_thread) {
        if (g_stop_event) {
            CloseHandle(g_stop_event);
            g_stop_event = nullptr;
        }
        g_adapter = nullptr;
        return true;
    }

    HANDLE pipe = nullptr;
    {
        AcquireSRWLockExclusive(&g_lock);
        pipe = g_pipe;
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (pipe)
        CancelIoEx(pipe, nullptr);

    const auto deadline =
        GetTickCount64() + timeout_ms;
    while (true) {
        const auto now = GetTickCount64();
        const DWORD remaining =
            now < deadline
                ? static_cast<DWORD>(
                      std::min<std::uint64_t>(deadline - now, 50))
                : 0;
        const DWORD wait =
            WaitForSingleObject(g_pipe_thread, remaining);
        if (wait == WAIT_OBJECT_0)
            break;
        if (wait == WAIT_FAILED) {
            DiagnosticLog::Error(
                "MCP pipe thread wait failed with Win32 error " +
                std::to_string(GetLastError()));
            return false;
        }

        if (!CancelSynchronousIo(g_pipe_thread)) {
            const auto error = GetLastError();
            if (error != ERROR_NOT_FOUND) {
                static DWORD last_cancel_error = ERROR_SUCCESS;
                if (last_cancel_error != error) {
                    DiagnosticLog::Error(
                        "CancelSynchronousIo failed for MCP pipe "
                        "thread with Win32 error " +
                        std::to_string(error));
                    last_cancel_error = error;
                }
            }
        }

        if (GetTickCount64() >= deadline) {
            DiagnosticLog::Error(
                "MCP pipe thread stop timed out; shutdown deferred");
            return false;
        }
    }

    CloseHandle(g_pipe_thread);
    g_pipe_thread = nullptr;
    if (g_stop_event) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
    g_adapter = nullptr;
    return true;
}

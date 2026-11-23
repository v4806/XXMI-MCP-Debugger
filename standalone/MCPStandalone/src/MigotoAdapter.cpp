#include "MigotoAdapter.h"

#include <chrono>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

#include "DiagnosticLog.h"
#include "Hash.h"
#include "HookRuntime.h"
#include "LiveStateCapture.h"
#include "ModuleImage.h"
#include "ProbeJson.h"
#include "RuntimeVariables.h"

namespace {

constexpr GUID kHackerDeviceIid = {
    0x83ffd841,
    0xa5c9,
    0x46f4,
    {0x81, 0x09, 0xbc, 0x25, 0x95, 0x58, 0xfe, 0xf4},
};
constexpr std::uintptr_t kShaderPathOffset = 0xC0;
constexpr std::uintptr_t kAnalysisPathOffset = 8;
constexpr std::uint64_t kDeviceCaptureTimeoutMs = 10000;

std::wstring ModulePath(HMODULE module) {
    std::wstring buffer(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(module, buffer.data(),
                           static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return buffer;
}

std::wstring ModulePathForAddress(const void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address),
            &module)) {
        return {};
    }
    return ModulePath(module);
}

std::filesystem::path Utf8Path(const std::string& value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::string Utf8String(const std::filesystem::path& value) {
    const auto utf8 = value.u8string();
    return std::string(
        reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

bool IsSystemD3D11(const std::wstring& path) {
    std::wstring lower = path;
    for (auto& character : lower)
        character = static_cast<wchar_t>(towlower(character));
    return lower.find(L"\\windows\\system32\\d3d11.dll") != std::wstring::npos ||
           lower.find(L"\\windows\\syswow64\\d3d11.dll") != std::wstring::npos;
}

std::string FileVersion(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size =
        GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0)
        return {};

    std::vector<std::uint8_t> data(size);
    if (!GetFileVersionInfoW(
            path.c_str(), 0, size, data.data())) {
        return {};
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(
            data.data(),
            L"\\",
            reinterpret_cast<LPVOID*>(&info),
            &info_size) ||
        !info ||
        info_size < sizeof(VS_FIXEDFILEINFO) ||
        info->dwSignature != 0xFEEF04BD) {
        return {};
    }

    std::ostringstream stream;
    stream << HIWORD(info->dwFileVersionMS) << '.'
           << LOWORD(info->dwFileVersionMS) << '.'
           << HIWORD(info->dwFileVersionLS) << '.'
           << LOWORD(info->dwFileVersionLS);
    return stream.str();
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

template <typename T>
std::optional<T> ReadValue(
    std::uintptr_t address,
    DWORD* error = nullptr) {
    if (error)
        *error = ERROR_SUCCESS;
    T value = {};
    SIZE_T read = 0;
    const BOOL read_ok = ReadProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<const void*>(address),
        &value,
        sizeof(value),
        &read);
    if (!read_ok || read != sizeof(value)) {
        if (error) {
            const auto last_error = GetLastError();
            *error = read_ok
                         ? ERROR_PARTIAL_COPY
                         : (last_error == ERROR_SUCCESS
                                ? ERROR_PARTIAL_COPY
                                : last_error);
        }
        return std::nullopt;
    }
    return value;
}

std::string ReadWideString(
    std::uintptr_t address,
    std::size_t capacity) {
    std::wstring value(capacity, L'\0');
    SIZE_T read = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(address),
            value.data(),
            value.size() * sizeof(wchar_t),
            &read) ||
        read < sizeof(wchar_t)) {
        return {};
    }
    value.resize(read / sizeof(wchar_t));
    const auto end = value.find(L'\0');
    if (end != std::wstring::npos)
        value.resize(end);
    if (value.empty())
        return {};
    return ToUtf8(value);
}

bool WriteValue(std::uintptr_t address, const void* value, std::size_t size) {
    SIZE_T written = 0;
    return WriteProcessMemory(
               GetCurrentProcess(),
               reinterpret_cast<void*>(address),
               value,
               size,
               &written) &&
           written == size;
}

DWORD CallFrameAnalysis(void* function, void* device) {
    __try {
        reinterpret_cast<void (*)(void*, void*)>(function)(device, nullptr);
        return ERROR_SUCCESS;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

DWORD CallShaderReload(void* function, void* device) {
    __try {
        reinterpret_cast<void (*)(void*, void*)>(function)(device, nullptr);
        return ERROR_SUCCESS;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

CommandResult ErrorResult(
    std::uint32_t status,
    const char* code,
    const std::string& message) {
    std::ostringstream stream;
    stream << "error=" << code << '\n';
    stream << "error_message=" << message << '\n';
    return {status, stream.str()};
}

}  // namespace

void MigotoAdapter::Run(const std::atomic<bool>& stopping) {
    running_ = true;
    start_tick_ms_.store(GetTickCount64(), std::memory_order_release);
    DiagnosticLog::Info("adapter discovery started");

    const auto module_deadline =
        GetTickCount64() + kDeviceCaptureTimeoutMs;
    std::uint64_t capture_deadline = 0;
    bool hooks_installed = false;
    std::string hook_detail;
    capture_state_.store(
        AdapterCaptureState::WaitingForModule,
        std::memory_order_release);
    while (!stopping.load(std::memory_order_acquire)) {
        const HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
        if (d3d11) {
            const auto path = ModulePath(d3d11);
            if (!IsSystemD3D11(path)) {
                const auto image = ModuleImage::Load(d3d11);
                if (image) {
                    wchar_t process_path[32768] = {};
                    GetModuleFileNameW(nullptr, process_path, 32768);
                    const auto wide_dll_path = ModulePath(d3d11);
                    process_path_ = ToUtf8(process_path);
                    dll_path_ = ToUtf8(wide_dll_path);
                    const auto directory =
                        std::filesystem::path(wide_dll_path).parent_path();
                    config_path_ =
                        ToUtf8((directory / L"d3dx.ini").wstring());
                    log_path_ =
                        ToUtf8((directory / L"d3d11_log.txt").wstring());
                    analysis_root_ = ToUtf8(directory.wstring());
                    const auto version = FileVersion(wide_dll_path);
                    std::ostringstream module_details;
                    module_details
                        << "official d3d11.dll loaded: path="
                        << dll_path_
                        << " file_version="
                        << (version.empty() ? "unknown" : version)
                        << " base=0x" << std::hex << image->Base()
                        << " size=0x" << image->Size()
                        << " timestamp=0x" << image->Timestamp()
                        << " checksum=0x" << image->Checksum();
                    DiagnosticLog::Info(module_details.str());
                    std::string hash;
                    std::string hash_error;
                    if (ComputeFileSha256(
                            wide_dll_path, hash, hash_error)) {
                        adapter_module_sha256_ = hash;
                        DiagnosticLog::Info(
                            "official d3d11.dll SHA-256: " + hash);
                    } else {
                        DiagnosticLog::Error(
                            "could not hash official d3d11.dll: " +
                            hash_error);
                    }
                    const auto mcp_path = ModulePathForAddress(
                        reinterpret_cast<const void*>(
                            &ModulePathForAddress));
                    if (!mcp_path.empty()) {
                        if (ComputeFileSha256(
                                mcp_path,
                                mcp_module_sha256_,
                                hash_error)) {
                            DiagnosticLog::Info(
                                "mcp.dll SHA-256: " +
                                mcp_module_sha256_);
                        } else {
                            DiagnosticLog::Error(
                                "could not hash mcp.dll: " +
                                hash_error);
                        }
                    }
                    capabilities_ = ResolveMigotoCapabilities(*image);
                    for (const auto& diagnostic : capabilities_.diagnostics) {
                        if (diagnostic.error)
                            DiagnosticLog::Error(
                                "resolver: " + diagnostic.message);
                        else
                            DiagnosticLog::Info(
                                "resolver: " + diagnostic.message);
                    }
                    DiagnosticLog::Info(
                        "adapter resolved in " +
                        std::string(capabilities_.mode ==
                                            ResolvedAdapterMode::Full
                                        ? "full"
                                        : capabilities_.mode ==
                                                  ResolvedAdapterMode::Partial
                                              ? "partial"
                                              : "read-only") +
                        " mode");

                    PresentHookCallbacks frame_callbacks = {
                        &MigotoAdapter::OnFrame,
                        this,
                    };
                    std::string present_hook_detail;
                    if (!HookRuntime::InstallPresentFunctionHooks(
                            capabilities_.present.available
                                ? reinterpret_cast<void*>(
                                      capabilities_.present.address)
                                : nullptr,
                            capabilities_.present1.available
                                ? reinterpret_cast<void*>(
                                      capabilities_.present1.address)
                                : nullptr,
                            frame_callbacks,
                            present_hook_detail)) {
                        capture_state_.store(
                            AdapterCaptureState::HookFailed,
                            std::memory_order_release);
                        DiagnosticLog::Error(present_hook_detail);
                        break;
                    }
                    AcquireSRWLockExclusive(&lock_);
                    present_hooked_ = true;
                    ReleaseSRWLockExclusive(&lock_);
                    DiagnosticLog::Info(present_hook_detail);
                } else {
                    DiagnosticLog::Error(
                        "ModuleImage failure: " + ModuleImage::LastLoadError());
                }

                DeviceCreateCallbacks callbacks = {
                    &MigotoAdapter::OnDeviceCreated,
                    this,
                };
                capture_state_.store(
                    AdapterCaptureState::WaitingForDevice,
                    std::memory_order_release);
                hooks_installed = HookRuntime::InstallCreateDeviceHooks(
                    callbacks, hook_detail);
                if (hooks_installed) {
                    DiagnosticLog::Info(hook_detail);
                    capture_deadline =
                        GetTickCount64() + kDeviceCaptureTimeoutMs;
                    break;
                }
                capture_state_.store(
                    AdapterCaptureState::HookFailed,
                    std::memory_order_release);
                HookRuntime::RemovePresentFunctionHooks();
                AcquireSRWLockExclusive(&lock_);
                present_hooked_ = false;
                ReleaseSRWLockExclusive(&lock_);
                DiagnosticLog::Error(hook_detail);
                break;
            }
        }

        if (GetTickCount64() >= module_deadline) {
            capture_state_.store(
                AdapterCaptureState::TimedOut,
                std::memory_order_release);
            DiagnosticLog::Error(
                "adapter timed out waiting for official d3d11.dll");
            break;
        }
        Sleep(100);
    }

    while (!stopping.load(std::memory_order_acquire)) {
        if (capture_deadline != 0 &&
            !IsAdapterReady() &&
            GetTickCount64() >= capture_deadline &&
            !capture_timeout_logged_.exchange(
                true, std::memory_order_acq_rel)) {
            const auto state =
                capture_state_.load(std::memory_order_acquire);
            if (state == AdapterCaptureState::WaitingForDevice ||
                state == AdapterCaptureState::WaitingForPresent) {
                capture_state_.store(
                    AdapterCaptureState::TimedOut,
                    std::memory_order_release);
            }
            std::ostringstream message;
            message
                << "adapter timed out waiting for HackerDevice capture; "
                   "monitoring will continue";
            if (const auto* device = HackerDevice()) {
                message << " object=0x" << std::hex
                        << reinterpret_cast<std::uintptr_t>(device);
            }
            DiagnosticLog::Error(message.str());
        }
        Sleep(250);
    }

    Shutdown();
    running_ = false;
}

void MigotoAdapter::Shutdown() {
    HookRuntime::RemovePresentFunctionHooks();
    HookRuntime::RemoveCreateDeviceHooks();
    AcquireSRWLockExclusive(&lock_);
    if (hacker_device_) {
        static_cast<IUnknown*>(hacker_device_)->Release();
        hacker_device_ = nullptr;
    }
    swap_chain_ = nullptr;
    present_hooked_ = false;
    ReleaseSRWLockExclusive(&lock_);
}

ResolvedMigotoCapabilities MigotoAdapter::Capabilities() const {
    AcquireSRWLockShared(&lock_);
    auto capabilities = capabilities_;
    ReleaseSRWLockShared(&lock_);
    return capabilities;
}

void* MigotoAdapter::HackerDevice() const {
    AcquireSRWLockShared(&lock_);
    auto* device = hacker_device_;
    ReleaseSRWLockShared(&lock_);
    return device;
}

IDXGISwapChain* MigotoAdapter::SwapChain() const {
    AcquireSRWLockShared(&lock_);
    auto* swap_chain = swap_chain_;
    ReleaseSRWLockShared(&lock_);
    return swap_chain;
}

std::uint64_t MigotoAdapter::FrameCount() const {
    return frame_count_.load(std::memory_order_relaxed);
}

bool MigotoAdapter::QueueCommand(
    std::uint32_t request_id,
    ControlCommand command,
    std::string& detail) {
    if (!IsAdapterReady()) {
        detail =
            std::string("adapter is not ready: ") +
            AdapterCaptureStateName(
                capture_state_.load(std::memory_order_acquire));
        return false;
    }
    const auto capabilities = Capabilities();
    const bool available =
        command == ControlCommand::LiveContext ||
                command == ControlCommand::RuntimeVariables
            ? true
            : command == ControlCommand::LiveDrawSnapshot
                  ? false
                  : command == ControlCommand::FrameAnalysis
            ? capabilities.frame_analysis.available
            : command == ControlCommand::ReloadConfig
                  ? capabilities.reload_config.available &&
                        capabilities.globals.has_value() &&
                        capabilities.reload_config_pending_offset != 0
                  : capabilities.reload_shaders.available;
    if (!available) {
        detail =
            command == ControlCommand::LiveDrawSnapshot
                ? "live probe capability is unavailable"
                : command == ControlCommand::FrameAnalysis
                ? "frame_analysis internal callback was not resolved"
                : command == ControlCommand::ReloadConfig
                      ? "reload_config pending field was not resolved "
                        "or validated"
                      : "reload_shaders internal callback was not resolved";
        return false;
    }
    return command_queue_.TryQueue(request_id, command, detail);
}

QueueWaitResult MigotoAdapter::WaitCommand(
    std::uint32_t request_id,
    std::uint32_t timeout_ms,
    CommandResult& result) {
    return command_queue_.Wait(request_id, timeout_ms, result);
}

#ifdef MCP_TESTING
void MigotoAdapter::SetTestRuntime(
    const ResolvedMigotoCapabilities& capabilities,
    void* device) {
    AcquireSRWLockExclusive(&lock_);
    capabilities_ = capabilities;
    hacker_device_ = device;
    ReleaseSRWLockExclusive(&lock_);
}

void MigotoAdapter::SetTestCaptureState(
    bool device_captured,
    bool present_hooked,
    AdapterCaptureState state) {
    AcquireSRWLockExclusive(&lock_);
    if (device_captured && !hacker_device_)
        hacker_device_ = reinterpret_cast<void*>(0x1234);
    else if (!device_captured)
        hacker_device_ = nullptr;
    present_hooked_ = present_hooked;
    ReleaseSRWLockExclusive(&lock_);
    capture_state_.store(state, std::memory_order_release);
}

void MigotoAdapter::ProcessPendingForTests() {
    ExecutePendingCommand();
}
#endif

bool MigotoAdapter::IsAdapterReady() const {
    AcquireSRWLockShared(&lock_);
    const bool ready = present_hooked_ && hacker_device_ != nullptr;
    ReleaseSRWLockShared(&lock_);
    return ready;
}

AdapterSnapshot MigotoAdapter::Snapshot() const {
    AdapterSnapshot snapshot;
    ResolvedMigotoCapabilities capabilities;
    IDXGISwapChain* swap_chain = nullptr;
    void* hacker_device = nullptr;
    bool present_hooked = false;
    {
        AcquireSRWLockShared(&lock_);
        capabilities = capabilities_;
        swap_chain = swap_chain_;
        hacker_device = hacker_device_;
        present_hooked = present_hooked_;
        snapshot.process_path = process_path_;
        snapshot.dll_path = dll_path_;
        snapshot.config_path = config_path_;
        snapshot.log_path = log_path_;
        snapshot.analysis_root = analysis_root_;
        ReleaseSRWLockShared(&lock_);
    }

    snapshot.device_captured = hacker_device != nullptr;
    snapshot.present_hooked = present_hooked;
    snapshot.adapter_ready = snapshot.device_captured && present_hooked;
    snapshot.capture_state =
        capture_state_.load(std::memory_order_acquire);
    if (snapshot.capture_state == AdapterCaptureState::Ready &&
        !snapshot.adapter_ready) {
        snapshot.capture_state =
            snapshot.device_captured
                ? AdapterCaptureState::WaitingForPresent
                : AdapterCaptureState::WaitingForDevice;
    }

    if (snapshot.adapter_ready) {
        snapshot.mode =
            capabilities.mode == ResolvedAdapterMode::Full
                ? AdapterSnapshotMode::Full
                : capabilities.mode == ResolvedAdapterMode::Partial
                      ? AdapterSnapshotMode::Partial
                      : AdapterSnapshotMode::ReadOnly;
        if (capabilities.frame_analysis.available)
            snapshot.capabilities.emplace_back("frame_analysis");
        if (capabilities.reload_config.available &&
            capabilities.globals.has_value() &&
            capabilities.reload_config_pending_offset != 0)
            snapshot.capabilities.emplace_back("reload_config");
        if (capabilities.reload_shaders.available)
            snapshot.capabilities.emplace_back("reload_shaders");
        snapshot.capabilities.emplace_back("live_context");
        snapshot.capabilities.emplace_back("runtime_variables");
    }

    snapshot.frame = frame_count_.load(std::memory_order_relaxed);
    snapshot.fps = fps_.load(std::memory_order_relaxed);
    snapshot.frame_time_ms =
        frame_time_ms_.load(std::memory_order_relaxed);
    const auto started = start_tick_ms_.load(std::memory_order_acquire);
    if (started)
        snapshot.uptime_s = (GetTickCount64() - started) / 1000.0;
    snapshot.adapter_error_count = DiagnosticLog::ErrorCount();
    snapshot.adapter_log_path = ToUtf8(DiagnosticLog::Path().wstring());
    snapshot.adapter_module_sha256 = adapter_module_sha256_;
    snapshot.capture_restore_hunting =
        capture_restore_hunting_.load(std::memory_order_acquire);

    if (swap_chain) {
        DXGI_SWAP_CHAIN_DESC description = {};
        if (SUCCEEDED(swap_chain->GetDesc(&description))) {
            snapshot.screen_width = description.BufferDesc.Width;
            snapshot.screen_height = description.BufferDesc.Height;
        }
    }

    if (capabilities.globals.has_value()) {
        const auto base = *capabilities.globals;
        snapshot.globals_resolved = true;
        if (const auto hunting =
                ReadValue<std::uint32_t>(
                    base + capabilities.hunting_offset)) {
            snapshot.hunting = *hunting;
        }
        if (const auto active =
                ReadValue<std::uint8_t>(
                    base + capabilities.analyse_frame_offset)) {
            snapshot.analysis_active = *active != 0;
        }
        if (const auto number =
                ReadValue<std::uint32_t>(
                    base + capabilities.analyse_frame_offset + 4)) {
            snapshot.analysis_number = *number;
        }
        snapshot.analysis_path = ReadWideString(
            base + capabilities.analyse_frame_offset +
                kAnalysisPathOffset,
            260);
        snapshot.shader_path =
            ReadWideString(base + kShaderPathOffset, 512);
    }
    return snapshot;
}

void MigotoAdapter::OnDeviceCreated(
    void* context,
    ID3D11Device* device,
    IDXGISwapChain* swap_chain) {
    static_cast<MigotoAdapter*>(context)->HandleDeviceCreated(
        device, swap_chain);
}

void MigotoAdapter::OnFrame(
    void* context,
    IDXGISwapChain* swap_chain) {
    static_cast<MigotoAdapter*>(context)->HandleFrame(swap_chain);
}

void MigotoAdapter::HandleDeviceCreated(
    ID3D11Device* device,
    IDXGISwapChain* swap_chain) {
    if (!device) {
        DiagnosticLog::Error("device-created callback received a null device");
        return;
    }

    IUnknown* hacker_device = nullptr;
    const auto query = device->QueryInterface(
        kHackerDeviceIid,
        reinterpret_cast<void**>(&hacker_device));
    if (FAILED(query) || !hacker_device) {
        DiagnosticLog::Warn(
            "IID_HackerDevice query failed; device capture skipped");
        return;
    }

    AcquireSRWLockExclusive(&lock_);
    if (hacker_device_) {
        static_cast<IUnknown*>(hacker_device_)->Release();
        hacker_device_ = nullptr;
    }
    hacker_device_ = hacker_device;
    if (swap_chain)
        swap_chain_ = swap_chain;
    ReleaseSRWLockExclusive(&lock_);
    capture_state_.store(
        present_hooked_
            ? AdapterCaptureState::Ready
            : AdapterCaptureState::WaitingForPresent,
        std::memory_order_release);

    DiagnosticLog::Info(
        "captured HackerDevice at 0x" +
        [] (const void* pointer) {
            std::ostringstream stream;
            stream << std::hex << reinterpret_cast<std::uintptr_t>(pointer);
            return stream.str();
        }(hacker_device));
    if (capture_timeout_logged_.load(std::memory_order_acquire)) {
        DiagnosticLog::Info(
            "HackerDevice capture recovered after the timeout");
    }

}

void MigotoAdapter::HandleFrame(IDXGISwapChain*) {
    const auto restore_mode =
        capture_restore_hunting_.load(std::memory_order_acquire);
    if (restore_mode >= 0) {
        const auto capabilities = Capabilities();
        if (capabilities.globals) {
            const auto active = ReadValue<std::uint8_t>(
                *capabilities.globals + capabilities.analyse_frame_offset);
            if (active && *active == 0) {
                const auto hunting = static_cast<std::uint32_t>(
                    restore_mode);
                if (WriteValue(
                        *capabilities.globals +
                            capabilities.hunting_offset,
                        &hunting,
                        sizeof(hunting))) {
                    DiagnosticLog::Info(
                        "restored hunting mode after frame analysis");
                    capture_restore_hunting_.store(
                        -1, std::memory_order_release);
                    capture_restore_failure_logged_.store(
                        false, std::memory_order_release);
                } else {
                    if (!capture_restore_failure_logged_.exchange(
                            true, std::memory_order_acq_rel)) {
                        DiagnosticLog::Error(
                            "failed to restore hunting mode after frame "
                            "analysis");
                    }
                }
            }
        }
    }

    ExecutePendingCommand();
    const auto now = GetTickCount64();
    const auto previous =
        last_frame_tick_ms_.exchange(now, std::memory_order_acq_rel);
    if (previous != 0 && now > previous) {
        const double milliseconds = static_cast<double>(now - previous);
        frame_time_ms_.store(milliseconds, std::memory_order_relaxed);
        fps_.store(1000.0 / milliseconds, std::memory_order_relaxed);
    }
    frame_count_.fetch_add(1, std::memory_order_relaxed);
}

CommandResult MigotoAdapter::CaptureLiveContextCommand(
    std::uint32_t request_id,
    void* hacker_device) {
    const auto directory = ProbeSnapshotDirectory(analysis_root_);
    if (directory.empty()) {
        return ErrorResult(
            4,
            "snapshot_directory_unavailable",
            "analysis root is unavailable");
    }

    ID3D11Device1* device = nullptr;
    const HRESULT query = static_cast<IUnknown*>(hacker_device)
                              ->QueryInterface(
                                  __uuidof(ID3D11Device1),
                                  reinterpret_cast<void**>(&device));
    if (FAILED(query) || !device) {
        return ErrorResult(
            4,
            "d3d11_device_unavailable",
            "HackerDevice does not expose ID3D11Device1");
    }

    LiveProbeSnapshot snapshot;
    snapshot.kind = "live_context";
    snapshot.frame =
        frame_count_.load(std::memory_order_relaxed) + 1;
    snapshot.captured_unix_ms =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    snapshot.pid = GetCurrentProcessId();
    snapshot.d3d11_sha256 = adapter_module_sha256_;
    snapshot.mcp_sha256 = mcp_module_sha256_;
    snapshot.process_path = process_path_;
    snapshot.dll_path = dll_path_;
    snapshot.config_path = config_path_;
    snapshot.log_path = log_path_;
    snapshot.analysis_root = analysis_root_;

    const auto current = Snapshot();
    snapshot.hunting = current.hunting;
    snapshot.analysis_active = current.analysis_active;
    if (snapshot.d3d11_sha256.empty())
        snapshot.missing_fields.emplace_back("d3d11_sha256");
    if (snapshot.mcp_sha256.empty())
        snapshot.missing_fields.emplace_back("mcp_sha256");

    std::string capture_error;
    const bool captured = CaptureImmediateContext(
        device, snapshot, capture_error);
    device->Release();
    if (!captured) {
        DiagnosticLog::Error(
            "live-context capture failed: " + capture_error);
        return ErrorResult(
            5,
            "live_context_capture_failed",
            capture_error);
    }

    const auto snapshot_directory = Utf8Path(directory);
    const auto filename =
        "live-context-" + std::to_string(snapshot.frame) + "-" +
        std::to_string(request_id) + ".json";
    const auto snapshot_path =
        snapshot_directory / Utf8Path(filename);
    std::string write_error;
    if (!WriteProbeSnapshot(
            snapshot_path, snapshot, write_error)) {
        DiagnosticLog::Error(
            "live-context snapshot write failed: " + write_error);
        return ErrorResult(
            5,
            "snapshot_write_failed",
            write_error);
    }

    std::string retention_error;
    if (!RetainNewestProbeSnapshots(
            snapshot_directory, 20, retention_error)) {
        DiagnosticLog::Warn(
            "live-context snapshot retention failed: " +
            retention_error);
    }

    std::ostringstream stream;
    stream << "command=live_context\n";
    stream << "result=captured\n";
    stream << "snapshot_path=" << Utf8String(snapshot_path) << '\n';
    stream << "frame=" << snapshot.frame << '\n';
    stream << "shader_count=" << snapshot.shaders.size() << '\n';
    stream << "resource_count=" << snapshot.resources.size() << '\n';
    stream << "state_count=" << snapshot.states.size() << '\n';
    stream << "missing_field_count="
           << snapshot.missing_fields.size() << '\n';
    stream << "limit_count=" << snapshot.limits_hit.size() << '\n';
    if (!retention_error.empty())
        stream << "retention_error=" << retention_error << '\n';
    return {0, stream.str()};
}

CommandResult MigotoAdapter::CaptureRuntimeVariablesCommand(
    std::uint32_t request_id) {
    if (analysis_root_.empty()) {
        return ErrorResult(
            4,
            "runtime_variables_unavailable",
            "analysis root is unavailable");
    }

    RuntimeConfigSnapshot snapshot;
    std::string read_error;
    if (!ReadRuntimeConfig(
            Utf8Path(analysis_root_), snapshot, read_error)) {
        return ErrorResult(
            5,
            "runtime_variables_read_failed",
            read_error);
    }

    snapshot.frame =
        frame_count_.load(std::memory_order_relaxed) + 1;
    snapshot.captured_unix_ms =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    snapshot.pid = GetCurrentProcessId();
    snapshot.d3d11_sha256 = adapter_module_sha256_;
    snapshot.mcp_sha256 = mcp_module_sha256_;
    snapshot.process_path = process_path_;
    snapshot.dll_path = dll_path_;
    const auto current = Snapshot();
    snapshot.hunting = current.hunting;
    snapshot.analysis_active = current.analysis_active;
    if (snapshot.d3d11_sha256.empty())
        snapshot.missing_fields.emplace_back("d3d11_sha256");
    if (snapshot.mcp_sha256.empty())
        snapshot.missing_fields.emplace_back("mcp_sha256");

    const auto directory = ProbeSnapshotDirectory(analysis_root_);
    if (directory.empty()) {
        return ErrorResult(
            4,
            "snapshot_directory_unavailable",
            "analysis root is unavailable");
    }
    const auto snapshot_directory = Utf8Path(directory);
    const auto filename =
        "runtime-variables-" + std::to_string(snapshot.frame) + "-" +
        std::to_string(request_id) + ".json";
    const auto snapshot_path =
        snapshot_directory / Utf8Path(filename);
    std::string write_error;
    if (!WriteRuntimeVariables(
            snapshot_path, snapshot, write_error)) {
        DiagnosticLog::Error(
            "runtime-variables snapshot write failed: " +
            write_error);
        return ErrorResult(
            5,
            "snapshot_write_failed",
            write_error);
    }

    std::string retention_error;
    if (!RetainNewestProbeSnapshots(
            snapshot_directory, 20, retention_error)) {
        DiagnosticLog::Warn(
            "runtime-variables snapshot retention failed: " +
            retention_error);
    }

    std::ostringstream stream;
    stream << "command=runtime_variables\n";
    stream << "result=captured\n";
    stream << "snapshot_path=" << Utf8String(snapshot_path) << '\n';
    stream << "frame=" << snapshot.frame << '\n';
    stream << "persistent_value_count="
           << snapshot.persistent_values.size() << '\n';
    stream << "unknown_setting_count="
           << snapshot.log_summary.unknown_setting_count << '\n';
    stream << "warning_count=" << snapshot.warnings.size() << '\n';
    if (!retention_error.empty())
        stream << "retention_error=" << retention_error << '\n';
    return {0, stream.str()};
}

void MigotoAdapter::ExecutePendingCommand() {
    const auto request = command_queue_.Claim();
    if (!request)
        return;

    auto capabilities = Capabilities();
    auto* device = HackerDevice();
    if (!device) {
        command_queue_.Complete(ErrorResult(
            4, "device_not_ready", "HackerDevice is not captured"));
        return;
    }

    CommandResult result;
    if (request->command == ControlCommand::FrameAnalysis) {
        if (!capabilities.frame_analysis.available ||
            !capabilities.globals) {
            result = ErrorResult(
                4,
                "frame_analysis_unavailable",
                "frame-analysis callback or Globals is unavailable");
        } else {
            const auto previous_hunting = ReadValue<std::uint32_t>(
                *capabilities.globals + capabilities.hunting_offset);
            if (!previous_hunting || *previous_hunting > 2) {
                result = ErrorResult(
                    5,
                    "frame_analysis_hunting_unreadable",
                    "could not read the previous hunting mode");
                command_queue_.Complete(result);
                return;
            }

            const bool hunting_changed = *previous_hunting != 1;
            if (hunting_changed) {
                const std::uint32_t enabled = 1;
                if (!WriteValue(
                        *capabilities.globals + capabilities.hunting_offset,
                        &enabled,
                        sizeof(enabled))) {
                    result = ErrorResult(
                        5,
                        "frame_analysis_hunting_enable_failed",
                        "could not enable hunting before frame analysis");
                    command_queue_.Complete(result);
                    return;
                }
            }

            const auto exception = CallFrameAnalysis(
                reinterpret_cast<void*>(capabilities.frame_analysis.address),
                device);
            if (exception == ERROR_SUCCESS) {
                const auto active = ReadValue<std::uint8_t>(
                    *capabilities.globals +
                        capabilities.analyse_frame_offset);
                const auto analysis_path = ReadWideString(
                    *capabilities.globals +
                        capabilities.analyse_frame_offset +
                        kAnalysisPathOffset,
                    260);
                if (!active || *active == 0 || analysis_path.empty()) {
                    if (hunting_changed) {
                        const auto hunting = *previous_hunting;
                        if (!WriteValue(
                                *capabilities.globals +
                                    capabilities.hunting_offset,
                                &hunting,
                                sizeof(hunting))) {
                            result = ErrorResult(
                                5,
                                "frame_analysis_hunting_restore_failed",
                                "analysis was not accepted and hunting "
                                "could not be restored");
                            command_queue_.Complete(result);
                            return;
                        }
                    }
                    std::ostringstream message;
                    message << "analysis_active=";
                    if (active)
                        message << static_cast<unsigned>(*active);
                    else
                        message << "unreadable";
                    message << " analysis_path_empty="
                            << (analysis_path.empty() ? 1 : 0)
                            << " analyse_options=unresolved";
                    result = ErrorResult(
                        5,
                        "frame_analysis_not_accepted",
                        message.str());
                } else {
                    const int restore_mode =
                        hunting_changed
                            ? static_cast<int>(*previous_hunting)
                            : -1;
                    capture_restore_hunting_.store(
                        restore_mode, std::memory_order_release);
                    capture_restore_failure_logged_.store(
                        false, std::memory_order_release);
                    std::ostringstream stream;
                    stream << "command=frame_analysis\n";
                    stream << "result=accepted\n";
                    stream << "analysis_path=" << analysis_path << '\n';
                    stream << "analyse_options=unresolved\n";
                    stream << "hunting_restore_mode=" << restore_mode
                           << '\n';
                    result = {0, stream.str()};
                }
            } else {
                if (hunting_changed) {
                    const auto hunting = *previous_hunting;
                    if (!WriteValue(
                            *capabilities.globals +
                                capabilities.hunting_offset,
                            &hunting,
                            sizeof(hunting))) {
                        result = ErrorResult(
                            5,
                            "frame_analysis_hunting_restore_failed",
                            "callback failed and hunting could not be "
                            "restored");
                        command_queue_.Complete(result);
                        return;
                    }
                }
                result = ErrorResult(
                    5,
                    "frame_analysis_exception",
                    std::to_string(exception));
            }
        }
    } else if (request->command == ControlCommand::LiveContext) {
        result = CaptureLiveContextCommand(
            request->request_id, device);
    } else if (request->command == ControlCommand::RuntimeVariables) {
        result = CaptureRuntimeVariablesCommand(
            request->request_id);
    } else if (request->command == ControlCommand::LiveDrawSnapshot) {
        result = ErrorResult(
            4,
            "live_draw_unavailable",
            "live draw capture is not available in this build");
    } else if (request->command == ControlCommand::ReloadConfig) {
        if (!capabilities.reload_config.available ||
            !capabilities.globals ||
            capabilities.reload_config_pending_offset == 0) {
            result = ErrorResult(
                4,
                "reload_config_unavailable",
                "official config-reload pending field is unavailable");
        } else {
            constexpr std::uint8_t kPending = 1;
            if (!WriteValue(
                    *capabilities.globals +
                        capabilities.reload_config_pending_offset,
                    &kPending,
                    sizeof(kPending))) {
                result = ErrorResult(
                    5,
                    "reload_config_pending_write_failed",
                    "could not set the official config-reload "
                    "pending field");
            } else {
                result = {
                    0,
                    "command=reload_config\n"
                    "result=queued\n"
                    "processing=official_present\n"};
            }
        }
    } else {
        if (!capabilities.reload_shaders.available) {
            result = ErrorResult(
                4,
                "reload_shaders_unavailable",
                "ReloadFixes callback is unavailable");
        } else {
            const auto exception = CallShaderReload(
                reinterpret_cast<void*>(capabilities.reload_shaders.address),
                device);
            result = exception == ERROR_SUCCESS
                         ? CommandResult{
                               0,
                               "command=reload_shaders\nresult=ok\n"}
                         : ErrorResult(
                               5,
                               "reload_shaders_exception",
                               std::to_string(exception));
        }
    }
    command_queue_.Complete(result);
}

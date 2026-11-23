#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class AdapterSnapshotMode {
    ReadOnly,
    Partial,
    Full,
};

enum class AdapterCaptureState {
    NotStarted,
    WaitingForModule,
    WaitingForDevice,
    WaitingForPresent,
    Ready,
    TimedOut,
    PresentMismatch,
    HookFailed,
};

inline const char* AdapterCaptureStateName(AdapterCaptureState state) {
    switch (state) {
    case AdapterCaptureState::NotStarted:
        return "not_started";
    case AdapterCaptureState::WaitingForModule:
        return "waiting_for_module";
    case AdapterCaptureState::WaitingForDevice:
        return "waiting_for_device";
    case AdapterCaptureState::WaitingForPresent:
        return "waiting_for_present";
    case AdapterCaptureState::Ready:
        return "ready";
    case AdapterCaptureState::TimedOut:
        return "timed_out";
    case AdapterCaptureState::PresentMismatch:
        return "present_mismatch";
    case AdapterCaptureState::HookFailed:
        return "hook_failed";
    }
    return "unknown";
}

struct AdapterSnapshot {
    AdapterSnapshotMode mode = AdapterSnapshotMode::ReadOnly;
    bool adapter_ready = false;
    bool device_captured = false;
    bool present_hooked = false;
    bool globals_resolved = false;
    AdapterCaptureState capture_state = AdapterCaptureState::NotStarted;
    std::vector<std::string> capabilities;
    std::uint64_t frame = 0;
    double fps = 0.0;
    double frame_time_ms = 0.0;
    double uptime_s = 0.0;
    std::string process_path;
    std::string dll_path;
    std::string config_path;
    std::string log_path;
    std::string analysis_root;
    std::string analysis_path;
    std::string shader_path;
    std::optional<std::uint32_t> hunting;
    std::optional<bool> analysis_active;
    std::optional<std::uint32_t> analysis_number;
    std::uint32_t screen_width = 0;
    std::uint32_t screen_height = 0;
    std::uint64_t adapter_error_count = 0;
    std::string adapter_log_path;
    std::string adapter_module_sha256;
    int capture_restore_hunting = -1;
};

enum class ControlCommand : std::uint16_t {
    FrameAnalysis = 2,
    ReloadConfig = 3,
    ReloadShaders = 4,
    LiveContext = 5,
    RuntimeVariables = 6,
    LiveDrawSnapshot = 7,
};

struct CommandResult {
    std::uint32_t status = 0;
    std::string payload;
};

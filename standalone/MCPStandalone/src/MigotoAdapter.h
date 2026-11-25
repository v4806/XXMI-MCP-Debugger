#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "AdapterTypes.h"
#include "CommandQueue.h"
#include "DrawProbe.h"
#include "MigotoSignatures.h"

class MigotoAdapter {
public:
    void Run(const std::atomic<bool>& stopping);
    void Shutdown();

    ResolvedMigotoCapabilities Capabilities() const;
    void* HackerDevice() const;
    IDXGISwapChain* SwapChain() const;
    std::uint64_t FrameCount() const;
    AdapterSnapshot Snapshot() const;
    bool QueueCommand(
        std::uint32_t request_id,
        ControlCommand command,
        std::string& detail);
    QueueWaitResult WaitCommand(
        std::uint32_t request_id,
        std::uint32_t timeout_ms,
        CommandResult& result);

#ifdef MCP_TESTING
    void SetTestRuntime(
        const ResolvedMigotoCapabilities& capabilities,
        void* device);
    void SetTestCaptureState(
        bool device_captured,
        bool present_hooked,
        AdapterCaptureState state);
    void ProcessPendingForTests();
    void ProcessPostPresentForTests();
#endif

private:
    static void OnDeviceCreated(
        void* context,
        ID3D11Device* device,
        IDXGISwapChain* swap_chain);
    static void OnFrame(void* context, IDXGISwapChain* swap_chain);
    static void OnPostFrame(void* context, IDXGISwapChain* swap_chain);

    void HandleDeviceCreated(
        ID3D11Device* device,
        IDXGISwapChain* swap_chain);
    void HandleFrame(IDXGISwapChain* swap_chain);
    void HandlePostFrame(IDXGISwapChain* swap_chain);
    void AdoptPresentingDevice(IDXGISwapChain* swap_chain);
    void RememberDeviceLocked(void* hacker_device);
    void RememberDrawingContext(void* drawing_context);
    ID3D11DeviceContext1* SelectActiveContext();
    void MeasureScreenSize(IDXGISwapChain* swap_chain);
    void ExecutePendingCommand(bool post_present);
    void ExecuteCommand(const QueuedCommand* request);
    CommandResult CaptureLiveContextCommand(
        std::uint32_t request_id,
        void* hacker_device);
    CommandResult CaptureRuntimeVariablesCommand(
        std::uint32_t request_id);
    bool StartLiveDrawProbe(
        std::uint32_t request_id,
        void* hacker_device,
        std::string& error);
    void ProcessDrawProbeAtPresent(std::uint32_t frame);
    static bool CaptureDrawProbeState(
        void* context,
        void* drawing_context,
        LiveProbeSnapshot& snapshot,
        std::string& error);
    bool IsAdapterReady() const;

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    void* hacker_device_ = nullptr;
    std::vector<void*> captured_devices_;
    IDXGISwapChain* swap_chain_ = nullptr;
    ResolvedMigotoCapabilities capabilities_;
    std::atomic<std::uint64_t> frame_count_{0};
    std::atomic<double> fps_{0.0};
    std::atomic<double> frame_time_ms_{0.0};
    std::atomic<std::uint64_t> start_tick_ms_{0};
    std::atomic<std::uint64_t> last_frame_tick_ms_{0};
    CommandQueue command_queue_;
    // FrameAnalysis is claimed during the pre-present seam but only executed
    // after the hooked Present returns, because 3DMigoto's RunFrameActions()
    // clears G->analyse_frame inside the same Present call.
    std::optional<QueuedCommand> deferred_frame_analysis_;
    std::atomic<int> capture_restore_hunting_{-1};
    std::atomic<AdapterCaptureState> capture_state_{
        AdapterCaptureState::NotStarted};
    std::atomic<bool> capture_timeout_logged_{false};
    std::atomic<bool> capture_restore_failure_logged_{false};
    std::string process_path_;
    std::string dll_path_;
    std::string config_path_;
    std::string log_path_;
    std::string analysis_root_;
    std::string adapter_module_sha256_;
    std::string mcp_module_sha256_;
    NativeBeforeDrawHook before_draw_hook_;
    BeforeDrawProbeController* draw_probe_ = nullptr;
    ID3D11Device1* draw_probe_device_ = nullptr;
    ID3D11DeviceContext1* draw_probe_context_ = nullptr;
    ID3D11DeviceContext1* drawing_context_ = nullptr;
    std::uint32_t draw_probe_request_id_ = 0;
    std::atomic<bool> draw_probe_active_{false};
    std::atomic<bool> live_draw_available_{false};
    std::atomic<std::uint32_t> screen_width_{0};
    std::atomic<std::uint32_t> screen_height_{0};
    std::atomic<bool> screen_size_failure_logged_{false};
    bool present_hooked_ = false;
    bool running_ = false;
};

#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <string>

class ModuleImage;

struct PresentHookCallbacks {
    void (*on_frame)(void* context, IDXGISwapChain* swap_chain) = nullptr;
    // Runs after the hooked Present/Present1 returns, i.e. after the official
    // HackerSwapChain::Present ran RunFrameActions(). Frame-analysis arming
    // must happen here: RunFrameActions() clears G->analyse_frame at the end
    // of the same Present, so arming before it would stop the analysis before
    // a single draw is recorded.
    void (*on_post_frame)(void* context, IDXGISwapChain* swap_chain) = nullptr;
    void* context = nullptr;
};

struct DeviceCreateCallbacks {
    void (*on_device_created)(void* context,
                              ID3D11Device* device,
                              IDXGISwapChain* swap_chain) = nullptr;
    void* context = nullptr;
};

struct PresentObjectCandidate {
    IDXGISwapChain* swap_chain = nullptr;
    std::size_t field_offset = 0;
};

class HookRuntime {
public:
    static bool InstallCreateDeviceHooks(
        const DeviceCreateCallbacks& callbacks,
        std::string& detail);
    static void RemoveCreateDeviceHooks();

    static bool InstallPresentHookForObject(
        IDXGISwapChain* swap_chain,
        const PresentHookCallbacks& callbacks,
        const ModuleImage& official_module,
        std::string& detail);
    static bool InstallPresentFunctionHooks(
        void* present_target,
        void* present1_target,
        const PresentHookCallbacks& callbacks,
        std::string& detail);
    static void RemovePresentFunctionHooks();
    static bool FindPresentObjectInMemory(
        void* container,
        std::size_t scan_size,
        const ModuleImage& official_module,
        PresentObjectCandidate& candidate,
        std::string& detail);

    static void* GetPresentTestContext();
    static void SetPresentTestContext(void* context);
    static void ResetPresentHooksForTests();

private:
    HookRuntime() = delete;
};

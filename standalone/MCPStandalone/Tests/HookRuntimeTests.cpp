#include <Windows.h>

#include <array>
#include <cstdint>
#include <string>

#include "HookRuntime.h"
#include "ModuleImage.h"
#include "TestHarness.h"

namespace {

constexpr std::size_t kPresentSlot = 8;
constexpr std::size_t kPresent1Slot = 22;
constexpr std::size_t kVtableSlots = 64;

struct FakeSwapChain {
    void** object_vptr;
};

struct FakeHackerDevice {
    void* leading_fields[5] = {};
    IDXGISwapChain* swap_chain = nullptr;
};

struct HookProbe {
    int frames = 0;
    int post_frames = 0;
    int original_present = 0;
    int original_present1 = 0;
    int frames_before_original = -1;
    int posts_after_original = -1;
};

__declspec(noinline) HRESULT STDMETHODCALLTYPE OriginalPresentFunction(
    IDXGISwapChain*,
    UINT,
    UINT) {
    auto* probe =
        static_cast<HookProbe*>(HookRuntime::GetPresentTestContext());
    ++probe->original_present;
    return S_OK;
}

__declspec(noinline) HRESULT STDMETHODCALLTYPE OriginalPresent1Function(
    IDXGISwapChain1*,
    UINT,
    UINT,
    const DXGI_PRESENT_PARAMETERS*) {
    auto* probe =
        static_cast<HookProbe*>(HookRuntime::GetPresentTestContext());
    ++probe->original_present1;
    return S_OK;
}

struct PresentHookReset {
    ~PresentHookReset() {
        HookRuntime::RemovePresentFunctionHooks();
        HookRuntime::ResetPresentHooksForTests();
        HookRuntime::SetPresentTestContext(nullptr);
    }
};

HRESULT STDMETHODCALLTYPE OriginalPresent(
    IDXGISwapChain*,
    UINT,
    UINT) {
    if (GetCurrentThreadId() == 0)
        return E_FAIL;
    auto* probe =
        static_cast<HookProbe*>(HookRuntime::GetPresentTestContext());
    ++probe->original_present;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE OriginalPresent1(
    IDXGISwapChain1*,
    UINT,
    UINT,
    const DXGI_PRESENT_PARAMETERS*) {
    if (GetCurrentThreadId() == 0)
        return E_FAIL;
    auto* probe =
        static_cast<HookProbe*>(HookRuntime::GetPresentTestContext());
    ++probe->original_present1;
    return S_OK;
}

void STDMETHODCALLTYPE FrameCallback(void* context, IDXGISwapChain*) {
    auto* probe = static_cast<HookProbe*>(context);
    ++probe->frames;
    probe->frames_before_original = probe->original_present;
}

void STDMETHODCALLTYPE PostFrameCallback(void* context, IDXGISwapChain*) {
    auto* probe = static_cast<HookProbe*>(context);
    ++probe->post_frames;
    probe->posts_after_original = probe->original_present;
}

void FillVtable(std::array<void*, kVtableSlots + 1>& table) {
    table[0] = &table[0];
    for (std::size_t index = 1; index < table.size(); ++index)
        table[index] = reinterpret_cast<void*>(0x10000 + index);
    table[kPresentSlot + 1] = reinterpret_cast<void*>(&OriginalPresent);
    table[kPresent1Slot + 1] = reinterpret_cast<void*>(&OriginalPresent1);
}

}  // namespace

TEST_CASE(PresentVtableHookCallsThroughAndNotifiesFrameCallback) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    REQUIRE(image.has_value());
    std::array<void*, kVtableSlots + 1> original_vtable = {};
    FillVtable(original_vtable);
    FakeSwapChain object = {&original_vtable[1]};
    HookProbe probe;
    PresentHookCallbacks callbacks = {&FrameCallback, nullptr, &probe};
    PresentHookReset reset;

    HookRuntime::ResetPresentHooksForTests();
    HookRuntime::SetPresentTestContext(&probe);

    std::string detail;
    REQUIRE(HookRuntime::InstallPresentHookForObject(
        reinterpret_cast<IDXGISwapChain*>(&object),
        callbacks,
        *image,
        detail));

    auto* current_vtable = object.object_vptr;
    REQUIRE(current_vtable != &original_vtable[1]);
    REQUIRE_EQ(reinterpret_cast<void*>(current_vtable[-1]),
               original_vtable[0]);
    REQUIRE_EQ(reinterpret_cast<void*>(current_vtable[3]),
               original_vtable[4]);

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
        IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    const auto present = reinterpret_cast<PresentFn>(current_vtable[kPresentSlot]);
    const auto present1 =
        reinterpret_cast<Present1Fn>(current_vtable[kPresent1Slot]);
    REQUIRE_EQ(present(reinterpret_cast<IDXGISwapChain*>(&object), 1, 0), S_OK);
    REQUIRE_EQ(present1(reinterpret_cast<IDXGISwapChain1*>(&object),
                        1, 0, nullptr),
               S_OK);
    REQUIRE_EQ(probe.frames, 2);
    REQUIRE_EQ(probe.original_present, 1);
    REQUIRE_EQ(probe.original_present1, 1);
}

TEST_CASE(PresentVtableHookRejectsNullObject) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    REQUIRE(image.has_value());
    HookProbe probe;
    PresentHookCallbacks callbacks = {&FrameCallback, nullptr, &probe};
    PresentHookReset reset;
    std::string detail;
    REQUIRE(!HookRuntime::InstallPresentHookForObject(
        nullptr,
        callbacks,
        *image,
        detail));
    REQUIRE(!detail.empty());
}

TEST_CASE(PresentVtableHookRejectsUnreadableObjectVptr) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    REQUIRE(image.has_value());
    FakeSwapChain object = {reinterpret_cast<void**>(0x1)};
    HookProbe probe;
    PresentHookCallbacks callbacks = {&FrameCallback, nullptr, &probe};
    PresentHookReset reset;
    std::string detail;

    REQUIRE(!HookRuntime::InstallPresentHookForObject(
        reinterpret_cast<IDXGISwapChain*>(&object),
        callbacks,
        *image,
        detail));
    REQUIRE(detail.find("reason=") != std::string::npos);
    REQUIRE(detail.find("object=") != std::string::npos);
}

TEST_CASE(PresentVtableHookRejectsMissingRttiStorage) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    REQUIRE(image.has_value());
    std::array<void*, kVtableSlots + 1> original_vtable = {};
    FillVtable(original_vtable);
    original_vtable[0] = nullptr;
    FakeSwapChain object = {&original_vtable[1]};
    HookProbe probe;
    PresentHookCallbacks callbacks = {&FrameCallback, nullptr, &probe};
    PresentHookReset reset;
    std::string detail;

    REQUIRE(!HookRuntime::InstallPresentHookForObject(
        reinterpret_cast<IDXGISwapChain*>(&object),
        callbacks,
        *image,
        detail));
    REQUIRE(detail.find("rtti=0x0") != std::string::npos);
}

TEST_CASE(PresentVtableHookRejectsPresentSlotOutsideModule) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    REQUIRE(image.has_value());
    std::array<void*, kVtableSlots + 1> original_vtable = {};
    FillVtable(original_vtable);
    original_vtable[kPresentSlot + 1] =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x10000));
    FakeSwapChain object = {&original_vtable[1]};
    HookProbe probe;
    PresentHookCallbacks callbacks = {&FrameCallback, nullptr, &probe};
    PresentHookReset reset;
    std::string detail;

    REQUIRE(!HookRuntime::InstallPresentHookForObject(
        reinterpret_cast<IDXGISwapChain*>(&object),
        callbacks,
        *image,
        detail));
    REQUIRE(detail.find("present_slot=8") != std::string::npos);
    REQUIRE(detail.find("module_base=") != std::string::npos);
    REQUIRE(detail.find("module_end=") != std::string::npos);
}

TEST_CASE(PresentVtableHookRejectsPresent1SlotNotFunctionStart) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    REQUIRE(image.has_value());
    const auto text = image->Section(".text");
    REQUIRE(text.has_value());
    std::array<void*, kVtableSlots + 1> original_vtable = {};
    FillVtable(original_vtable);
    original_vtable[kPresent1Slot + 1] =
        reinterpret_cast<void*>(text->address + 1);
    FakeSwapChain object = {&original_vtable[1]};
    HookProbe probe;
    PresentHookCallbacks callbacks = {&FrameCallback, nullptr, &probe};
    PresentHookReset reset;
    std::string detail;

    REQUIRE(!HookRuntime::InstallPresentHookForObject(
        reinterpret_cast<IDXGISwapChain*>(&object),
        callbacks,
        *image,
        detail));
    REQUIRE(detail.find("present1_slot=22") != std::string::npos);
    REQUIRE(detail.find("pdata") != std::string::npos);
}

TEST_CASE(PresentObjectDiscoveryFindsNonLegacySwapChainOffset) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    REQUIRE(image.has_value());
    std::array<void*, kVtableSlots + 1> original_vtable = {};
    FillVtable(original_vtable);
    FakeSwapChain object = {&original_vtable[1]};
    FakeHackerDevice device = {};
    device.swap_chain = reinterpret_cast<IDXGISwapChain*>(&object);

    PresentObjectCandidate candidate;
    std::string detail;
    REQUIRE(HookRuntime::FindPresentObjectInMemory(
        &device,
        sizeof(device),
        *image,
        candidate,
        detail));
    REQUIRE_EQ(candidate.swap_chain,
               reinterpret_cast<IDXGISwapChain*>(&object));
    REQUIRE_EQ(
        candidate.field_offset,
        offsetof(FakeHackerDevice, swap_chain));
}

TEST_CASE(PresentHooksSkipFrameCallbacksForPresentTest) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    REQUIRE(image.has_value());
    std::array<void*, kVtableSlots + 1> original_vtable = {};
    FillVtable(original_vtable);
    FakeSwapChain object = {&original_vtable[1]};
    HookProbe probe;
    PresentHookCallbacks callbacks = {&FrameCallback, nullptr, &probe};
    PresentHookReset reset;

    HookRuntime::ResetPresentHooksForTests();
    HookRuntime::SetPresentTestContext(&probe);
    std::string detail;
    REQUIRE(HookRuntime::InstallPresentHookForObject(
        reinterpret_cast<IDXGISwapChain*>(&object),
        callbacks,
        *image,
        detail));

    auto* current_vtable = object.object_vptr;
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
        IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    const auto present =
        reinterpret_cast<PresentFn>(current_vtable[kPresentSlot]);
    const auto present1 =
        reinterpret_cast<Present1Fn>(current_vtable[kPresent1Slot]);

    REQUIRE_EQ(
        present(
            reinterpret_cast<IDXGISwapChain*>(&object),
            1,
            DXGI_PRESENT_TEST),
        S_OK);
    REQUIRE_EQ(
        present1(
            reinterpret_cast<IDXGISwapChain1*>(&object),
            1,
            DXGI_PRESENT_TEST,
            nullptr),
        S_OK);
    REQUIRE_EQ(probe.frames, 0);
    REQUIRE_EQ(probe.original_present, 1);
    REQUIRE_EQ(probe.original_present1, 1);
}

TEST_CASE(PresentFunctionHooksNotifyAndSkipPresentTest) {
    HookProbe probe;
    HookRuntime::SetPresentTestContext(&probe);
    PresentHookCallbacks callbacks = {&FrameCallback, nullptr, &probe};
    PresentHookReset reset;
    std::string detail;
    REQUIRE(HookRuntime::InstallPresentFunctionHooks(
        reinterpret_cast<void*>(&OriginalPresentFunction),
        reinterpret_cast<void*>(&OriginalPresent1Function),
        callbacks,
        detail));

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
        IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    const auto present =
        reinterpret_cast<PresentFn>(&OriginalPresentFunction);
    const auto present1 =
        reinterpret_cast<Present1Fn>(&OriginalPresent1Function);

    REQUIRE_EQ(present(nullptr, 1, 0), S_OK);
    REQUIRE_EQ(present1(nullptr, 1, 0, nullptr), S_OK);
    REQUIRE_EQ(present(nullptr, 1, DXGI_PRESENT_TEST), S_OK);
    REQUIRE_EQ(present1(nullptr, 1, DXGI_PRESENT_TEST, nullptr), S_OK);
    REQUIRE_EQ(probe.frames, 2);
    REQUIRE_EQ(probe.original_present, 2);
    REQUIRE_EQ(probe.original_present1, 2);
}

TEST_CASE(PresentVtableHookRunsPostFrameAfterOriginalPresent) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    REQUIRE(image.has_value());
    std::array<void*, kVtableSlots + 1> original_vtable = {};
    FillVtable(original_vtable);
    FakeSwapChain object = {&original_vtable[1]};
    HookProbe probe;
    PresentHookCallbacks callbacks = {
        &FrameCallback, &PostFrameCallback, &probe};
    PresentHookReset reset;

    HookRuntime::ResetPresentHooksForTests();
    HookRuntime::SetPresentTestContext(&probe);

    std::string detail;
    REQUIRE(HookRuntime::InstallPresentHookForObject(
        reinterpret_cast<IDXGISwapChain*>(&object),
        callbacks,
        *image,
        detail));

    auto* current_vtable = object.object_vptr;
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    const auto present =
        reinterpret_cast<PresentFn>(current_vtable[kPresentSlot]);
    REQUIRE_EQ(present(reinterpret_cast<IDXGISwapChain*>(&object), 1, 0), S_OK);

    REQUIRE_EQ(probe.frames, 1);
    REQUIRE_EQ(probe.post_frames, 1);
    // The pre seam runs before the original Present; the post seam must run
    // after it, which is where frame analysis is armed.
    REQUIRE_EQ(probe.frames_before_original, 0);
    REQUIRE_EQ(probe.posts_after_original, 1);
}

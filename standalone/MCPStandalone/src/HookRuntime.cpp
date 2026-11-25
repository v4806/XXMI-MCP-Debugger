#include "HookRuntime.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <sstream>
#include <vector>

#include "../Nektra/NktHookLib.h"
#include "ModuleImage.h"

namespace {

constexpr std::size_t kPresentSlot = 8;
constexpr std::size_t kPresent1Slot = 22;
constexpr std::size_t kVtableSlots = 64;
constexpr std::size_t kMaxPresentHooks = 8;

class SrwExclusiveLock {
public:
    explicit SrwExclusiveLock(SRWLOCK& lock) : lock_(lock) {
        AcquireSRWLockExclusive(&lock_);
    }
    ~SrwExclusiveLock() {
        ReleaseSRWLockExclusive(&lock_);
    }

private:
    SRWLOCK& lock_;
};

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain* swap_chain,
    UINT sync_interval,
    UINT flags);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain1* swap_chain,
    UINT sync_interval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters);
using CreateDeviceFn = HRESULT(WINAPI*)(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels,
    UINT feature_level_count,
    UINT sdk_version,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* feature_level,
    ID3D11DeviceContext** immediate_context);
using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels,
    UINT feature_level_count,
    UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC* swap_chain_desc,
    IDXGISwapChain** swap_chain,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* feature_level,
    ID3D11DeviceContext** immediate_context);

struct PresentHookRecord {
    IDXGISwapChain* swap_chain = nullptr;
    void** original_vtable = nullptr;
    void** storage = nullptr;
    PresentFn original_present = nullptr;
    Present1Fn original_present1 = nullptr;
    PresentHookCallbacks callbacks;
};

struct PresentVtableSnapshot {
    void* object = nullptr;
    void* vptr = nullptr;
    void* rtti = nullptr;
    std::array<void*, kVtableSlots> slots = {};
};

SRWLOCK g_present_lock = SRWLOCK_INIT;
std::array<PresentHookRecord, kMaxPresentHooks> g_present_hooks = {};
std::atomic<void*> g_present_test_context = nullptr;

SRWLOCK g_create_lock = SRWLOCK_INIT;
CNktHookLib g_create_hook_lib;
SIZE_T g_create_device_hook_id = 0;
SIZE_T g_create_swap_chain_hook_id = 0;
CreateDeviceFn g_original_create_device = nullptr;
CreateDeviceAndSwapChainFn g_original_create_device_and_swap_chain = nullptr;
DeviceCreateCallbacks g_device_callbacks;
bool g_create_hooks_installed = false;

SRWLOCK g_present_function_lock = SRWLOCK_INIT;
CNktHookLib g_present_function_hook_lib;
SIZE_T g_present_function_hook_id = 0;
SIZE_T g_present1_function_hook_id = 0;
PresentFn g_original_present_function = nullptr;
Present1Fn g_original_present1_function = nullptr;
PresentHookCallbacks g_present_function_callbacks;
bool g_present_function_hooks_installed = false;

std::string HookError(const char* operation, DWORD error) {
    std::ostringstream stream;
    stream << operation << " failed with Win32/NktHook code " << error;
    return stream.str();
}

std::string Hex(const void* pointer) {
    std::ostringstream stream;
    stream << "0x" << std::hex
           << reinterpret_cast<std::uintptr_t>(pointer);
    return stream.str();
}

std::string DescribePresentEvidence(
    const PresentVtableSnapshot& snapshot,
    const ModuleImage& official_module,
    const char* reason) {
    std::ostringstream stream;
    stream << "reason=" << reason
           << " object=" << Hex(snapshot.object)
           << " vptr=" << Hex(snapshot.vptr)
           << " rtti=" << Hex(snapshot.rtti)
           << " present_slot=8 present=" << Hex(snapshot.slots[kPresentSlot])
           << " present1_slot=22 present1="
           << Hex(snapshot.slots[kPresent1Slot])
           << " module_base=" << Hex(
                  reinterpret_cast<const void*>(official_module.Base()))
           << " module_end=" << Hex(
                  reinterpret_cast<const void*>(
                      official_module.Base() + official_module.Size()));
    return stream.str();
}

bool ReadProcessValue(
    const void* address,
    void* value,
    std::size_t size,
    std::string& reason) {
    SIZE_T read = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            address,
            value,
            size,
            &read) ||
        read != size) {
        const auto error = GetLastError();
        std::ostringstream stream;
        stream << "ReadProcessMemory failed at "
               << Hex(address) << " with Win32 error "
               << (error == ERROR_SUCCESS ? ERROR_PARTIAL_COPY : error);
        reason = stream.str();
        return false;
    }
    return true;
}

bool ReadPresentVtable(
    IDXGISwapChain* swap_chain,
    PresentVtableSnapshot& snapshot,
    std::string& reason) {
    snapshot.object = swap_chain;
    if (!swap_chain) {
        reason = "swap chain is null";
        return false;
    }

    void** vptr = nullptr;
    if (!ReadProcessValue(
            swap_chain,
            &vptr,
            sizeof(vptr),
            reason)) {
        reason = "could not read object vptr: " + reason;
        return false;
    }
    snapshot.vptr = vptr;
    if (!vptr) {
        reason = "swap-chain vtable is null";
        return false;
    }

    const auto vptr_address =
        reinterpret_cast<std::uintptr_t>(vptr);
    if (vptr_address < sizeof(void*)) {
        reason = "vtable address is too small to contain RTTI storage";
        return false;
    }
    if (!ReadProcessValue(
            reinterpret_cast<const void*>(
                vptr_address - sizeof(void*)),
            &snapshot.rtti,
            sizeof(snapshot.rtti),
            reason)) {
        reason = "could not read vtable RTTI storage: " + reason;
        return false;
    }
    if (!snapshot.rtti) {
        reason = "vtable RTTI storage is null";
        return false;
    }
    std::uintptr_t rtti_probe = 0;
    if (!ReadProcessValue(
            snapshot.rtti,
            &rtti_probe,
            sizeof(rtti_probe),
            reason)) {
        reason = "could not read RTTI target: " + reason;
        return false;
    }
    if (rtti_probe == 0) {
        reason = "RTTI target storage is zero";
        return false;
    }

    if (!ReadProcessValue(
            vptr,
            snapshot.slots.data(),
            snapshot.slots.size() * sizeof(void*),
            reason)) {
        reason = "could not read vtable storage: " + reason;
        return false;
    }
    return true;
}

bool ValidatePresentTarget(
    const ModuleImage& official_module,
    const char* slot_name,
    std::size_t slot_index,
    void* target,
    std::string& reason) {
    if (!target) {
        std::ostringstream stream;
        stream << slot_name << " slot " << slot_index << " is null";
        reason = stream.str();
        return false;
    }
    const auto address =
        reinterpret_cast<std::uintptr_t>(target);
    if (!official_module.ContainsAddress(address, 1)) {
        std::ostringstream stream;
        stream << slot_name << " slot " << slot_index
               << " target " << Hex(target)
               << " is outside the official module";
        reason = stream.str();
        return false;
    }
    if (!official_module.IsFunctionStart(address)) {
        std::ostringstream stream;
        stream << slot_name << " slot " << slot_index
               << " target " << Hex(target)
               << " is not a .pdata function start";
        reason = stream.str();
        return false;
    }
    return true;
}

PresentHookRecord* FindPresentRecordLocked(IDXGISwapChain* swap_chain) {
    for (auto& record : g_present_hooks) {
        if (record.swap_chain == swap_chain)
            return &record;
    }
    return nullptr;
}

PresentHookRecord* FindPresentRecord(IDXGISwapChain* swap_chain) {
    AcquireSRWLockShared(&g_present_lock);
    auto* record = FindPresentRecordLocked(swap_chain);
    ReleaseSRWLockShared(&g_present_lock);
    return record;
}

void NotifyFrame(IDXGISwapChain* swap_chain) {
    auto* record = FindPresentRecord(swap_chain);
    if (record && record->callbacks.on_frame)
        record->callbacks.on_frame(record->callbacks.context, swap_chain);
}

void NotifyPostFrame(IDXGISwapChain* swap_chain) {
    auto* record = FindPresentRecord(swap_chain);
    if (record && record->callbacks.on_post_frame)
        record->callbacks.on_post_frame(record->callbacks.context, swap_chain);
}

HRESULT STDMETHODCALLTYPE HookedPresent(
    IDXGISwapChain* swap_chain,
    UINT sync_interval,
    UINT flags) {
    auto* record = FindPresentRecord(swap_chain);
    if (!record || !record->original_present)
        return E_FAIL;
    const bool notify = (flags & DXGI_PRESENT_TEST) == 0;
    if (notify)
        NotifyFrame(swap_chain);
    const HRESULT hr =
        record->original_present(swap_chain, sync_interval, flags);
    if (notify)
        NotifyPostFrame(swap_chain);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresent1(
    IDXGISwapChain1* swap_chain,
    UINT sync_interval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    auto* record =
        FindPresentRecord(reinterpret_cast<IDXGISwapChain*>(swap_chain));
    if (!record || !record->original_present1)
        return E_FAIL;
    const bool notify = (flags & DXGI_PRESENT_TEST) == 0;
    if (notify)
        NotifyFrame(reinterpret_cast<IDXGISwapChain*>(swap_chain));
    const HRESULT hr = record->original_present1(
        swap_chain, sync_interval, flags, parameters);
    if (notify)
        NotifyPostFrame(reinterpret_cast<IDXGISwapChain*>(swap_chain));
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresentFunction(
    IDXGISwapChain* swap_chain,
    UINT sync_interval,
    UINT flags) {
    if (!g_original_present_function)
        return E_FAIL;
    const bool notify =
        (flags & DXGI_PRESENT_TEST) == 0 &&
        g_present_function_callbacks.on_frame;
    if (notify) {
        g_present_function_callbacks.on_frame(
            g_present_function_callbacks.context, swap_chain);
    }
    const HRESULT hr =
        g_original_present_function(swap_chain, sync_interval, flags);
    if ((flags & DXGI_PRESENT_TEST) == 0 &&
        g_present_function_callbacks.on_post_frame) {
        g_present_function_callbacks.on_post_frame(
            g_present_function_callbacks.context, swap_chain);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresent1Function(
    IDXGISwapChain1* swap_chain,
    UINT sync_interval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    if (!g_original_present1_function)
        return E_FAIL;
    if ((flags & DXGI_PRESENT_TEST) == 0 &&
        g_present_function_callbacks.on_frame) {
        g_present_function_callbacks.on_frame(
            g_present_function_callbacks.context,
            reinterpret_cast<IDXGISwapChain*>(swap_chain));
    }
    const HRESULT hr = g_original_present1_function(
        swap_chain, sync_interval, flags, parameters);
    if ((flags & DXGI_PRESENT_TEST) == 0 &&
        g_present_function_callbacks.on_post_frame) {
        g_present_function_callbacks.on_post_frame(
            g_present_function_callbacks.context,
            reinterpret_cast<IDXGISwapChain*>(swap_chain));
    }
    return hr;
}

void NotifyDeviceCreated(
    ID3D11Device* device,
    IDXGISwapChain* swap_chain) {
    if (g_device_callbacks.on_device_created) {
        g_device_callbacks.on_device_created(
            g_device_callbacks.context, device, swap_chain);
    }
}

HRESULT WINAPI HookedCreateDevice(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels,
    UINT feature_level_count,
    UINT sdk_version,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* feature_level,
    ID3D11DeviceContext** immediate_context) {
    const auto result = g_original_create_device(
        adapter, driver_type, software, flags, feature_levels,
        feature_level_count, sdk_version, device, feature_level,
        immediate_context);
    if (SUCCEEDED(result))
        NotifyDeviceCreated(device ? *device : nullptr, nullptr);
    return result;
}

HRESULT WINAPI HookedCreateDeviceAndSwapChain(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels,
    UINT feature_level_count,
    UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC* swap_chain_desc,
    IDXGISwapChain** swap_chain,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* feature_level,
    ID3D11DeviceContext** immediate_context) {
    const auto result = g_original_create_device_and_swap_chain(
        adapter, driver_type, software, flags, feature_levels,
        feature_level_count, sdk_version, swap_chain_desc, swap_chain,
        device, feature_level, immediate_context);
    if (SUCCEEDED(result)) {
        NotifyDeviceCreated(
            device ? *device : nullptr,
            swap_chain ? *swap_chain : nullptr);
    }
    return result;
}

}  // namespace

bool HookRuntime::InstallCreateDeviceHooks(
    const DeviceCreateCallbacks& callbacks,
    std::string& detail) {
    SrwExclusiveLock lock(g_create_lock);
    if (g_create_hooks_installed) {
        detail = "create-device hooks are already installed";
        return true;
    }

    const HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) {
        detail = "official d3d11.dll is not loaded";
        return false;
    }

    const auto create_device = reinterpret_cast<CreateDeviceFn>(
        GetProcAddress(d3d11, "D3D11CreateDevice"));
    const auto create_device_and_swap_chain =
        reinterpret_cast<CreateDeviceAndSwapChainFn>(
            GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (!create_device || !create_device_and_swap_chain) {
        detail = "D3D11 create-device exports are missing";
        return false;
    }

    g_device_callbacks = callbacks;
    const auto first_error = g_create_hook_lib.Hook(
        &g_create_device_hook_id,
        reinterpret_cast<LPVOID*>(&g_original_create_device),
        reinterpret_cast<LPVOID>(create_device),
        reinterpret_cast<LPVOID>(&HookedCreateDevice));
    if (first_error != 0) {
        g_device_callbacks = {};
        detail = HookError("Hooking D3D11CreateDevice", first_error);
        return false;
    }

    const auto second_error = g_create_hook_lib.Hook(
        &g_create_swap_chain_hook_id,
        reinterpret_cast<LPVOID*>(&g_original_create_device_and_swap_chain),
        reinterpret_cast<LPVOID>(create_device_and_swap_chain),
        reinterpret_cast<LPVOID>(&HookedCreateDeviceAndSwapChain));
    if (second_error != 0) {
        g_create_hook_lib.Unhook(g_create_device_hook_id);
        g_create_device_hook_id = 0;
        g_original_create_device = nullptr;
        g_device_callbacks = {};
        detail = HookError(
            "Hooking D3D11CreateDeviceAndSwapChain", second_error);
        return false;
    }

    g_create_hooks_installed = true;
    detail = "create-device hooks installed";
    return true;
}

void HookRuntime::RemoveCreateDeviceHooks() {
    SrwExclusiveLock lock(g_create_lock);
    if (!g_create_hooks_installed)
        return;
    if (g_create_swap_chain_hook_id)
        g_create_hook_lib.Unhook(g_create_swap_chain_hook_id);
    if (g_create_device_hook_id)
        g_create_hook_lib.Unhook(g_create_device_hook_id);
    g_create_swap_chain_hook_id = 0;
    g_create_device_hook_id = 0;
    g_original_create_device = nullptr;
    g_original_create_device_and_swap_chain = nullptr;
    g_device_callbacks = {};
    g_create_hooks_installed = false;
}

bool HookRuntime::InstallPresentFunctionHooks(
    void* present_target,
    void* present1_target,
    const PresentHookCallbacks& callbacks,
    std::string& detail) {
    SrwExclusiveLock lock(g_present_function_lock);
    if (g_present_function_hooks_installed) {
        detail = "Present function hooks are already installed";
        return true;
    }
    if (!present_target && !present1_target) {
        detail = "Present function targets are missing";
        return false;
    }
    if (!callbacks.on_frame) {
        detail = "Present function frame callback is null";
        return false;
    }

    g_present_function_callbacks = callbacks;
    if (present_target) {
        const auto error = g_present_function_hook_lib.Hook(
            &g_present_function_hook_id,
            reinterpret_cast<LPVOID*>(&g_original_present_function),
            present_target,
            reinterpret_cast<LPVOID>(&HookedPresentFunction));
        if (error != 0) {
            g_present_function_callbacks = {};
            detail = HookError(
                "Hooking HackerSwapChain::Present", error);
            return false;
        }
    }

    if (present1_target) {
        const auto error = g_present_function_hook_lib.Hook(
            &g_present1_function_hook_id,
            reinterpret_cast<LPVOID*>(&g_original_present1_function),
            present1_target,
            reinterpret_cast<LPVOID>(&HookedPresent1Function));
        if (error != 0) {
            if (g_present_function_hook_id) {
                g_present_function_hook_lib.Unhook(
                    g_present_function_hook_id);
            }
            g_present_function_hook_id = 0;
            g_original_present_function = nullptr;
            g_present_function_callbacks = {};
            detail = HookError(
                "Hooking HackerSwapChain::Present1", error);
            return false;
        }
    }

    g_present_function_hooks_installed = true;
    std::ostringstream stream;
    stream << "Present function hooks installed"
           << " present=" << Hex(present_target)
           << " present1=" << Hex(present1_target);
    detail = stream.str();
    return true;
}

void HookRuntime::RemovePresentFunctionHooks() {
    SrwExclusiveLock lock(g_present_function_lock);
    if (!g_present_function_hooks_installed)
        return;
    if (g_present1_function_hook_id) {
        g_present_function_hook_lib.Unhook(
            g_present1_function_hook_id);
    }
    if (g_present_function_hook_id) {
        g_present_function_hook_lib.Unhook(
            g_present_function_hook_id);
    }
    g_present1_function_hook_id = 0;
    g_present_function_hook_id = 0;
    g_original_present_function = nullptr;
    g_original_present1_function = nullptr;
    g_present_function_callbacks = {};
    g_present_function_hooks_installed = false;
}

bool HookRuntime::InstallPresentHookForObject(
    IDXGISwapChain* swap_chain,
    const PresentHookCallbacks& callbacks,
    const ModuleImage& official_module,
    std::string& detail) {
    if (!swap_chain) {
        PresentVtableSnapshot snapshot;
        snapshot.object = swap_chain;
        detail = DescribePresentEvidence(
            snapshot, official_module, "swap chain is null");
        return false;
    }
    if (!callbacks.on_frame) {
        PresentVtableSnapshot snapshot;
        snapshot.object = swap_chain;
        detail = DescribePresentEvidence(
            snapshot, official_module, "frame callback is null");
        return false;
    }

    AcquireSRWLockExclusive(&g_present_lock);
    if (FindPresentRecordLocked(swap_chain)) {
        ReleaseSRWLockExclusive(&g_present_lock);
        detail = "swap chain already has a Present hook";
        return true;
    }

    PresentHookRecord* slot = nullptr;
    for (auto& record : g_present_hooks) {
        if (!record.swap_chain) {
            slot = &record;
            break;
        }
    }
    if (!slot) {
        ReleaseSRWLockExclusive(&g_present_lock);
        PresentVtableSnapshot snapshot;
        snapshot.object = swap_chain;
        detail = DescribePresentEvidence(
            snapshot, official_module, "Present hook table is full");
        return false;
    }

    PresentVtableSnapshot snapshot;
    std::string reason;
    if (!ReadPresentVtable(swap_chain, snapshot, reason)) {
        detail = DescribePresentEvidence(snapshot, official_module,
                                         reason.c_str());
        ReleaseSRWLockExclusive(&g_present_lock);
        return false;
    }
    if (!ValidatePresentTarget(
            official_module, "Present", kPresentSlot,
            snapshot.slots[kPresentSlot], reason) ||
        !ValidatePresentTarget(
            official_module, "Present1", kPresent1Slot,
            snapshot.slots[kPresent1Slot], reason)) {
        detail = DescribePresentEvidence(snapshot, official_module,
                                         reason.c_str());
        ReleaseSRWLockExclusive(&g_present_lock);
        return false;
    }

    auto* storage = new (std::nothrow) void*[kVtableSlots + 1];
    if (!storage) {
        ReleaseSRWLockExclusive(&g_present_lock);
        detail = DescribePresentEvidence(
            snapshot, official_module, "could not allocate vtable clone");
        return false;
    }

    storage[0] = snapshot.rtti;
    auto* clone = &storage[1];
    std::memcpy(clone, snapshot.slots.data(),
                snapshot.slots.size() * sizeof(void*));

    const auto original_present =
        reinterpret_cast<PresentFn>(clone[kPresentSlot]);
    const auto original_present1 =
        reinterpret_cast<Present1Fn>(clone[kPresent1Slot]);
    clone[kPresentSlot] = reinterpret_cast<void*>(&HookedPresent);
    clone[kPresent1Slot] = reinterpret_cast<void*>(&HookedPresent1);

    *slot = PresentHookRecord{
        swap_chain,
        static_cast<void**>(snapshot.vptr),
        storage,
        original_present,
        original_present1,
        callbacks,
    };
    InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(swap_chain),
        clone);
    ReleaseSRWLockExclusive(&g_present_lock);
    detail = "Present and Present1 hooks installed: " +
             DescribePresentEvidence(
                 snapshot, official_module, "validated");
    return true;
}

bool HookRuntime::FindPresentObjectInMemory(
    void* container,
    std::size_t scan_size,
    const ModuleImage& official_module,
    PresentObjectCandidate& candidate,
    std::string& detail) {
    candidate = {};
    if (!container) {
        detail = "container object is null";
        return false;
    }
    if (scan_size < sizeof(void*) ||
        scan_size > 0x1000 ||
        scan_size % sizeof(void*) != 0) {
        detail = "container scan size is invalid";
        return false;
    }

    std::vector<std::uint8_t> bytes(scan_size);
    SIZE_T read = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            container,
            bytes.data(),
            bytes.size(),
            &read) ||
        read != bytes.size()) {
        std::ostringstream stream;
        stream << "could not read container memory at "
               << Hex(container) << " size=0x" << std::hex
               << scan_size << " Win32 error "
               << std::dec
               << (GetLastError() == ERROR_SUCCESS
                       ? ERROR_PARTIAL_COPY
                       : GetLastError());
        detail = stream.str();
        return false;
    }

    std::size_t readable_candidates = 0;
    std::string last_reason;
    for (std::size_t offset = 0;
         offset + sizeof(void*) <= bytes.size();
         offset += sizeof(void*)) {
        void* value = nullptr;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        if (!value ||
            reinterpret_cast<std::uintptr_t>(value) % alignof(void*) != 0) {
            continue;
        }

        PresentVtableSnapshot snapshot;
        std::string reason;
        if (!ReadPresentVtable(
                reinterpret_cast<IDXGISwapChain*>(value),
                snapshot,
                reason)) {
            continue;
        }
        ++readable_candidates;
        if (!ValidatePresentTarget(
                official_module,
                "Present",
                kPresentSlot,
                snapshot.slots[kPresentSlot],
                reason) ||
            !ValidatePresentTarget(
                official_module,
                "Present1",
                kPresent1Slot,
                snapshot.slots[kPresent1Slot],
                reason)) {
            last_reason = std::move(reason);
            continue;
        }

        candidate.swap_chain =
            reinterpret_cast<IDXGISwapChain*>(value);
        candidate.field_offset = offset;
        std::ostringstream stream;
        stream << "validated swap-chain candidate name=field"
               << " field_offset=0x" << std::hex << offset
               << " readable_candidates=" << std::dec
               << readable_candidates
               << " " << DescribePresentEvidence(
                      snapshot, official_module, "validated");
        detail = stream.str();
        return true;
    }

    std::ostringstream stream;
    stream << "no validated swap-chain candidate found"
           << " container=" << Hex(container)
           << " scan_size=0x" << std::hex << scan_size
           << " readable_candidates=" << std::dec
           << readable_candidates;
    if (!last_reason.empty())
        stream << " last_reason=" << last_reason;
    detail = stream.str();
    return false;
}

void* HookRuntime::GetPresentTestContext() {
    return g_present_test_context.load(std::memory_order_acquire);
}

void HookRuntime::SetPresentTestContext(void* context) {
    g_present_test_context.store(context, std::memory_order_release);
}

void HookRuntime::ResetPresentHooksForTests() {
    AcquireSRWLockExclusive(&g_present_lock);
    for (auto& record : g_present_hooks) {
        if (record.swap_chain) {
            InterlockedExchangePointer(
                reinterpret_cast<PVOID volatile*>(record.swap_chain),
                record.original_vtable);
        }
        delete[] record.storage;
        record = {};
    }
    ReleaseSRWLockExclusive(&g_present_lock);
    g_present_test_context.store(nullptr, std::memory_order_release);
}

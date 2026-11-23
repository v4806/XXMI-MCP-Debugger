#include "DrawProbe.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

#include "../Nektra/NktHookLib.h"
#include "ModuleImage.h"

namespace {

constexpr std::array<std::size_t, 7> kDrawVtableSlots = {
    13,  // Draw
    12,  // DrawIndexed
    19,  // DrawInstanced
    18,  // DrawIndexedInstanced
    36,  // DrawAuto
    37,  // DrawIndexedInstancedIndirect
    38,  // DrawInstancedIndirect
};

struct ProbeDrawCallInfo {
    std::uint32_t type = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t first_vertex = 0;
    std::uint32_t first_index = 0;
    std::uint32_t first_instance = 0;
    std::uint32_t thread_group_x = 0;
    std::uint32_t thread_group_y = 0;
    std::uint32_t thread_group_z = 0;
    void* indirect_buffer = nullptr;
    std::uint32_t args_offset = 0;
    std::uint8_t skip = 0;
    std::uint8_t hunting_skip = 0;
};

struct ProbeDrawContext {
    void* old_pixel_shader = nullptr;
    void* old_vertex_shader = nullptr;
    void* post_commands[5] = {};
    ProbeDrawCallInfo call_info;
};

static_assert(
    offsetof(ProbeDrawContext, call_info) == 56,
    "DrawContext layout must match the official implementation");
static_assert(
    offsetof(ProbeDrawCallInfo, indirect_buffer) == 40,
    "DrawCallInfo layout must match the official implementation");
static_assert(
    offsetof(ProbeDrawCallInfo, skip) == 52,
    "DrawCallInfo skip offset must match the official implementation");

using BeforeDrawFn = void (*)(void* hacker_context, void* draw_context);

std::string PointerString(const void* pointer) {
    std::ostringstream stream;
    stream << "0x" << std::hex
           << reinterpret_cast<std::uintptr_t>(pointer);
    return stream.str();
}

std::string DrawTypeName(std::uint32_t type) {
    switch (type) {
    case 1:
        return "Draw";
    case 2:
        return "DrawIndexed";
    case 3:
        return "DrawInstanced";
    case 4:
        return "DrawIndexedInstanced";
    case 5:
        return "DrawInstancedIndirect";
    case 6:
        return "DrawIndexedInstancedIndirect";
    case 7:
        return "DrawAuto";
    default:
        return "unknown";
    }
}

bool ReadDrawContext(
    void* draw_context,
    ProbeDrawContext& value) {
    if (!draw_context)
        return false;
    SIZE_T read = 0;
    return ReadProcessMemory(
               GetCurrentProcess(),
               draw_context,
               &value,
               sizeof(value),
               &read) &&
           read == sizeof(value);
}

void AddMissing(LiveProbeSnapshot& snapshot, std::string value) {
    if (std::find(
            snapshot.missing_fields.begin(),
            snapshot.missing_fields.end(),
            value) == snapshot.missing_fields.end()) {
        snapshot.missing_fields.push_back(std::move(value));
    }
}

void FillDrawInfo(
    LiveProbeSnapshot& snapshot,
    const ProbeDrawContext& context) {
    const auto& info = context.call_info;
    snapshot.draw.type = DrawTypeName(info.type);
    if (snapshot.draw.type == "unknown")
        AddMissing(snapshot, "draw.type");
    snapshot.draw.vertex_count = info.vertex_count;
    snapshot.draw.index_count = info.index_count;
    snapshot.draw.instance_count = info.instance_count;
    snapshot.draw.first_vertex = info.first_vertex;
    snapshot.draw.first_index = info.first_index;
    snapshot.draw.first_instance = info.first_instance;
    snapshot.draw.thread_group_x = info.thread_group_x;
    snapshot.draw.thread_group_y = info.thread_group_y;
    snapshot.draw.thread_group_z = info.thread_group_z;
    if (info.type == 5 || info.type == 6) {
        AddMissing(snapshot, "draw.indirect_arguments");
    }
}

void FillDrawResult(
    LiveProbeSnapshot& snapshot,
    const ProbeDrawContext& context) {
    snapshot.draw.skip = context.call_info.skip != 0;
    snapshot.draw.hunting_skip =
        context.call_info.hunting_skip != 0;
    snapshot.draw.post_command_slots.clear();
    for (std::uint32_t index = 0; index < 5; ++index) {
        if (context.post_commands[index])
            snapshot.draw.post_command_slots.push_back(index);
    }
}

struct HookCallCandidate {
    std::uintptr_t target = 0;
    std::size_t occurrences = 0;
    std::size_t earliest_offset = 0;
};

bool ReadVtableSlot(
    ID3D11DeviceContext1* context,
    std::size_t slot,
    void*& target,
    std::string& detail) {
    void** vtable = nullptr;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            context,
            &vtable,
            sizeof(vtable),
            nullptr)) {
        detail = "could not read HackerContext vtable pointer";
        return false;
    }
    if (!vtable) {
        detail = "HackerContext vtable pointer is null";
        return false;
    }
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            &vtable[slot],
            &target,
            sizeof(target),
            nullptr)) {
        detail = "could not read HackerContext vtable slot " +
                 std::to_string(slot);
        return false;
    }
    return target != nullptr;
}

std::vector<std::pair<std::uintptr_t, std::size_t>>
ReadFunctionCalls(
    const ModuleImage& official_module,
    const FunctionRange& range) {
    std::vector<std::pair<std::uintptr_t, std::size_t>> calls;
    const auto size = std::min<std::size_t>(
        range.end - range.begin, 0x1000);
    std::vector<std::uint8_t> bytes(size);
    if (!official_module.Read(range.begin, bytes.data(), bytes.size()))
        return calls;

    for (std::size_t offset = 0; offset + 5 <= bytes.size(); ++offset) {
        if (bytes[offset] != 0xE8)
            continue;
        std::int32_t displacement = 0;
        std::memcpy(
            &displacement,
            bytes.data() + offset + 1,
            sizeof(displacement));
        const auto call_address =
            range.begin + offset;
        const auto target = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(call_address) + 5 +
            displacement);
        if (official_module.ContainsAddress(target, 1) &&
            official_module.IsFunctionStart(target)) {
            calls.emplace_back(target, offset);
        }
    }
    return calls;
}

struct NativeHookState {
    SRWLOCK lock = SRWLOCK_INIT;
    CNktHookLib hook_library;
    SIZE_T hook_id = 0;
    void* original = nullptr;
    BeforeDrawHook::Callback callback = nullptr;
    void* callback_context = nullptr;
    bool installed = false;
};

NativeHookState g_native_hook_state;
thread_local bool g_in_native_before_draw = false;

struct BeforeDrawReentryGuard {
    BeforeDrawReentryGuard() {
        g_in_native_before_draw = true;
    }
    ~BeforeDrawReentryGuard() {
        g_in_native_before_draw = false;
    }
};

void HookedNativeBeforeDraw(
    void* hacker_context,
    void* draw_context) {
    auto& state = g_native_hook_state;
    if (g_in_native_before_draw) {
        const auto original = reinterpret_cast<BeforeDrawFn>(
            state.original);
        if (original)
            original(hacker_context, draw_context);
        return;
    }
    BeforeDrawReentryGuard reentry_guard;
    AcquireSRWLockShared(&state.lock);
    const auto callback = state.callback;
    const auto callback_context = state.callback_context;
    const auto original = reinterpret_cast<BeforeDrawFn>(
        state.original);
    if (callback)
        callback(callback_context, hacker_context, draw_context, false);
    if (original)
        original(hacker_context, draw_context);
    if (callback)
        callback(callback_context, hacker_context, draw_context, true);
    ReleaseSRWLockShared(&state.lock);
}

}  // namespace

BeforeDrawProbeController::BeforeDrawProbeController(
    BeforeDrawHook& hook,
    DrawProbeCaptureCallback capture_callback,
    void* capture_context)
    : hook_(hook),
      capture_callback_(capture_callback),
      capture_context_(capture_context) {}

BeforeDrawProbeController::~BeforeDrawProbeController() {
    if (state_ == DrawProbeState::Armed ||
        state_ == DrawProbeState::Capturing ||
        state_ == DrawProbeState::RemovePending) {
        std::string ignored;
        TryRemoveHook(ignored);
    }
}

bool BeforeDrawProbeController::Arm(
    void* official_before_draw,
    void* immediate_context,
    std::uint32_t frame,
    std::string& error) {
    error.clear();
    if (state_ != DrawProbeState::Idle &&
        state_ != DrawProbeState::Captured &&
        state_ != DrawProbeState::NotMatched &&
        state_ != DrawProbeState::Failed) {
        error = "another before-draw probe is active";
        return false;
    }
    if (!official_before_draw) {
        error = "official BeforeDraw target is null";
        return false;
    }
    if (!immediate_context) {
        error = "immediate context is null";
        return false;
    }

    snapshot_ = {};
    snapshot_.kind = "live_draw";
    snapshot_.frame = frame;
    snapshot_.context_pointer =
        reinterpret_cast<std::uintptr_t>(immediate_context);
    error_.clear();
    target_context_ = immediate_context;
    frame_ = frame;
    pending_result_state_ = DrawProbeState::Idle;
    if (!hook_.Install(
            official_before_draw,
            &BeforeDrawProbeController::HookCallback,
            this,
            error)) {
        state_ = DrawProbeState::Idle;
        return false;
    }
    state_ = DrawProbeState::Armed;
    return true;
}

DrawProbeState BeforeDrawProbeController::State() const {
    return state_;
}

void BeforeDrawProbeController::HookCallback(
    void* callback_context,
    void* hacker_context,
    void* draw_context,
    bool after_original) {
    static_cast<BeforeDrawProbeController*>(callback_context)
        ->HandleHook(hacker_context, draw_context, after_original);
}

void BeforeDrawProbeController::HandleHook(
    void* hacker_context,
    void* draw_context,
    bool after_original) {
    if (hacker_context != target_context_)
        return;
    if (!after_original) {
        if (state_ != DrawProbeState::Armed)
            return;
        ProbeDrawContext context;
        if (!ReadDrawContext(draw_context, context)) {
            AddMissing(snapshot_, "draw_context");
        } else {
            FillDrawInfo(snapshot_, context);
        }
        if (capture_callback_ &&
            !capture_callback_(
                capture_context_, snapshot_, error_)) {
            state_ = DrawProbeState::Failed;
            return;
        }
        state_ = DrawProbeState::Capturing;
        return;
    }

    if (state_ != DrawProbeState::Capturing &&
        state_ != DrawProbeState::Failed) {
        return;
    }
    ProbeDrawContext context;
    if (ReadDrawContext(draw_context, context)) {
        FillDrawResult(snapshot_, context);
    } else {
        AddMissing(snapshot_, "draw_result");
    }
    if (state_ == DrawProbeState::Capturing)
        state_ = DrawProbeState::Captured;
}

void BeforeDrawProbeController::OnPresent(std::uint32_t frame) {
    if (state_ == DrawProbeState::Armed &&
        frame > frame_) {
        pending_result_state_ = DrawProbeState::NotMatched;
        state_ = DrawProbeState::RemovePending;
    } else if (state_ == DrawProbeState::Capturing) {
        pending_result_state_ = DrawProbeState::NotMatched;
        state_ = DrawProbeState::RemovePending;
    } else if (
        state_ == DrawProbeState::Captured ||
        state_ == DrawProbeState::Failed) {
        pending_result_state_ = state_;
        state_ = DrawProbeState::RemovePending;
    }

    if (state_ == DrawProbeState::RemovePending) {
        std::string ignored;
        TryRemoveHook(ignored);
    }
}

bool BeforeDrawProbeController::TryRemoveHook(std::string& error) {
    error.clear();
    if (state_ != DrawProbeState::RemovePending &&
        state_ != DrawProbeState::Armed &&
        state_ != DrawProbeState::Capturing &&
        state_ != DrawProbeState::Captured &&
        state_ != DrawProbeState::Failed) {
        return true;
    }
    if (!hook_.TryRemove(error)) {
        if (pending_result_state_ == DrawProbeState::Idle) {
            pending_result_state_ =
                state_ == DrawProbeState::Captured
                    ? DrawProbeState::Captured
                    : state_ == DrawProbeState::Failed
                          ? DrawProbeState::Failed
                          : DrawProbeState::NotMatched;
        }
        state_ = DrawProbeState::RemovePending;
        return false;
    }
    if (pending_result_state_ == DrawProbeState::Idle &&
        (state_ == DrawProbeState::Armed ||
         state_ == DrawProbeState::Capturing)) {
        pending_result_state_ = DrawProbeState::NotMatched;
    }
    if (pending_result_state_ != DrawProbeState::Idle) {
        state_ = pending_result_state_;
    } else if (state_ == DrawProbeState::RemovePending) {
        state_ = DrawProbeState::NotMatched;
    }
    return true;
}

bool BeforeDrawProbeController::HasResult() const {
    return state_ == DrawProbeState::Captured ||
           state_ == DrawProbeState::NotMatched ||
           state_ == DrawProbeState::Failed;
}

const LiveProbeSnapshot& BeforeDrawProbeController::Snapshot() const {
    return snapshot_;
}

const std::string& BeforeDrawProbeController::Error() const {
    return error_;
}

NativeBeforeDrawHook::NativeBeforeDrawHook()
    : impl_(std::make_unique<Impl>()) {}

struct NativeBeforeDrawHook::Impl {};

NativeBeforeDrawHook::~NativeBeforeDrawHook() {
    std::string ignored;
    TryRemove(ignored);
}

bool NativeBeforeDrawHook::Install(
    void* target,
    Callback callback,
    void* callback_context,
    std::string& error) {
    error.clear();
    if (!target || !callback) {
        error = "BeforeDraw target or callback is null";
        return false;
    }
    auto& state = g_native_hook_state;
    AcquireSRWLockExclusive(&state.lock);
    if (state.installed) {
        ReleaseSRWLockExclusive(&state.lock);
        error = "BeforeDraw hook is already installed";
        return false;
    }
    state.callback = callback;
    state.callback_context = callback_context;
    const auto result = state.hook_library.Hook(
        &state.hook_id,
        reinterpret_cast<LPVOID*>(&state.original),
        target,
        reinterpret_cast<LPVOID>(&HookedNativeBeforeDraw));
    if (result != 0) {
        state.callback = nullptr;
        state.callback_context = nullptr;
        state.original = nullptr;
        ReleaseSRWLockExclusive(&state.lock);
        error = "Hooking HackerContext::BeforeDraw failed with code " +
                std::to_string(result);
        return false;
    }
    state.installed = true;
    ReleaseSRWLockExclusive(&state.lock);
    return true;
}

bool NativeBeforeDrawHook::TryRemove(std::string& error) {
    error.clear();
    auto& state = g_native_hook_state;
    if (!TryAcquireSRWLockExclusive(&state.lock)) {
        error = "a BeforeDraw pass-through call is active";
        return false;
    }
    if (!state.installed) {
        ReleaseSRWLockExclusive(&state.lock);
        return true;
    }
    const auto result = state.hook_library.Unhook(state.hook_id);
    if (result != 0) {
        ReleaseSRWLockExclusive(&state.lock);
        error = "Unhooking HackerContext::BeforeDraw failed with code " +
                std::to_string(result);
        return false;
    }
    state.hook_id = 0;
    state.original = nullptr;
    state.callback = nullptr;
    state.callback_context = nullptr;
    state.installed = false;
    ReleaseSRWLockExclusive(&state.lock);
    return true;
}

void* NativeBeforeDrawHook::Original() const {
    return g_native_hook_state.original;
}

bool ResolveBeforeDrawTarget(
    ID3D11DeviceContext1* context,
    const ModuleImage& official_module,
    void*& target,
    std::string& detail) {
    target = nullptr;
    detail.clear();
    if (!context) {
        detail = "HackerContext is null";
        return false;
    }

    const auto ranges = official_module.FunctionRanges();
    const auto find_range = [&ranges](std::uintptr_t address)
        -> std::optional<FunctionRange> {
        for (const auto& range : ranges) {
            if (range.begin <= address && address < range.end)
                return range;
        }
        return std::nullopt;
    };

    std::map<std::uintptr_t, HookCallCandidate> candidates;
    std::size_t validated_functions = 0;
    for (const auto slot : kDrawVtableSlots) {
        void* function = nullptr;
        std::string slot_error;
        if (!ReadVtableSlot(context, slot, function, slot_error) ||
            !official_module.ContainsAddress(
                reinterpret_cast<std::uintptr_t>(function), 1) ||
            !official_module.IsFunctionStart(
                reinterpret_cast<std::uintptr_t>(function))) {
            continue;
        }
        const auto range = find_range(
            reinterpret_cast<std::uintptr_t>(function));
        if (!range)
            continue;
        ++validated_functions;
        const auto calls =
            ReadFunctionCalls(official_module, *range);
        std::map<std::uintptr_t, std::size_t> function_calls;
        for (const auto& [call_target, offset] : calls) {
            const auto [iterator, inserted] =
                function_calls.emplace(call_target, offset);
            if (!inserted && offset < iterator->second)
                iterator->second = offset;
        }
        for (const auto& [call_target, offset] : function_calls) {
            auto& candidate = candidates[call_target];
            candidate.target = call_target;
            ++candidate.occurrences;
            if (candidate.earliest_offset == 0 ||
                offset < candidate.earliest_offset) {
                candidate.earliest_offset = offset;
            }
        }
    }

    std::vector<HookCallCandidate> ordered;
    for (const auto& [ignored, candidate] : candidates) {
        if (candidate.occurrences >= 3 &&
            official_module.IsFunctionStart(candidate.target)) {
            ordered.push_back(candidate);
        }
    }
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const HookCallCandidate& left,
           const HookCallCandidate& right) {
            if (left.occurrences != right.occurrences)
                return left.occurrences > right.occurrences;
            return left.earliest_offset < right.earliest_offset;
        });
    if (ordered.empty()) {
        std::ostringstream stream;
        stream << "no shared direct BeforeDraw call target found; "
               << "validated_draw_functions=" << validated_functions
               << " candidate_count=" << candidates.size();
        detail = stream.str();
        return false;
    }

    target = reinterpret_cast<void*>(ordered.front().target);
    std::ostringstream stream;
    stream << "resolved BeforeDraw target=" << PointerString(target)
           << " occurrences=" << ordered.front().occurrences
           << " first_call_offset=0x" << std::hex
           << ordered.front().earliest_offset
           << " validated_draw_functions=" << std::dec
           << validated_functions;
    if (ordered.size() > 1) {
        stream << " alternatives=";
        for (std::size_t index = 1;
             index < std::min<std::size_t>(ordered.size(), 3);
             ++index) {
            if (index > 1)
                stream << ',';
            stream << PointerString(
                reinterpret_cast<void*>(ordered[index].target));
        }
    }
    detail = stream.str();
    return true;
}

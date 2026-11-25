#pragma once

#include <Windows.h>
#include <d3d11_1.h>

#include <cstdint>
#include <memory>
#include <string>

#include "ProbeTypes.h"

class ModuleImage;

enum class DrawProbeState {
    Idle,
    Armed,
    Capturing,
    Captured,
    NotMatched,
    Failed,
    RemovePending,
};

class BeforeDrawHook {
public:
    using Callback = void (*)(
        void* callback_context,
        void* hacker_context,
        void* draw_context,
        bool after_original);

    virtual ~BeforeDrawHook() = default;

    virtual bool Install(
        void* target,
        Callback callback,
        void* callback_context,
        std::string& error) = 0;
    virtual bool TryRemove(std::string& error) = 0;
    virtual void* Original() const = 0;
};

using DrawProbeCaptureCallback = bool (*)(
    void* capture_context,
    void* drawing_context,
    LiveProbeSnapshot& snapshot,
    std::string& error);

class BeforeDrawProbeController {
public:
    explicit BeforeDrawProbeController(
        BeforeDrawHook& hook,
        DrawProbeCaptureCallback capture_callback = nullptr,
        void* capture_context = nullptr);
    ~BeforeDrawProbeController();

    bool Arm(
        void* official_before_draw,
        void* immediate_context,
        std::uint32_t frame,
        std::string& error);
    DrawProbeState State() const;
    void OnPresent(std::uint32_t frame);
    bool TryRemoveHook(std::string& error);

    bool HasResult() const;
    const LiveProbeSnapshot& Snapshot() const;
    const std::string& Error() const;

private:
    static void HookCallback(
        void* callback_context,
        void* hacker_context,
        void* draw_context,
        bool after_original);
    void HandleHook(
        void* hacker_context,
        void* draw_context,
        bool after_original);
    void HandleForeignDraw(
        void* hacker_context,
        void* draw_context,
        bool after_original);
    void StartDraw(
        LiveProbeSnapshot& snapshot,
        void* hacker_context,
        void* draw_context);
    bool FinishDraw(
        LiveProbeSnapshot& snapshot,
        void* hacker_context,
        void* draw_context);

    BeforeDrawHook& hook_;
    DrawProbeCaptureCallback capture_callback_;
    void* capture_context_ = nullptr;
    void* target_context_ = nullptr;
    std::uint32_t frame_ = 0;
    DrawProbeState state_ = DrawProbeState::Idle;
    DrawProbeState pending_result_state_ = DrawProbeState::Idle;
    LiveProbeSnapshot snapshot_;
    std::string error_;
    LiveProbeSnapshot foreign_snapshot_;
    bool foreign_draw_pending_ = false;
    bool foreign_draw_ready_ = false;
};

class NativeBeforeDrawHook final : public BeforeDrawHook {
public:
    NativeBeforeDrawHook();
    ~NativeBeforeDrawHook() override;

    bool Install(
        void* target,
        Callback callback,
        void* callback_context,
        std::string& error) override;
    bool TryRemove(std::string& error) override;
    void* Original() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool ResolveBeforeDrawTarget(
    ID3D11DeviceContext1* context,
    const ModuleImage& official_module,
    void*& target,
    std::string& detail);

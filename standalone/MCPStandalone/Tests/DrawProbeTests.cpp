#include <string>

#include "DrawProbe.h"
#include "TestHarness.h"

namespace {

class FakeBeforeDrawHook final : public BeforeDrawHook {
public:
    bool Install(
        void* target,
        Callback callback,
        void* callback_context,
        std::string& error) override {
        if (!target || !callback) {
            error = "missing hook target or callback";
            return false;
        }
        target_ = target;
        callback_ = callback;
        callback_context_ = callback_context;
        installed_ = true;
        return true;
    }

    bool TryRemove(std::string& error) override {
        if (active_calls_ != 0) {
            error = "hook call is active";
            return false;
        }
        installed_ = false;
        callback_ = nullptr;
        callback_context_ = nullptr;
        return true;
    }

    void* Original() const override {
        return reinterpret_cast<void*>(0x1234);
    }

    void Invoke(void* context, void* draw) {
        if (!installed_ || !callback_)
            return;
        callback_(callback_context_, context, draw, false);
        callback_(callback_context_, context, draw, true);
    }

    void BeginPassThrough() {
        ++active_calls_;
    }

    void EndPassThrough() {
        --active_calls_;
    }

    bool unhooked() const {
        return !installed_;
    }

private:
    void* target_ = nullptr;
    Callback callback_ = nullptr;
    void* callback_context_ = nullptr;
    bool installed_ = false;
    int active_calls_ = 0;
};

}  // namespace

TEST_CASE(BeforeDrawProbeCapturesOnlyTargetContext) {
    FakeBeforeDrawHook hook;
    BeforeDrawProbeController probe(hook);
    void* target_context = reinterpret_cast<void*>(0x2000);
    void* other_context = reinterpret_cast<void*>(0x3000);
    std::string error;

    REQUIRE(probe.Arm(hook.Original(), target_context, 10, error));
    hook.Invoke(other_context, nullptr);
    REQUIRE(probe.State() == DrawProbeState::Armed);
    hook.Invoke(target_context, nullptr);
    REQUIRE(probe.State() == DrawProbeState::Captured);
}

TEST_CASE(BeforeDrawProbeTimesOutAndUnhooks) {
    FakeBeforeDrawHook hook;
    BeforeDrawProbeController probe(hook);
    std::string error;

    REQUIRE(probe.Arm(
        hook.Original(),
        reinterpret_cast<void*>(0x2000),
        10,
        error));
    probe.OnPresent(11);
    REQUIRE(probe.State() == DrawProbeState::NotMatched);
    REQUIRE(hook.unhooked());
}

TEST_CASE(BeforeDrawProbeWaitsForActivePassThroughCalls) {
    FakeBeforeDrawHook hook;
    BeforeDrawProbeController probe(hook);
    std::string error;

    REQUIRE(probe.Arm(
        hook.Original(),
        reinterpret_cast<void*>(0x2000),
        10,
        error));
    hook.BeginPassThrough();
    probe.OnPresent(11);
    REQUIRE(!hook.unhooked());
    REQUIRE(probe.State() == DrawProbeState::RemovePending);
    hook.EndPassThrough();
    REQUIRE(probe.TryRemoveHook(error));
    REQUIRE(hook.unhooked());
    REQUIRE(probe.State() == DrawProbeState::NotMatched);
}

TEST_CASE(BeforeDrawProbePreservesCapturedResultWhileWaitingForRemoval) {
    FakeBeforeDrawHook hook;
    BeforeDrawProbeController probe(hook);
    std::string error;
    void* target_context = reinterpret_cast<void*>(0x2000);

    REQUIRE(probe.Arm(
        hook.Original(), target_context, 10, error));
    hook.BeginPassThrough();
    hook.Invoke(target_context, nullptr);
    REQUIRE(probe.State() == DrawProbeState::Captured);
    probe.OnPresent(11);
    REQUIRE(probe.State() == DrawProbeState::RemovePending);
    hook.EndPassThrough();
    REQUIRE(probe.TryRemoveHook(error));
    REQUIRE(probe.State() == DrawProbeState::Captured);
}

TEST_CASE(BeforeDrawProbeManualRemoveMarksNotMatched) {
    FakeBeforeDrawHook hook;
    BeforeDrawProbeController probe(hook);
    std::string error;

    REQUIRE(probe.Arm(
        hook.Original(),
        reinterpret_cast<void*>(0x2000),
        10,
        error));
    REQUIRE(probe.TryRemoveHook(error));
    REQUIRE(probe.State() == DrawProbeState::NotMatched);
}

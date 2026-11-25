#include <Windows.h>

#include <cstddef>
#include <cstring>
#include <string>

#include "AdapterTypes.h"
#include "MigotoAdapter.h"
#include "TestHarness.h"

namespace {

int g_shader_reload_calls = 0;
int g_config_reload_calls = 0;
int g_frame_analysis_calls = 0;

struct AnalysisGlobals {
    std::uint32_t hunting = 0;
    std::uint8_t analysis_active = 0;
    std::uint8_t padding[3] = {};
    std::uint32_t analysis_number = 0;
    wchar_t analysis_path[260] = {};
    std::uint8_t reload_config_pending = 0;
};

enum class FrameAnalysisBehavior {
    NoAcknowledgement,
    PathOnly,
    Accepted,
};

AnalysisGlobals* g_analysis_globals = nullptr;
FrameAnalysisBehavior g_frame_analysis_behavior =
    FrameAnalysisBehavior::NoAcknowledgement;

void FakeShaderReload(void*, void*) {
    ++g_shader_reload_calls;
}

void FakeConfigReload(void*) {
    ++g_config_reload_calls;
}

void FakeFrameAnalysis(void*, void*) {
    ++g_frame_analysis_calls;
    if (!g_analysis_globals)
        return;
    if (g_frame_analysis_behavior == FrameAnalysisBehavior::PathOnly) {
        wcscpy_s(
            g_analysis_globals->analysis_path,
            L"D:\\Temp\\FrameAnalysis-path-only");
        return;
    }
    if (g_frame_analysis_behavior == FrameAnalysisBehavior::Accepted) {
        g_analysis_globals->analysis_active = 1;
        wcscpy_s(
            g_analysis_globals->analysis_path,
            L"D:\\Temp\\FrameAnalysis-accepted");
    }
}

ResolvedMigotoCapabilities ReloadCapabilities() {
    ResolvedMigotoCapabilities capabilities;
    capabilities.mode = ResolvedAdapterMode::Partial;
    capabilities.reload_config.available = true;
    capabilities.reload_config.address =
        reinterpret_cast<std::uintptr_t>(&FakeConfigReload);
    capabilities.reload_shaders.available = true;
    capabilities.reload_shaders.address =
        reinterpret_cast<std::uintptr_t>(&FakeShaderReload);
    return capabilities;
}

ResolvedMigotoCapabilities FrameAnalysisCapabilities(
    std::uintptr_t globals) {
    auto capabilities = ReloadCapabilities();
    capabilities.globals = globals;
    capabilities.reload_config_pending_offset =
        static_cast<std::uint32_t>(
            offsetof(AnalysisGlobals, reload_config_pending));
    capabilities.frame_analysis.available = true;
    capabilities.frame_analysis.address =
        reinterpret_cast<std::uintptr_t>(&FakeFrameAnalysis);
    capabilities.hunting_offset =
        static_cast<std::uint32_t>(offsetof(AnalysisGlobals, hunting));
    capabilities.analyse_frame_offset =
        static_cast<std::uint32_t>(
            offsetof(AnalysisGlobals, analysis_active));
    return capabilities;
}

void SetReadyForTests(
    MigotoAdapter& adapter,
    const ResolvedMigotoCapabilities& capabilities,
    void* device = reinterpret_cast<void*>(0x1234)) {
    adapter.SetTestRuntime(capabilities, device);
    adapter.SetTestCaptureState(true, true, AdapterCaptureState::Ready);
}

}  // namespace

TEST_CASE(ControlDispatchRunsShaderReloadOnRenderThreadSeam) {
    g_shader_reload_calls = 0;
    MigotoAdapter adapter;
    SetReadyForTests(adapter, ReloadCapabilities());

    std::string detail;
    REQUIRE(adapter.QueueCommand(
        90, ControlCommand::ReloadShaders, detail));
    adapter.ProcessPendingForTests();

    CommandResult result;
    REQUIRE(adapter.WaitCommand(90, 100, result) ==
            QueueWaitResult::Completed);
    REQUIRE_EQ(result.status, 0U);
    REQUIRE_EQ(g_shader_reload_calls, 1);
    REQUIRE(result.payload.find("result=ok") != std::string::npos);
}

TEST_CASE(ControlDispatchRejectsBusyQueue) {
    MigotoAdapter adapter;
    SetReadyForTests(adapter, ReloadCapabilities());
    std::string detail;
    REQUIRE(adapter.QueueCommand(
        91, ControlCommand::ReloadShaders, detail));
    REQUIRE(!adapter.QueueCommand(
        92, ControlCommand::ReloadShaders, detail));
    REQUIRE(!detail.empty());
}

TEST_CASE(ControlDispatchQueuesLiveContextAndFailsWithoutSnapshotRoot) {
    MigotoAdapter adapter;
    SetReadyForTests(adapter, ReloadCapabilities());

    std::string detail;
    REQUIRE(adapter.QueueCommand(
        101, ControlCommand::LiveContext, detail));
    adapter.ProcessPendingForTests();

    CommandResult result;
    REQUIRE(adapter.WaitCommand(101, 100, result) ==
            QueueWaitResult::Completed);
    REQUIRE_EQ(result.status, 4U);
    REQUIRE(
        result.payload.find("snapshot_directory_unavailable") !=
        std::string::npos);
}

TEST_CASE(ControlDispatchQueuesRuntimeVariablesAndFailsWithoutSnapshotRoot) {
    MigotoAdapter adapter;
    SetReadyForTests(adapter, ReloadCapabilities());

    std::string detail;
    REQUIRE(adapter.QueueCommand(
        102, ControlCommand::RuntimeVariables, detail));
    adapter.ProcessPendingForTests();

    CommandResult result;
    REQUIRE(adapter.WaitCommand(102, 100, result) ==
            QueueWaitResult::Completed);
    REQUIRE_EQ(result.status, 4U);
    REQUIRE(result.payload.find("runtime_variables_unavailable") !=
            std::string::npos);
}

TEST_CASE(ControlDispatchQueuesConfigReloadThroughPendingFlag) {
    g_config_reload_calls = 0;
    AnalysisGlobals globals;
    auto capabilities = ReloadCapabilities();
    capabilities.globals =
        reinterpret_cast<std::uintptr_t>(&globals);
    capabilities.reload_config_pending_offset =
        static_cast<std::uint32_t>(
            offsetof(AnalysisGlobals, reload_config_pending));
    MigotoAdapter adapter;
    SetReadyForTests(adapter, capabilities);

    std::string detail;
    REQUIRE(adapter.QueueCommand(
        93, ControlCommand::ReloadConfig, detail));
    adapter.ProcessPendingForTests();
    CommandResult result;
    REQUIRE(adapter.WaitCommand(93, 100, result) ==
            QueueWaitResult::Completed);
    REQUIRE_EQ(result.status, 0U);
    REQUIRE_EQ(globals.reload_config_pending, 1U);
    REQUIRE_EQ(g_config_reload_calls, 0);
}

TEST_CASE(ControlDispatchRejectsConfigReloadWithoutPendingField) {
    MigotoAdapter adapter;
    SetReadyForTests(adapter, ReloadCapabilities());

    std::string detail;
    REQUIRE(!adapter.QueueCommand(
        95, ControlCommand::ReloadConfig, detail));
    REQUIRE(detail.find("pending field") != std::string::npos);
}

TEST_CASE(ControlDispatchReportsMissingCapability) {
    MigotoAdapter adapter;
    SetReadyForTests(adapter, ReloadCapabilities());
    std::string detail;
    REQUIRE(!adapter.QueueCommand(
        94, ControlCommand::FrameAnalysis, detail));
    REQUIRE(detail.find("frame_analysis") != std::string::npos);
}

TEST_CASE(ControlDispatchRejectsFrameAnalysisWithoutPostCallbackAck) {
    AnalysisGlobals globals;
    globals.hunting = 2;
    g_analysis_globals = &globals;
    g_frame_analysis_behavior = FrameAnalysisBehavior::NoAcknowledgement;
    g_frame_analysis_calls = 0;

    MigotoAdapter adapter;
    SetReadyForTests(
        adapter,
        FrameAnalysisCapabilities(
            reinterpret_cast<std::uintptr_t>(&globals)));

    std::string detail;
    REQUIRE(adapter.QueueCommand(
        96, ControlCommand::FrameAnalysis, detail));
    adapter.ProcessPendingForTests();
    adapter.ProcessPostPresentForTests();

    CommandResult result;
    REQUIRE(adapter.WaitCommand(96, 100, result) ==
            QueueWaitResult::Completed);
    REQUIRE_EQ(result.status, 5U);
    REQUIRE(result.payload.find("frame_analysis_not_accepted") !=
            std::string::npos);
    REQUIRE_EQ(g_frame_analysis_calls, 1);
    REQUIRE_EQ(globals.hunting, 2U);
    REQUIRE_EQ(adapter.Snapshot().capture_restore_hunting, -1);
}

TEST_CASE(ControlDispatchRejectsFrameAnalysisWithPathButInactive) {
    AnalysisGlobals globals;
    globals.hunting = 1;
    g_analysis_globals = &globals;
    g_frame_analysis_behavior = FrameAnalysisBehavior::PathOnly;
    g_frame_analysis_calls = 0;

    MigotoAdapter adapter;
    SetReadyForTests(
        adapter,
        FrameAnalysisCapabilities(
            reinterpret_cast<std::uintptr_t>(&globals)));

    std::string detail;
    REQUIRE(adapter.QueueCommand(
        97, ControlCommand::FrameAnalysis, detail));
    adapter.ProcessPendingForTests();
    adapter.ProcessPostPresentForTests();

    CommandResult result;
    REQUIRE(adapter.WaitCommand(97, 100, result) ==
            QueueWaitResult::Completed);
    REQUIRE_EQ(result.status, 5U);
    REQUIRE(result.payload.find("frame_analysis_not_accepted") !=
            std::string::npos);
    REQUIRE_EQ(g_frame_analysis_calls, 1);
}

TEST_CASE(ControlDispatchAcknowledgesFrameAnalysisAndReportsPath) {
    AnalysisGlobals globals;
    globals.hunting = 2;
    g_analysis_globals = &globals;
    g_frame_analysis_behavior = FrameAnalysisBehavior::Accepted;
    g_frame_analysis_calls = 0;

    MigotoAdapter adapter;
    SetReadyForTests(
        adapter,
        FrameAnalysisCapabilities(
            reinterpret_cast<std::uintptr_t>(&globals)));

    std::string detail;
    REQUIRE(adapter.QueueCommand(
        98, ControlCommand::FrameAnalysis, detail));
    adapter.ProcessPendingForTests();
    adapter.ProcessPostPresentForTests();

    CommandResult result;
    REQUIRE(adapter.WaitCommand(98, 100, result) ==
            QueueWaitResult::Completed);
    REQUIRE_EQ(result.status, 0U);
    REQUIRE(result.payload.find("result=accepted") != std::string::npos);
    REQUIRE(result.payload.find(
                "analysis_path=D:\\Temp\\FrameAnalysis-accepted") !=
            std::string::npos);
    REQUIRE(result.payload.find("analyse_options=unresolved") !=
            std::string::npos);
    REQUIRE_EQ(globals.hunting, 1U);

    const auto snapshot = adapter.Snapshot();
    REQUIRE_EQ(snapshot.capture_restore_hunting, 2);
    REQUIRE_EQ(
        snapshot.analysis_path,
        std::string("D:\\Temp\\FrameAnalysis-accepted"));
}

TEST_CASE(ControlDispatchFrameAnalysisFailsBeforeCallbackWhenGlobalsUnreadable) {
    g_analysis_globals = nullptr;
    g_frame_analysis_calls = 0;

    MigotoAdapter adapter;
    SetReadyForTests(adapter, FrameAnalysisCapabilities(0x1));

    std::string detail;
    REQUIRE(adapter.QueueCommand(
        99, ControlCommand::FrameAnalysis, detail));
    adapter.ProcessPendingForTests();
    adapter.ProcessPostPresentForTests();

    CommandResult result;
    REQUIRE(adapter.WaitCommand(99, 100, result) ==
            QueueWaitResult::Completed);
    REQUIRE_EQ(result.status, 5U);
    REQUIRE(result.payload.find("frame_analysis_hunting_unreadable") !=
            std::string::npos);
    REQUIRE_EQ(g_frame_analysis_calls, 0);
}

TEST_CASE(ControlDispatchDefersFrameAnalysisUntilPostPresentSeam) {
    AnalysisGlobals globals;
    globals.hunting = 2;
    g_analysis_globals = &globals;
    g_frame_analysis_behavior = FrameAnalysisBehavior::Accepted;
    g_frame_analysis_calls = 0;

    MigotoAdapter adapter;
    SetReadyForTests(
        adapter,
        FrameAnalysisCapabilities(
            reinterpret_cast<std::uintptr_t>(&globals)));

    std::string detail;
    REQUIRE(adapter.QueueCommand(
        104, ControlCommand::FrameAnalysis, detail));

    // The pre-present seam must only defer the request: arming here would be
    // cancelled by the same present's RunFrameActions().
    adapter.ProcessPendingForTests();
    REQUIRE_EQ(g_frame_analysis_calls, 0);
    REQUIRE_EQ(globals.hunting, 2U);

    adapter.ProcessPostPresentForTests();
    REQUIRE_EQ(g_frame_analysis_calls, 1);
    REQUIRE_EQ(globals.hunting, 1U);

    CommandResult result;
    REQUIRE(adapter.WaitCommand(104, 100, result) ==
            QueueWaitResult::Completed);
    REQUIRE_EQ(result.status, 0U);
    REQUIRE(result.payload.find("result=accepted") != std::string::npos);
}

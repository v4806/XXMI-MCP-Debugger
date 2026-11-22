#include "AdapterTypes.h"
#include "MigotoAdapter.h"
#include "TestHarness.h"

TEST_CASE(MigotoAdapterDefaultsToReadOnlySnapshot) {
    MigotoAdapter adapter;
    const auto snapshot = adapter.Snapshot();
    REQUIRE(snapshot.mode == AdapterSnapshotMode::ReadOnly);
    REQUIRE(!snapshot.adapter_ready);
    REQUIRE(!snapshot.device_captured);
    REQUIRE(!snapshot.present_hooked);
    REQUIRE(snapshot.capture_state == AdapterCaptureState::NotStarted);
    REQUIRE_EQ(snapshot.frame, 0U);
    REQUIRE_EQ(snapshot.fps, 0.0);
    REQUIRE(!snapshot.hunting.has_value());
    REQUIRE(!snapshot.analysis_active.has_value());
    REQUIRE(snapshot.analysis_path.empty());
    REQUIRE_EQ(snapshot.capture_restore_hunting, -1);
    REQUIRE(snapshot.capabilities.empty());
}

TEST_CASE(MigotoAdapterDoesNotReportLiveModeFromSignaturesOnly) {
    ResolvedMigotoCapabilities capabilities;
    capabilities.mode = ResolvedAdapterMode::Full;
    capabilities.frame_analysis.available = true;
    capabilities.reload_config.available = true;
    capabilities.reload_shaders.available = true;

    MigotoAdapter adapter;
    adapter.SetTestRuntime(capabilities, nullptr);
    adapter.SetTestCaptureState(
        false, false, AdapterCaptureState::WaitingForDevice);

    const auto snapshot = adapter.Snapshot();
    REQUIRE(snapshot.mode == AdapterSnapshotMode::ReadOnly);
    REQUIRE(!snapshot.adapter_ready);
    REQUIRE(!snapshot.device_captured);
    REQUIRE(!snapshot.present_hooked);
    REQUIRE(snapshot.capture_state == AdapterCaptureState::WaitingForDevice);
    REQUIRE(snapshot.capabilities.empty());
}

TEST_CASE(MigotoAdapterReportsDeviceWithoutPresentAsNotReady) {
    MigotoAdapter adapter;
    adapter.SetTestCaptureState(
        true, false, AdapterCaptureState::WaitingForPresent);

    const auto snapshot = adapter.Snapshot();
    REQUIRE(!snapshot.adapter_ready);
    REQUIRE(snapshot.device_captured);
    REQUIRE(!snapshot.present_hooked);
    REQUIRE(snapshot.capture_state == AdapterCaptureState::WaitingForPresent);
}

TEST_CASE(MigotoAdapterReportsCaptureTimeoutWithoutLiveMode) {
    MigotoAdapter adapter;
    adapter.SetTestCaptureState(
        false, false, AdapterCaptureState::TimedOut);

    const auto snapshot = adapter.Snapshot();
    REQUIRE(snapshot.mode == AdapterSnapshotMode::ReadOnly);
    REQUIRE(!snapshot.adapter_ready);
    REQUIRE(snapshot.capture_state == AdapterCaptureState::TimedOut);
}

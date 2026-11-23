#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "ProbeJson.h"
#include "ProbeTypes.h"
#include "TestHarness.h"

TEST_CASE(ProbeJsonEscapesStringsAndReportsMissingFields) {
    LiveProbeSnapshot snapshot;
    snapshot.kind = "live_draw";
    snapshot.config_path = "C:\\game\\quote\"path";
    snapshot.missing_fields = {"vs.hash"};

    const auto json = SerializeProbeSnapshot(snapshot);

    REQUIRE(json.find("C:\\\\game\\\\quote\\\"path") != std::string::npos);
    REQUIRE(
        json.find("\"missing_fields\":[\"vs.hash\"]") !=
        std::string::npos);
}

TEST_CASE(ProbeJsonReportsCollectionLimits) {
    LiveProbeSnapshot snapshot;
    snapshot.resources.resize(129);

    const auto json = SerializeProbeSnapshot(snapshot);

    REQUIRE(
        json.find("\"limits_hit\":[\"resources\"]") !=
        std::string::npos);
    REQUIRE(json.find("\"resources\":[") != std::string::npos);
}

TEST_CASE(ProbeJsonWritesAndReadsSnapshotFile) {
    LiveProbeSnapshot snapshot;
    snapshot.kind = "live_context";
    snapshot.frame = 42;

    const auto path =
        std::filesystem::temp_directory_path() /
        L"xxmi-probe-json-test.json";
    std::string error;
    REQUIRE(WriteProbeSnapshot(path, snapshot, error));
    REQUIRE(error.empty());

    std::ifstream stream(path, std::ios::binary);
    const std::string contents(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    REQUIRE(
        contents.find("\"frame\":42") !=
        std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

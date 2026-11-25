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

TEST_CASE(ProbeJsonKeepsUtf8AndEscapesStrayBytes) {
    const std::string utf8_name(
        "\xE7\xBB\x9D\xE5\x8C\xBA", 6);
    const std::string raw_mask("read=", 5);
    const char raw_byte[] = {static_cast<char>(0xFF), '\0'};
    LiveProbeSnapshot snapshot;
    snapshot.kind = "live_context";
    snapshot.config_path = utf8_name;
    snapshot.states.push_back(
        {"stencil_mask", raw_mask + raw_byte});

    const auto json = SerializeProbeSnapshot(snapshot);

    REQUIRE(json.find(utf8_name) != std::string::npos);
    REQUIRE(json.find("read=\\u00ff") != std::string::npos);
    REQUIRE(json.find(raw_mask + raw_byte) == std::string::npos);
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

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "RuntimeVariables.h"
#include "TestHarness.h"

namespace {

class TemporaryRuntimeDirectory {
public:
    TemporaryRuntimeDirectory()
        : path_(
              std::filesystem::temp_directory_path() /
              ("xxmi-runtime-variables-" +
               std::to_string(GetCurrentProcessId()) + "-" +
               std::to_string(GetTickCount64()))) {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryRuntimeDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void WriteFile(
    const std::filesystem::path& path,
    const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
}

}  // namespace

TEST_CASE(RuntimeVariablesParseUserConfigValues) {
    TemporaryRuntimeDirectory directory;
    WriteFile(
        directory.path() / "d3dx.ini",
        "[System]\n"
        "clear_unknown_settings = 0\n"
        "[Rendering]\n"
        "ini_params = 7\n");
    WriteFile(
        directory.path() / "d3dx_user.ini",
        "; generated\n"
        "[Constants]\n"
        "\\mods\\test.ini\\visible = 1.000000\n"
        "$global\\fault = 0.125\n");

    RuntimeConfigSnapshot snapshot;
    std::string error;
    REQUIRE(ReadRuntimeConfig(directory.path(), snapshot, error));
    REQUIRE(error.empty());
    REQUIRE_EQ(snapshot.persistent_values.size(), 2U);
    REQUIRE_EQ(
        snapshot.persistent_values[0].name,
        std::string("\\mods\\test.ini\\visible"));
    REQUIRE(snapshot.persistent_values[0].value.has_value());
    REQUIRE_EQ(*snapshot.persistent_values[0].value, 1.0);
    REQUIRE_EQ(
        snapshot.persistent_values[1].name,
        std::string("$global\\fault"));
    REQUIRE_EQ(*snapshot.persistent_values[1].value, 0.125);
    REQUIRE(snapshot.ini_params_register.has_value());
    REQUIRE_EQ(*snapshot.ini_params_register, 7);
    REQUIRE(snapshot.ini_params_register_valid);
    REQUIRE(snapshot.clear_unknown_settings.has_value());
    REQUIRE(!*snapshot.clear_unknown_settings);
}

TEST_CASE(RuntimeVariablesReportUnknownSettingsWithoutDuplicates) {
    TemporaryRuntimeDirectory directory;
    const auto log_path = directory.path() / "d3d11_log.txt";
    WriteFile(
        log_path,
        "NOTICE: Detected 1 unknown user settings in d3dx_user.ini\n"
        " The first unrecognised entry was: \"$stale = 1\"\n"
        "Unknown settings clean-up will remove them on next reload\n"
        "NOTICE: Detected 1 unknown user settings in d3dx_user.ini\n");

    REQUIRE_EQ(CountUnknownSettings(log_path), 1U);
    RuntimeLogSummary summary;
    std::string error;
    REQUIRE(ReadRuntimeLog(log_path, summary, error));
    REQUIRE_EQ(summary.unknown_setting_count, 1U);
    REQUIRE_EQ(
        summary.first_unknown_setting,
        std::string("$stale = 1"));
    REQUIRE(!summary.warnings.empty());
}

TEST_CASE(RuntimeVariablesSerializeValuesAndExcerpts) {
    RuntimeConfigSnapshot snapshot;
    snapshot.persistent_values.push_back(
        {"\\mods\\test.ini\\visible", 1.0, "1.000000"});
    snapshot.log_summary.warnings = {
        "line one",
        "quote \"line\"",
    };
    const auto json = SerializeRuntimeVariables(snapshot);
    REQUIRE(json.find("\"kind\":\"runtime_variables\"") !=
            std::string::npos);
    REQUIRE(json.find(
                "\"name\":\"\\\\mods\\\\test.ini\\\\visible\"") !=
            std::string::npos);
    REQUIRE(json.find("\"value\":1") != std::string::npos);
    REQUIRE(json.find("quote \\\"line\\\"") !=
            std::string::npos);
}

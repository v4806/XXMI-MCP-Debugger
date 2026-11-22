#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "DiagnosticLog.h"
#include "TestHarness.h"

namespace {

std::filesystem::path MakeTempDirectory() {
    const auto suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto path = std::filesystem::temp_directory_path() /
                ("xxmi-mcp-log-test-" + suffix);
    std::filesystem::create_directories(path);
    return path;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE(DiagnosticLogWritesMessagesAndCountsErrors) {
    const auto directory = MakeTempDirectory();
    const auto path = directory / "xxmi_mcp.log";
    DiagnosticLog::Initialize(path, 1024);
    DiagnosticLog::Info("adapter initialized");
    DiagnosticLog::Warn("anchor missing");
    DiagnosticLog::Error("reload failed");
    DiagnosticLog::Shutdown();

    const auto contents = ReadFile(path);
    REQUIRE(contents.find("[INFO] adapter initialized") != std::string::npos);
    REQUIRE(contents.find("[WARN] anchor missing") != std::string::npos);
    REQUIRE(contents.find("[ERROR] reload failed") != std::string::npos);
    REQUIRE_EQ(DiagnosticLog::ErrorCount(), 1U);
    REQUIRE_EQ(DiagnosticLog::Path(), path);
    std::filesystem::remove_all(directory);
}

TEST_CASE(DiagnosticLogRotatesAtConfiguredLimit) {
    const auto directory = MakeTempDirectory();
    const auto path = directory / "xxmi_mcp.log";
    DiagnosticLog::Initialize(path, 64);
    DiagnosticLog::Info("0123456789012345678901234567890123456789");
    DiagnosticLog::Info("new-file-marker");
    DiagnosticLog::Shutdown();

    REQUIRE(std::filesystem::exists(path.string() + ".1"));
    REQUIRE(ReadFile(path).find("new-file-marker") != std::string::npos);
    std::filesystem::remove_all(directory);
}

#include <filesystem>
#include <fstream>
#include <string>

#include "Hash.h"
#include "TestHarness.h"

TEST_CASE(Sha256KnownVector) {
    const auto path =
        std::filesystem::temp_directory_path() / "xxmi-mcp-sha256-test.txt";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "abc";
    }

    std::string digest;
    std::string error;
    REQUIRE(ComputeFileSha256(path, digest, error));
    REQUIRE_EQ(
        digest,
        std::string(
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad"));
    std::filesystem::remove(path);
}

TEST_CASE(Sha256ReportsMissingFile) {
    std::string digest;
    std::string error;
    REQUIRE(!ComputeFileSha256(
        "Z:\\definitely-missing\\xxmi.dll",
        digest,
        error));
    REQUIRE(!error.empty());
}

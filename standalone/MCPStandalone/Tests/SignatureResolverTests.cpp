#include <array>
#include <cstdint>
#include <vector>

#include "SignatureResolver.h"
#include "TestHarness.h"

namespace {

constexpr std::uint8_t kAny = 0xCC;

}  // namespace

TEST_CASE(MaskedScanFindsUniquePattern) {
    const std::array<std::uint8_t, 12> bytes = {
        0x10, 0x20, 0x30, 0x44, 0x50, 0x60,
        0x11, 0x20, 0x30, 0x44, 0x50, 0x61,
    };
    const std::array<std::uint8_t, 5> pattern = {0x10, 0x20, 0x30, 0x44, 0x50};
    const std::array<std::uint8_t, 5> mask = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    const auto result = ScanMasked(bytes, 0x1000, pattern, mask);
    REQUIRE(result.state == MatchState::Found);
    REQUIRE_EQ(result.address, 0x1000U);
    REQUIRE_EQ(result.candidates.size(), 1U);
}

TEST_CASE(MaskedScanSupportsWildcards) {
    const std::array<std::uint8_t, 8> bytes = {0xAA, 0xBB, 0x10, 0x99, 0x30, 0x40, 0xBB, 0xCC};
    const std::array<std::uint8_t, 4> pattern = {0x10, 0x00, 0x30, 0x40};
    const std::array<std::uint8_t, 4> mask = {0xFF, kAny, 0xFF, 0xFF};

    const auto result = ScanMasked(bytes, 0x2000, pattern, mask);
    REQUIRE(result.state == MatchState::Found);
    REQUIRE_EQ(result.address, 0x2002U);
}

TEST_CASE(MaskedScanRejectsDuplicatePatterns) {
    const std::array<std::uint8_t, 10> bytes = {
        0x01, 0x02, 0x03, 0x04, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x00,
    };
    const std::array<std::uint8_t, 4> pattern = {0x01, 0x02, 0x03, 0x04};
    const std::array<std::uint8_t, 4> mask = {0xFF, 0xFF, 0xFF, 0xFF};

    const auto result = ScanMasked(bytes, 0x3000, pattern, mask);
    REQUIRE(result.state == MatchState::Ambiguous);
    REQUIRE_EQ(result.candidates.size(), 2U);
}

TEST_CASE(MaskedScanReportsMissingPattern) {
    const std::array<std::uint8_t, 8> bytes = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::array<std::uint8_t, 3> pattern = {9, 8, 7};
    const std::array<std::uint8_t, 3> mask = {0xFF, 0xFF, 0xFF};

    const auto result = ScanMasked(bytes, 0x4000, pattern, mask);
    REQUIRE(result.state == MatchState::Missing);
    REQUIRE(result.candidates.empty());
}

TEST_CASE(MaskedScanRejectsMismatchedMaskLength) {
    const std::array<std::uint8_t, 4> bytes = {1, 2, 3, 4};
    const std::array<std::uint8_t, 2> pattern = {1, 2};
    const std::array<std::uint8_t, 1> mask = {0xFF};

    const auto result = ScanMasked(bytes, 0x5000, pattern, mask);
    REQUIRE(result.state == MatchState::Invalid);
    REQUIRE(!result.detail.empty());
}

TEST_CASE(ResolveRipRelativeComputesTarget) {
    const std::array<std::uint8_t, 7> instruction = {
        0x48, 0x8B, 0x05, 0x78, 0x56, 0x34, 0x12,
    };

    const auto result = ResolveRipRelative(0x1000, instruction, 3);
    REQUIRE(result.state == MatchState::Found);
    REQUIRE_EQ(result.address, 0x1234667FU);
}

TEST_CASE(ResolveRipRelativeRejectsOutOfRangeDisplacement) {
    const std::array<std::uint8_t, 4> instruction = {0x48, 0x8B, 0x05, 0x00};
    const auto result = ResolveRipRelative(0x1000, instruction, 3);
    REQUIRE(result.state == MatchState::Invalid);
}

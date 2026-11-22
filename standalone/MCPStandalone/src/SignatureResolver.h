#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

enum class MatchState {
    Found,
    Missing,
    Ambiguous,
    Invalid,
};

struct MatchResult {
    MatchState state = MatchState::Missing;
    std::uintptr_t address = 0;
    std::vector<std::uintptr_t> candidates;
    std::string detail;
};

MatchResult ScanMasked(std::span<const std::uint8_t> bytes,
                       std::uintptr_t base_address,
                       std::span<const std::uint8_t> pattern,
                       std::span<const std::uint8_t> mask);

MatchResult ResolveRipRelative(std::uintptr_t instruction_address,
                               std::span<const std::uint8_t> instruction,
                               std::uint8_t displacement_offset);

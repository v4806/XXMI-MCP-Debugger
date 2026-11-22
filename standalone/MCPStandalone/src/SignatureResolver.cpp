#include "SignatureResolver.h"

#include <cstring>
#include <limits>

namespace {

constexpr std::uint8_t kExactMask = 0xFF;
constexpr std::uint8_t kWildcardMask = 0xCC;

bool MatchesAt(std::span<const std::uint8_t> bytes, std::size_t offset,
               std::span<const std::uint8_t> pattern,
               std::span<const std::uint8_t> mask) {
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const auto mask_byte = mask[index];
        if (mask_byte == kWildcardMask)
            continue;
        if ((bytes[offset + index] & mask_byte) !=
            (pattern[index] & mask_byte)) {
            return false;
        }
    }
    return true;
}

}  // namespace

MatchResult ScanMasked(std::span<const std::uint8_t> bytes,
                       std::uintptr_t base_address,
                       std::span<const std::uint8_t> pattern,
                       std::span<const std::uint8_t> mask) {
    MatchResult result;
    if (pattern.empty() || pattern.size() != mask.size() ||
        pattern.size() > bytes.size()) {
        result.state = MatchState::Invalid;
        result.detail =
            "signature or mask length is invalid for the searched range";
        return result;
    }

    const auto last_offset = bytes.size() - pattern.size();
    for (std::size_t offset = 0; offset <= last_offset; ++offset) {
        if (!MatchesAt(bytes, offset, pattern, mask))
            continue;
        result.candidates.push_back(
            base_address + static_cast<std::uintptr_t>(offset));
    }

    if (result.candidates.empty()) {
        result.state = MatchState::Missing;
        result.detail = "signature not found";
    } else if (result.candidates.size() > 1) {
        result.state = MatchState::Ambiguous;
        result.detail = "signature matched more than one location";
    } else {
        result.state = MatchState::Found;
        result.address = result.candidates.front();
        result.detail = "signature matched uniquely";
    }
    return result;
}

MatchResult ResolveRipRelative(std::uintptr_t instruction_address,
                               std::span<const std::uint8_t> instruction,
                               std::uint8_t displacement_offset) {
    MatchResult result;
    if (static_cast<std::size_t>(displacement_offset) + sizeof(std::int32_t) >
        instruction.size()) {
        result.state = MatchState::Invalid;
        result.detail = "RIP displacement lies outside the instruction";
        return result;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, instruction.data() + displacement_offset,
                sizeof(displacement));

    const auto next_instruction =
        instruction_address + displacement_offset + sizeof(displacement);
    const auto target =
        next_instruction + static_cast<std::intptr_t>(displacement);
    result.state = MatchState::Found;
    result.address = target;
    result.candidates.push_back(target);
    result.detail = "RIP-relative target resolved";
    return result;
}

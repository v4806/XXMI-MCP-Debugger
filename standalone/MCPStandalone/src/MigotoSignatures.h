#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ModuleImage.h"
#include "SignatureResolver.h"

enum class MigotoTarget {
    AnalyseFrame,
    ReloadConfig,
    ReloadFixes,
    Present,
    Present1,
};

struct MaskedSignature {
    std::string_view name;
    MigotoTarget target = MigotoTarget::AnalyseFrame;
    std::span<const std::uint8_t> pattern;
    std::span<const std::uint8_t> mask;
};

struct GlobalsLoadSignature {
    std::span<const std::uint8_t> pattern;
    std::span<const std::uint8_t> mask;
    std::uint8_t displacement_offset = 3;
};

struct ResolvedFunction {
    bool available = false;
    std::uintptr_t address = 0;
    std::size_t size = 0;
    std::string detail;
};

struct ResolverDiagnostic {
    std::string message;
    bool error = false;
};

enum class ResolvedAdapterMode {
    ReadOnly,
    Partial,
    Full,
};

struct ResolvedMigotoCapabilities {
    ResolvedAdapterMode mode = ResolvedAdapterMode::ReadOnly;
    ResolvedFunction frame_analysis;
    ResolvedFunction reload_config;
    ResolvedFunction reload_shaders;
    ResolvedFunction present;
    ResolvedFunction present1;
    std::optional<std::uintptr_t> globals;
    std::uint32_t hunting_offset = 0;
    std::uint32_t analyse_frame_offset = 0;
    std::uint32_t reload_config_pending_offset = 0;
    std::vector<ResolverDiagnostic> diagnostics;
};

std::span<const MaskedSignature> GetMigotoFunctionSignatures();
const GlobalsLoadSignature& GetGlobalsLoadSignature();
std::string DescribeMatchResult(
    std::string_view name,
    const MatchResult& match,
    std::uintptr_t image_base);
ResolvedMigotoCapabilities ResolveMigotoCapabilities(const ModuleImage& image);

#include "MigotoSignatures.h"

#include <array>
#include <cstring>
#include <limits>
#include <sstream>

#include "MigotoSignaturesData.h"
#include "SignatureResolver.h"

namespace {

constexpr std::array<std::uint8_t, 7> kHuntingPattern = {
    0x83, 0xB8, 0, 0, 0, 0, 0x01,
};
constexpr std::array<std::uint8_t, 7> kHuntingMask = {
    0xFF, 0xFF, 0xCC, 0xCC, 0xCC, 0xCC, 0xFF,
};
constexpr std::array<std::uint8_t, 7> kAnalysePattern = {
    0xC6, 0x80, 0, 0, 0, 0, 0x01,
};
constexpr std::array<std::uint8_t, 7> kAnalyseMask = {
    0xFF, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xFF,
};
constexpr std::array<std::uint8_t, 6> kReloadPendingPattern = {
    0x40, 0x88, 0x70, 0, 0x89, 0xB0,
};
constexpr std::array<std::uint8_t, 6> kReloadPendingMask = {
    0xFF, 0xFF, 0xFF, 0xCC, 0xFF, 0xFF,
};

std::string Hex(std::uintptr_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

void AddDiagnostic(
    std::vector<ResolverDiagnostic>& diagnostics,
    std::string message,
    bool error) {
    diagnostics.push_back({std::move(message), error});
}

std::string SectionName(
    const ModuleImage& image,
    std::optional<std::uintptr_t> address) {
    if (!address)
        return "unresolved";
    const auto section = image.SectionContaining(*address);
    return section ? section->name : "outside_module";
}

std::string DescribeGlobalsEvidence(
    const ModuleImage& image,
    std::optional<std::uintptr_t> instruction,
    std::optional<std::uintptr_t> target,
    std::optional<std::uintptr_t> loaded,
    std::string_view validation) {
    std::ostringstream stream;
    stream << "Globals: slot=RIP+3";
    if (instruction)
        stream << " instruction=" << Hex(*instruction);
    else
        stream << " instruction=unresolved";
    if (target)
        stream << " target=" << Hex(*target);
    else
        stream << " target=unresolved";
    stream << " target_section="
           << SectionName(image, target);
    if (loaded)
        stream << " loaded=" << Hex(*loaded);
    else
        stream << " loaded=unresolved";
    stream << " loaded_section="
           << SectionName(image, loaded)
           << " module_section="
           << SectionName(image, loaded)
           << " validation=" << validation;
    return stream.str();
}

ResolvedFunction ResolveFunction(
    const ModuleImage& image,
    const MaskedSignature& signature,
    std::vector<ResolverDiagnostic>& diagnostics) {
    ResolvedFunction resolved;
    const auto text = image.Section(".text");
    if (!text) {
        resolved.detail = "PE image has no .text section";
        AddDiagnostic(
            diagnostics,
            std::string(signature.name) + ": " + resolved.detail,
            true);
        return resolved;
    }

    const auto match = ScanMasked(text->bytes, text->address,
                                  signature.pattern, signature.mask);
    if (match.state != MatchState::Found) {
        resolved.detail = match.detail;
        AddDiagnostic(
            diagnostics,
            DescribeMatchResult(
                signature.name, match, image.Base()),
            true);
        return resolved;
    }
    if (!image.IsFunctionStart(match.address)) {
        resolved.detail =
            "signature matched an address outside a .pdata function start";
        AddDiagnostic(
            diagnostics,
            std::string(signature.name) + " at " +
                Hex(match.address) + ": " + resolved.detail,
            true);
        return resolved;
    }

    const auto function = image.FunctionContaining(match.address);
    if (!function) {
        resolved.detail = ".pdata function range is missing";
        AddDiagnostic(
            diagnostics,
            std::string(signature.name) + ": " + resolved.detail,
            true);
        return resolved;
    }

    resolved.available = true;
    resolved.address = match.address;
    resolved.size = static_cast<std::size_t>(function->end - function->begin);
    resolved.detail = "resolved at " + Hex(match.address);
    AddDiagnostic(
        diagnostics,
        std::string(signature.name) + ": " + resolved.detail,
        false);
    return resolved;
}

bool ExtractDisp32(
    const ModuleImage& image,
    const ResolvedFunction& function,
    std::span<const std::uint8_t> pattern,
    std::span<const std::uint8_t> mask,
    std::uint32_t& value,
    std::string& detail) {
    if (!function.available) {
        detail = "target function is unavailable";
        return false;
    }
    if (function.size < pattern.size()) {
        detail = "function is too small for field pattern";
        return false;
    }

    std::vector<std::uint8_t> bytes(function.size);
    if (!image.Read(function.address, bytes.data(), bytes.size())) {
        detail = "could not read function bytes";
        return false;
    }

    const auto match = ScanMasked(bytes, function.address, pattern, mask);
    if (match.state != MatchState::Found) {
        detail = match.detail;
        return false;
    }
    const auto offset = static_cast<std::size_t>(match.address - function.address);
    if (offset + 2 + sizeof(value) > bytes.size()) {
        detail = "field displacement lies outside function";
        return false;
    }
    std::memcpy(&value, bytes.data() + offset + 2, sizeof(value));
    if (value == 0 ||
        value > std::numeric_limits<std::uint32_t>::max() / 2) {
        detail = "field displacement is not plausible";
        return false;
    }
    detail = "field offset " + Hex(value);
    return true;
}

bool ExtractDisp8(
    const ModuleImage& image,
    const ResolvedFunction& function,
    std::span<const std::uint8_t> pattern,
    std::span<const std::uint8_t> mask,
    std::uint32_t& value,
    std::string& detail) {
    if (!function.available)
        return false;
    if (function.size < pattern.size()) {
        detail = "function is too small for byte field pattern";
        return false;
    }

    std::vector<std::uint8_t> bytes(function.size);
    if (!image.Read(function.address, bytes.data(), bytes.size())) {
        detail = "could not read function bytes";
        return false;
    }

    const auto match = ScanMasked(bytes, function.address, pattern, mask);
    if (match.state != MatchState::Found) {
        detail = match.detail;
        return false;
    }
    const auto offset =
        static_cast<std::size_t>(match.address - function.address);
    if (offset + 3 >= bytes.size()) {
        detail = "byte field displacement lies outside function";
        return false;
    }
    value = bytes[offset + 3];
    // Globals members use a positive offset; 0x80..0xFF encode negative disp8.
    if (value == 0 || value > 0x7F) {
        detail =
            "byte field displacement is not a positive disp8";
        return false;
    }
    detail = "byte field offset " + Hex(value);
    return true;
}

}  // namespace

std::string DescribeMatchResult(
    std::string_view name,
    const MatchResult& match,
    std::uintptr_t image_base) {
    std::ostringstream stream;
    stream << name << ": " << match.detail;
    if (!match.candidates.empty()) {
        stream << " candidates_rva=";
        for (std::size_t index = 0; index < match.candidates.size();
             ++index) {
            if (index != 0)
                stream << ',';
            const auto rva =
                match.candidates[index] >= image_base
                    ? match.candidates[index] - image_base
                    : match.candidates[index];
            stream << Hex(rva);
        }
    }
    return stream.str();
}

std::span<const MaskedSignature> GetMigotoFunctionSignatures() {
    static const std::array<MaskedSignature, 5> signatures = {
        MaskedSignature{
            "AnalyseFrame",
            MigotoTarget::AnalyseFrame,
            signatures_data::kAnalyseFramePattern,
            signatures_data::kAnalyseFrameMask,
        },
        MaskedSignature{
            "ReloadConfig",
            MigotoTarget::ReloadConfig,
            signatures_data::kReloadConfigPattern,
            signatures_data::kReloadConfigMask,
        },
        MaskedSignature{
            "ReloadFixes",
            MigotoTarget::ReloadFixes,
            signatures_data::kReloadFixesPattern,
            signatures_data::kReloadFixesMask,
        },
        MaskedSignature{
            "HackerSwapChain::Present",
            MigotoTarget::Present,
            signatures_data::kPresentPattern,
            signatures_data::kPresentMask,
        },
        MaskedSignature{
            "HackerSwapChain::Present1",
            MigotoTarget::Present1,
            signatures_data::kPresent1Pattern,
            signatures_data::kPresent1Mask,
        },
    };
    return signatures;
}

const GlobalsLoadSignature& GetGlobalsLoadSignature() {
    static const GlobalsLoadSignature signature = {
        signatures_data::kGlobalsLoadPattern,
        signatures_data::kGlobalsLoadMask,
        3,
    };
    return signature;
}

ResolvedMigotoCapabilities ResolveMigotoCapabilities(
    const ModuleImage& image) {
    ResolvedMigotoCapabilities result;
    for (const auto& signature : GetMigotoFunctionSignatures()) {
        auto resolved = ResolveFunction(image, signature, result.diagnostics);
        switch (signature.target) {
        case MigotoTarget::AnalyseFrame:
            result.frame_analysis = std::move(resolved);
            break;
        case MigotoTarget::ReloadConfig:
            result.reload_config = std::move(resolved);
            break;
        case MigotoTarget::ReloadFixes:
            result.reload_shaders = std::move(resolved);
            break;
        case MigotoTarget::Present:
            result.present = std::move(resolved);
            break;
        case MigotoTarget::Present1:
            result.present1 = std::move(resolved);
            break;
        }
    }

    const auto text = image.Section(".text");
    const auto& globals_signature = GetGlobalsLoadSignature();
    std::optional<std::uintptr_t> globals_instruction;
    std::optional<std::uintptr_t> globals_target;
    std::optional<std::uintptr_t> globals_loaded;
    if (!text) {
        AddDiagnostic(
            result.diagnostics,
            DescribeGlobalsEvidence(
                image,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                "PE image has no .text section"),
            true);
    } else {
        const auto match = ScanMasked(
            text->bytes,
            text->address,
            globals_signature.pattern,
            globals_signature.mask);
        if (match.state != MatchState::Found) {
            AddDiagnostic(
                result.diagnostics,
                DescribeGlobalsEvidence(
                    image,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    DescribeMatchResult(
                        "signature", match, image.Base())),
                true);
        } else {
            globals_instruction = match.address;
            std::array<std::uint8_t, 32> instruction = {};
            if (!image.Read(match.address, instruction.data(), instruction.size())) {
                AddDiagnostic(
                    result.diagnostics,
                    DescribeGlobalsEvidence(
                        image,
                        globals_instruction,
                        std::nullopt,
                        std::nullopt,
                        "could not read load instruction"),
                    true);
            } else {
                const auto target = ResolveRipRelative(
                    match.address,
                    instruction,
                    globals_signature.displacement_offset);
                if (target.state != MatchState::Found) {
                    AddDiagnostic(
                        result.diagnostics,
                        DescribeGlobalsEvidence(
                            image,
                            globals_instruction,
                            std::nullopt,
                            std::nullopt,
                            "RIP-relative target resolution failed: " +
                                target.detail),
                        true);
                } else {
                    globals_target = target.address;
                    std::uintptr_t globals_pointer = 0;
                    if (!image.Read(
                            target.address,
                            &globals_pointer,
                            sizeof(globals_pointer))) {
                        AddDiagnostic(
                            result.diagnostics,
                            DescribeGlobalsEvidence(
                                image,
                                globals_instruction,
                                globals_target,
                                std::nullopt,
                                "could not read loaded pointer"),
                            true);
                    } else {
                        globals_loaded = globals_pointer;
                        if (!image.IsWritable(globals_pointer, 0x1000)) {
                            AddDiagnostic(
                                result.diagnostics,
                                DescribeGlobalsEvidence(
                                    image,
                                    globals_instruction,
                                    globals_target,
                                    globals_loaded,
                                    "loaded pointer is not a writable "
                                    "module range"),
                                true);
                        } else {
                            result.globals = globals_pointer;
                            AddDiagnostic(
                                result.diagnostics,
                                DescribeGlobalsEvidence(
                                    image,
                                    globals_instruction,
                                    globals_target,
                                    globals_loaded,
                                    "passed"),
                                false);
                        }
                    }
                }
            }
        }
    }

    std::string detail;
    if (!ExtractDisp32(
            image,
            result.frame_analysis,
            kHuntingPattern,
            kHuntingMask,
            result.hunting_offset,
            detail)) {
        AddDiagnostic(
            result.diagnostics, "Hunting field: " + detail, true);
    } else {
        AddDiagnostic(
            result.diagnostics, "Hunting field: " + detail, false);
    }
    if (!ExtractDisp32(
            image,
            result.frame_analysis,
            kAnalysePattern,
            kAnalyseMask,
            result.analyse_frame_offset,
            detail)) {
        AddDiagnostic(
            result.diagnostics,
            "Frame-analysis field: " + detail,
            true);
    } else {
        AddDiagnostic(
            result.diagnostics,
            "Frame-analysis field: " + detail,
            false);
    }
    if (!ExtractDisp8(
            image,
            result.reload_config,
            kReloadPendingPattern,
            kReloadPendingMask,
            result.reload_config_pending_offset,
            detail)) {
        AddDiagnostic(
            result.diagnostics,
            "Config-reload pending field: " + detail,
            true);
        result.reload_config.available = false;
    } else {
        AddDiagnostic(
            result.diagnostics,
            "Config-reload pending field: " + detail,
            false);
    }

    if (result.globals && result.hunting_offset != 0 &&
        result.analyse_frame_offset != 0) {
        std::uint32_t hunting = 0;
        std::uint8_t analysis_active = 0;
        const auto hunting_readable = image.Read(
            *result.globals + result.hunting_offset,
            &hunting,
            sizeof(hunting));
        const auto analysis_readable = image.Read(
            *result.globals + result.analyse_frame_offset,
            &analysis_active,
            sizeof(analysis_active));
        if (!hunting_readable || hunting > 2 ||
            !analysis_readable || analysis_active > 1) {
            AddDiagnostic(
                result.diagnostics,
                DescribeGlobalsEvidence(
                    image,
                    globals_instruction,
                    globals_target,
                    globals_loaded,
                    "field validation failed"),
                true);
            result.frame_analysis.available = false;
        }
    } else if (result.frame_analysis.available) {
        AddDiagnostic(
            result.diagnostics,
            DescribeGlobalsEvidence(
                image,
                globals_instruction,
                globals_target,
                globals_loaded,
                "Globals offsets are unavailable; frame analysis "
                "disabled"),
            true);
        result.frame_analysis.available = false;
    }

    if (result.reload_config.available) {
        if (!result.globals ||
            result.reload_config_pending_offset == 0) {
            AddDiagnostic(
                result.diagnostics,
                "Config-reload pending field validation failed: "
                "Globals or field offset is unavailable; config "
                "reload is disabled",
                true);
            result.reload_config.available = false;
        } else {
            std::uint8_t pending = 0;
            if (!image.Read(
                    *result.globals +
                        result.reload_config_pending_offset,
                    &pending,
                    sizeof(pending))) {
                AddDiagnostic(
                    result.diagnostics,
                    "Config-reload pending field validation failed: "
                    "could not read Globals+" +
                        Hex(result.reload_config_pending_offset) +
                        "; official layout may have changed and "
                        "config reload is disabled",
                    true);
                result.reload_config.available = false;
            } else if (pending > 1) {
                AddDiagnostic(
                    result.diagnostics,
                    "Config-reload pending field validation failed: "
                    "Globals+" +
                        Hex(result.reload_config_pending_offset) +
                        " has unexpected value=" +
                        std::to_string(pending) +
                        "; config reload is disabled",
                    true);
                result.reload_config.available = false;
            } else {
                AddDiagnostic(
                    result.diagnostics,
                    "Config-reload pending field validation: Globals+" +
                        Hex(result.reload_config_pending_offset) +
                        " value=" + std::to_string(pending) +
                        " passed",
                    false);
            }
        }
    }

    const int capabilities =
        static_cast<int>(result.frame_analysis.available) +
        static_cast<int>(result.reload_config.available) +
        static_cast<int>(result.reload_shaders.available);
    if (capabilities == 3)
        result.mode = ResolvedAdapterMode::Full;
    else if (capabilities > 0)
        result.mode = ResolvedAdapterMode::Partial;
    else
        result.mode = ResolvedAdapterMode::ReadOnly;
    return result;
}

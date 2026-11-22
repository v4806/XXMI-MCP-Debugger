#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>

#include "MigotoSignatures.h"
#include "ModuleImage.h"
#include "TestHarness.h"

TEST_CASE(MigotoSignaturesHaveEqualPatternAndMaskLengths) {
    const auto signatures = GetMigotoFunctionSignatures();
    REQUIRE_EQ(signatures.size(), 5U);
    for (const auto& signature : signatures) {
        REQUIRE(!signature.name.empty());
        REQUIRE(!signature.pattern.empty());
        REQUIRE_EQ(signature.pattern.size(), signature.mask.size());
    }

    const auto globals = GetGlobalsLoadSignature();
    REQUIRE(!globals.pattern.empty());
    REQUIRE_EQ(globals.pattern.size(), globals.mask.size());
}

TEST_CASE(MigotoResolverDegradesOnNonMigotoImage) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    REQUIRE(image.has_value());

    const auto result = ResolveMigotoCapabilities(*image);
    REQUIRE(result.mode == ResolvedAdapterMode::ReadOnly);
    REQUIRE(!result.globals.has_value());
    REQUIRE(!result.frame_analysis.available);
    REQUIRE(!result.reload_config.available);
    REQUIRE(!result.reload_shaders.available);
    REQUIRE(!result.diagnostics.empty());
    for (const auto& diagnostic : result.diagnostics)
        REQUIRE(diagnostic.error);

    const auto globals = std::find_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.message.find("Globals:") !=
                   std::string::npos;
        });
    REQUIRE(globals != result.diagnostics.end());
    REQUIRE(globals->message.find("slot=RIP+3") != std::string::npos);
    REQUIRE(globals->message.find("target=") != std::string::npos);
    REQUIRE(globals->message.find("loaded=") != std::string::npos);
    REQUIRE(globals->message.find("section=") != std::string::npos);
    REQUIRE(globals->message.find("validation=") != std::string::npos);
}

TEST_CASE(MigotoResolverAmbiguityDiagnosticIncludesCandidateRvas) {
    MatchResult match;
    match.state = MatchState::Ambiguous;
    match.candidates = {0x1100, 0x2200};
    match.detail = "signature matched more than one location";

    const auto diagnostic = DescribeMatchResult(
        "ReloadConfig", match, 0x1000);
    REQUIRE(diagnostic.find("candidates_rva=") != std::string::npos);
    REQUIRE(diagnostic.find("0x100") != std::string::npos);
    REQUIRE(diagnostic.find("0x1200") != std::string::npos);
}

TEST_CASE(MigotoResolverFindsOfficialV133Anchors) {
    constexpr wchar_t kOfficialDll[] =
        L"D:\\Temp\\XXMI-PACKAGE-v1.3.3\\d3d11.dll";
    if (!std::filesystem::exists(kOfficialDll))
        return;

    HMODULE module = LoadLibraryExW(
        kOfficialDll,
        nullptr,
        DONT_RESOLVE_DLL_REFERENCES);
    REQUIRE(module != nullptr);

    const auto image = ModuleImage::Load(module);
    if (!image.has_value())
        std::cerr << "ModuleImage::Load failed: "
                  << ModuleImage::LastLoadError() << '\n';
    REQUIRE(image.has_value());
    const auto result = ResolveMigotoCapabilities(*image);
    REQUIRE(result.mode == ResolvedAdapterMode::Full);
    REQUIRE(result.frame_analysis.available);
    REQUIRE(result.reload_config.available);
    REQUIRE(result.reload_shaders.available);
    REQUIRE(result.present.available);
    REQUIRE(result.present1.available);
    REQUIRE(result.globals.has_value());
    REQUIRE_EQ(result.hunting_offset, 0x770U);
    REQUIRE_EQ(result.analyse_frame_offset, 0x78CU);
    REQUIRE_EQ(result.reload_config_pending_offset, 0x29U);

    FreeLibrary(module);
}

#include <Windows.h>

#include <algorithm>
#include <iostream>
#include <string>

#include "ModuleImage.h"
#include "TestHarness.h"

TEST_CASE(ModuleImageLoadsCurrentExecutable) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    if (!image.has_value())
        std::cerr << "ModuleImage::Load failed: " << ModuleImage::LastLoadError()
                  << '\n';
    REQUIRE(image.has_value());
    REQUIRE(image->Base() != 0);

    const auto text = image->Section(".text");
    REQUIRE(text.has_value());
    REQUIRE(text->bytes.size() > 16);

    const auto functions = image->FunctionRanges();
    REQUIRE(!functions.empty());
    REQUIRE(std::is_sorted(functions.begin(), functions.end(),
                          [](const auto& lhs, const auto& rhs) {
                              return lhs.begin < rhs.begin;
                          }));
}

TEST_CASE(ModuleImageRejectsNullModule) {
    REQUIRE(!ModuleImage::Load(nullptr).has_value());
}

TEST_CASE(ModuleImageReadsInvalidRangeSafely) {
    const auto image = ModuleImage::Load(GetModuleHandleW(nullptr));
    if (!image.has_value())
        std::cerr << "ModuleImage::Load failed: " << ModuleImage::LastLoadError()
                  << '\n';
    REQUIRE(image.has_value());
    unsigned char byte = 0;
    REQUIRE(!image->Read(image->Base() + 0x7FFFFFF0U, &byte, 1));
}

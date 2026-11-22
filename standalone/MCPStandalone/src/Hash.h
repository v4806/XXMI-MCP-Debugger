#pragma once

#include <filesystem>
#include <string>

bool ComputeFileSha256(
    const std::filesystem::path& path,
    std::string& digest_hex,
    std::string& error);

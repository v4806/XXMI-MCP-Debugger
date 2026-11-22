#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

class DiagnosticLog {
public:
    static void Initialize(const std::filesystem::path& path,
                           std::uintmax_t max_bytes = 2U * 1024U * 1024U);
    static void Shutdown();

    static void Info(std::string_view message);
    static void Warn(std::string_view message);
    static void Error(std::string_view message);

    static std::filesystem::path Path();
    static std::uint64_t ErrorCount();

private:
    static void Write(std::string_view level, std::string_view message);
};

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct RuntimePersistentValue {
    std::string name;
    std::optional<double> value;
    std::string raw_value;
};

struct RuntimeLogSummary {
    std::uint64_t unknown_setting_count = 0;
    std::string first_unknown_setting;
    std::vector<std::string> warnings;
};

struct RuntimeConfigSnapshot {
    std::uint32_t format_version = 1;
    std::string kind = "runtime_variables";
    std::uint64_t frame = 0;
    std::uint64_t captured_unix_ms = 0;
    std::uint32_t pid = 0;
    std::string d3d11_sha256;
    std::string mcp_sha256;
    std::string process_path;
    std::string dll_path;
    std::string config_path;
    std::string user_config_path;
    std::string log_path;
    std::optional<std::uint32_t> hunting;
    std::optional<bool> analysis_active;
    std::optional<std::int32_t> ini_params_register;
    bool ini_params_register_valid = false;
    std::optional<bool> clear_unknown_settings;
    std::vector<RuntimePersistentValue> persistent_values;
    bool persistent_values_truncated = false;
    RuntimeLogSummary log_summary;
    std::vector<std::string> missing_fields;
    std::vector<std::string> limits_hit;
    std::vector<std::string> warnings;
};

bool ReadRuntimeConfig(
    const std::filesystem::path& config_root,
    RuntimeConfigSnapshot& snapshot,
    std::string& error);

std::uint64_t CountUnknownSettings(
    const std::filesystem::path& log_path);

bool ReadRuntimeLog(
    const std::filesystem::path& log_path,
    RuntimeLogSummary& summary,
    std::string& error);

std::string SerializeRuntimeVariables(
    const RuntimeConfigSnapshot& snapshot);

bool WriteRuntimeVariables(
    const std::filesystem::path& path,
    const RuntimeConfigSnapshot& snapshot,
    std::string& error);

#include "RuntimeVariables.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace {

constexpr std::size_t kMaxPersistentValues = 1000;
constexpr std::size_t kMaxLogWarnings = 64;
constexpr std::size_t kMaxStringBytes = 512;
constexpr std::size_t kMaxLogBytes = 4U * 1024U * 1024U;

std::string LimitString(const std::string& value) {
    if (value.size() <= kMaxStringBytes)
        return value;
    auto size = kMaxStringBytes;
    while (size > 0 &&
           (static_cast<unsigned char>(value[size]) & 0xC0) == 0x80) {
        --size;
    }
    return value.substr(0, size);
}

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string LowerAscii(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return value;
}

bool ReadFile(
    const std::filesystem::path& path,
    std::string& contents,
    std::string& error) {
    contents.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "could not open file";
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    contents = buffer.str();
    if (contents.size() > kMaxLogBytes)
        contents.resize(kMaxLogBytes);
    return true;
}

std::filesystem::path Utf8Path(const std::string& value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::string Utf8String(const std::filesystem::path& value) {
    const auto utf8 = value.u8string();
    return std::string(
        reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::optional<double> ParseNumber(const std::string& value) {
    if (value.empty())
        return std::nullopt;
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (errno != 0 || !end ||
        !Trim(std::string(end)).empty() || end == value.c_str())
        return std::nullopt;
    return parsed;
}

std::optional<bool> ParseBool(const std::string& value) {
    const auto lower = LowerAscii(Trim(value));
    if (lower == "1" || lower == "true" || lower == "yes" ||
        lower == "on")
        return true;
    if (lower == "0" || lower == "false" || lower == "no" ||
        lower == "off")
        return false;
    if (const auto number = ParseNumber(lower)) {
        if (*number == 0.0)
            return false;
        if (*number == 1.0)
            return true;
    }
    return std::nullopt;
}

struct IniEntry {
    std::string section;
    std::string key;
    std::string value;
};

std::vector<IniEntry> ParseIni(const std::string& contents) {
    std::vector<IniEntry> entries;
    std::istringstream stream(contents);
    std::string section;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[') {
            const auto end = line.find(']');
            if (end != std::string::npos)
                section = line.substr(1, end - 1);
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        entries.push_back({
            section,
            LimitString(Trim(line.substr(0, equals))),
            LimitString(Trim(line.substr(equals + 1))),
        });
    }
    return entries;
}

std::string FirstEntryValue(
    const std::vector<IniEntry>& entries,
    const char* wanted_section,
    const char* wanted_key) {
    for (const auto& entry : entries) {
        if (LowerAscii(entry.section) == LowerAscii(wanted_section) &&
            LowerAscii(entry.key) == LowerAscii(wanted_key)) {
            return entry.value;
        }
    }
    return {};
}

std::uint64_t ParseUnknownSettingCount(const std::string& line) {
    const auto marker =
        line.find("Detected ");
    if (marker == std::string::npos)
        return 0;
    const auto number_start = marker + 9;
    char* end = nullptr;
    errno = 0;
    const auto number = std::strtoull(
        line.c_str() + number_start, &end, 10);
    if (errno != 0 || !end || end == line.c_str() + number_start)
        return 0;
    return number;
}

std::string ExtractUnknownEntry(const std::string& line) {
    constexpr std::string_view first_prefix =
        "The first unrecognised entry was:";
    const auto first = line.find(first_prefix);
    if (first != std::string::npos) {
        auto value = Trim(line.substr(first + first_prefix.size()));
        if (!value.empty() && value.front() == '"')
            value.erase(value.begin());
        if (!value.empty() && value.back() == '"')
            value.pop_back();
        return LimitString(Trim(value));
    }
    constexpr std::string_view persistent_prefix =
        "Unrecognised persistent variable:";
    const auto persistent = line.find(persistent_prefix);
    if (persistent != std::string::npos)
        return LimitString(Trim(
            line.substr(persistent + persistent_prefix.size())));
    return {};
}

bool IsWarningLine(const std::string& line) {
    return line.find("Unrecognised") != std::string::npos ||
           line.find("Unknown settings") != std::string::npos ||
           line.find("Unknown setting") != std::string::npos ||
           line.find("Variable not recognized") != std::string::npos ||
           line.find("Syntax Error") != std::string::npos ||
           line.find("Detected ") != std::string::npos;
}

bool IsUsefulLogLine(const std::string& line) {
    return line.find("IniParams") != std::string::npos ||
           line.find("Frame analysis saved") != std::string::npos ||
           line.find("Reloaded config") != std::string::npos ||
           line.find("successfully reloaded shaders") != std::string::npos;
}

std::string JsonString(const std::string& value) {
    std::ostringstream stream;
    stream << '"';
    for (const unsigned char character : LimitString(value)) {
        switch (character) {
        case '"':
            stream << "\\\"";
            break;
        case '\\':
            stream << "\\\\";
            break;
        case '\b':
            stream << "\\b";
            break;
        case '\f':
            stream << "\\f";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            if (character < 0x20) {
                stream << "\\u"
                       << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(character)
                       << std::dec << std::setfill(' ');
            } else {
                stream << static_cast<char>(character);
            }
        }
    }
    stream << '"';
    return stream.str();
}

template <typename T>
void AppendKeyValue(
    std::ostringstream& stream,
    bool& first,
    const char* name,
    const T& value) {
    if (!first)
        stream << ',';
    first = false;
    stream << JsonString(name) << ':' << value;
}

void AppendString(
    std::ostringstream& stream,
    bool& first,
    const char* name,
    const std::string& value) {
    AppendKeyValue(stream, first, name, JsonString(value));
}

void AppendStringArray(
    std::ostringstream& stream,
    const char* name,
    const std::vector<std::string>& values) {
    stream << ',' << JsonString(name) << ":[";
    const auto count = std::min<std::size_t>(
        values.size(), kMaxLogWarnings);
    for (std::size_t index = 0; index < count; ++index) {
        if (index)
            stream << ',';
        stream << JsonString(values[index]);
    }
    stream << ']';
}

}  // namespace

bool ReadRuntimeLog(
    const std::filesystem::path& log_path,
    RuntimeLogSummary& summary,
    std::string& error) {
    summary = {};
    error.clear();
    if (!std::filesystem::exists(log_path)) {
        error = "log file does not exist";
        return false;
    }
    std::string contents;
    if (!ReadFile(log_path, contents, error))
        return false;

    std::istringstream stream(contents);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto count = ParseUnknownSettingCount(line);
        if (count != 0)
            summary.unknown_setting_count = count;
        if (summary.first_unknown_setting.empty())
            summary.first_unknown_setting = ExtractUnknownEntry(line);
        if (IsWarningLine(line) && summary.warnings.size() < kMaxLogWarnings)
            summary.warnings.push_back(LimitString(Trim(line)));
        else if (IsUsefulLogLine(line) &&
                 summary.warnings.size() < kMaxLogWarnings)
            summary.warnings.push_back(LimitString(Trim(line)));
    }
    if (summary.unknown_setting_count == 0) {
        for (const auto& warning : summary.warnings) {
            if (warning.find("Unknown settings clean-up") !=
                std::string::npos) {
                summary.unknown_setting_count = 1;
                break;
            }
        }
    }
    return true;
}

std::uint64_t CountUnknownSettings(
    const std::filesystem::path& log_path) {
    RuntimeLogSummary summary;
    std::string error;
    if (!ReadRuntimeLog(log_path, summary, error))
        return 0;
    return summary.unknown_setting_count;
}

bool ReadRuntimeConfig(
    const std::filesystem::path& config_root,
    RuntimeConfigSnapshot& snapshot,
    std::string& error) {
    error.clear();
    snapshot = {};
    std::error_code filesystem_error;
    if (!std::filesystem::exists(config_root, filesystem_error)) {
        error = filesystem_error
                    ? filesystem_error.message()
                    : "config root does not exist";
        return false;
    }

    const auto d3dx_path = config_root / L"d3dx.ini";
    const auto default_user_path = config_root / L"d3dx_user.ini";
    const auto log_path = config_root / L"d3d11_log.txt";
    snapshot.config_path = Utf8String(d3dx_path);
    snapshot.log_path = Utf8String(log_path);

    std::string contents;
    std::vector<IniEntry> config_entries;
    if (ReadFile(d3dx_path, contents, error)) {
        config_entries = ParseIni(contents);
    } else {
        snapshot.warnings.push_back(error);
        error.clear();
    }

    const auto ini_register =
        FirstEntryValue(config_entries, "Rendering", "ini_params");
    if (!ini_register.empty()) {
        if (const auto number = ParseNumber(ini_register)) {
            snapshot.ini_params_register =
                static_cast<std::int32_t>(*number);
            snapshot.ini_params_register_valid =
                *number >= 0.0 && *number < 128.0;
        } else {
            snapshot.warnings.emplace_back(
                "Rendering.ini_params is not a number");
        }
    }
    const auto clear_unknown =
        FirstEntryValue(config_entries, "System", "clear_unknown_settings");
    if (!clear_unknown.empty())
        snapshot.clear_unknown_settings = ParseBool(clear_unknown);
    if (clear_unknown.empty() || !snapshot.clear_unknown_settings) {
        // A missing setting keeps the official default, which is enabled.
        if (clear_unknown.empty())
            snapshot.clear_unknown_settings = true;
    }

    auto user_path = default_user_path;
    const auto configured_user =
        FirstEntryValue(config_entries, "Include", "user_config");
    if (!configured_user.empty()) {
        const auto candidate =
            std::filesystem::path(configured_user);
        user_path = candidate.is_absolute()
                        ? candidate
                        : config_root / candidate;
    }
    snapshot.user_config_path = Utf8String(user_path);

    std::string user_contents;
    if (ReadFile(user_path, user_contents, error)) {
        const auto entries = ParseIni(user_contents);
        std::vector<std::size_t> positions;
        for (const auto& entry : entries) {
            const auto position = std::find_if(
                positions.begin(),
                positions.end(),
                [&] (std::size_t index) {
                    return snapshot.persistent_values[index].name ==
                           entry.key;
                });
            const auto value = ParseNumber(entry.value);
            if (position != positions.end()) {
                snapshot.persistent_values[*position].value = value;
                snapshot.persistent_values[*position].raw_value =
                    entry.value;
                continue;
            }
            if (snapshot.persistent_values.size() >= kMaxPersistentValues) {
                snapshot.persistent_values_truncated = true;
                continue;
            }
            positions.push_back(snapshot.persistent_values.size());
            snapshot.persistent_values.push_back({
                LimitString(entry.key),
                value,
                LimitString(entry.value),
            });
        }
    } else {
        snapshot.warnings.push_back(error);
        error.clear();
    }

    std::string log_error;
    if (ReadRuntimeLog(log_path, snapshot.log_summary, log_error)) {
        snapshot.warnings.insert(
            snapshot.warnings.end(),
            snapshot.log_summary.warnings.begin(),
            snapshot.log_summary.warnings.end());
    } else if (std::filesystem::exists(log_path)) {
        snapshot.warnings.push_back(log_error);
    }
    if (snapshot.persistent_values_truncated)
        snapshot.limits_hit.emplace_back("persistent_values");
    return true;
}

std::string SerializeRuntimeVariables(
    const RuntimeConfigSnapshot& snapshot) {
    std::ostringstream stream;
    bool first = true;
    stream << '{';
    AppendKeyValue(stream, first, "format_version", snapshot.format_version);
    AppendString(stream, first, "kind", snapshot.kind);
    AppendKeyValue(stream, first, "frame", snapshot.frame);
    AppendKeyValue(
        stream, first, "captured_unix_ms", snapshot.captured_unix_ms);
    AppendKeyValue(stream, first, "pid", snapshot.pid);
    AppendString(stream, first, "d3d11_sha256", snapshot.d3d11_sha256);
    AppendString(stream, first, "mcp_sha256", snapshot.mcp_sha256);
    AppendString(stream, first, "process_path", snapshot.process_path);
    AppendString(stream, first, "dll_path", snapshot.dll_path);
    AppendString(stream, first, "config_path", snapshot.config_path);
    AppendString(stream, first, "user_config_path",
                 snapshot.user_config_path);
    AppendString(stream, first, "log_path", snapshot.log_path);
    if (snapshot.hunting) {
        AppendKeyValue(stream, first, "hunting", *snapshot.hunting);
    }
    if (snapshot.analysis_active) {
        AppendKeyValue(
            stream,
            first,
            "analysis_active",
            *snapshot.analysis_active ? "true" : "false");
    }
    if (snapshot.ini_params_register) {
        AppendKeyValue(
            stream,
            first,
            "ini_params_register",
            *snapshot.ini_params_register);
    }
    AppendKeyValue(
        stream,
        first,
        "ini_params_register_valid",
        snapshot.ini_params_register_valid ? "true" : "false");
    if (snapshot.clear_unknown_settings) {
        AppendKeyValue(
            stream,
            first,
            "clear_unknown_settings",
            *snapshot.clear_unknown_settings ? "true" : "false");
    }
    AppendKeyValue(
        stream,
        first,
        "persistent_value_count",
        snapshot.persistent_values.size());
    stream << ",\"persistent_values\":[";
    for (std::size_t index = 0;
         index < snapshot.persistent_values.size();
         ++index) {
        if (index)
            stream << ',';
        const auto& value = snapshot.persistent_values[index];
        stream << '{';
        bool first_value = true;
        AppendString(stream, first_value, "name", value.name);
        if (value.value) {
            std::ostringstream number;
            number << std::setprecision(17) << *value.value;
            AppendKeyValue(stream, first_value, "value", number.str());
        }
        AppendString(stream, first_value, "raw_value", value.raw_value);
        stream << '}';
    }
    stream << ']';
    stream << ",\"unknown_setting_count\":"
           << snapshot.log_summary.unknown_setting_count;
    AppendString(
        stream,
        first,
        "first_unknown_setting",
        snapshot.log_summary.first_unknown_setting);
    std::vector<std::string> warnings = snapshot.warnings;
    for (const auto& warning : snapshot.log_summary.warnings) {
        if (std::find(warnings.begin(), warnings.end(), warning) ==
            warnings.end()) {
            warnings.push_back(warning);
        }
    }
    AppendStringArray(stream, "warnings", warnings);
    AppendStringArray(
        stream, "missing_fields", snapshot.missing_fields);
    AppendStringArray(stream, "limits_hit", snapshot.limits_hit);
    stream << '}';
    return stream.str();
}

bool WriteRuntimeVariables(
    const std::filesystem::path& path,
    const RuntimeConfigSnapshot& snapshot,
    std::string& error) {
    error.clear();
    std::error_code filesystem_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(
            path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = filesystem_error.message();
            return false;
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "could not open runtime-variables snapshot";
        return false;
    }
    const auto json = SerializeRuntimeVariables(snapshot);
    stream.write(
        json.data(), static_cast<std::streamsize>(json.size()));
    if (!stream) {
        error = "could not write runtime-variables snapshot";
        return false;
    }
    return true;
}

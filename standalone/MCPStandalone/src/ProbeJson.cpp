#include "ProbeJson.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {

constexpr std::size_t kMaxSnapshotBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaxResources = 128;
constexpr std::size_t kMaxVertexBuffers = 16;
constexpr std::size_t kMaxInputElements = 32;
constexpr std::size_t kMaxStateValues = 256;
constexpr std::size_t kMaxShaders = 12;
constexpr std::size_t kMaxListValues = 128;
constexpr std::size_t kMaxStringBytes = 4096;

std::string LimitedString(const std::string& value) {
    if (value.size() <= kMaxStringBytes)
        return value;
    return value.substr(0, kMaxStringBytes);
}

std::string JsonString(const std::string& value) {
    std::ostringstream stream;
    stream << '"';
    for (const unsigned char character : LimitedString(value)) {
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

std::string PointerString(std::uintptr_t pointer) {
    if (!pointer)
        return {};
    std::ostringstream stream;
    stream << "0x" << std::hex << pointer;
    return stream.str();
}

template <typename T>
void KeyValue(
    std::ostringstream& stream,
    bool& first,
    const char* key,
    const T& value) {
    if (!first)
        stream << ',';
    first = false;
    stream << JsonString(key) << ':' << value;
}

void KeyString(
    std::ostringstream& stream,
    bool& first,
    const char* key,
    const std::string& value) {
    KeyValue(stream, first, key, JsonString(value));
}

template <typename T>
void KeyOptional(
    std::ostringstream& stream,
    bool& first,
    const char* key,
    const std::optional<T>& value) {
    if (!value)
        return;
    KeyValue(stream, first, key, *value);
}

void KeyPointer(
    std::ostringstream& stream,
    bool& first,
    const char* key,
    std::uintptr_t pointer) {
    KeyValue(stream, first, key, JsonString(PointerString(pointer)));
}

void KeyOptionalHash(
    std::ostringstream& stream,
    bool& first,
    const char* key,
    const std::optional<std::uint32_t>& hash) {
    if (!hash)
        return;
    std::ostringstream formatted;
    formatted << std::hex << std::setw(8) << std::setfill('0') << *hash;
    KeyValue(stream, first, key, JsonString(formatted.str()));
}

void AppendStringArray(
    std::ostringstream& stream,
    const char* name,
    const std::vector<std::string>& values) {
    stream << ',' << JsonString(name) << ":[";
    const auto count = std::min(values.size(), kMaxListValues);
    for (std::size_t index = 0; index < count; ++index) {
        if (index)
            stream << ',';
        stream << JsonString(values[index]);
    }
    stream << ']';
}

void AppendShader(std::ostringstream& stream, const ProbeShaderInfo& shader) {
    bool first = true;
    stream << '{';
    KeyString(stream, first, "stage", shader.stage);
    KeyPointer(stream, first, "pointer", shader.pointer);
    KeyOptional(stream, first, "hash", shader.hash);
    KeyString(stream, first, "hash_status", shader.hash_status);
    stream << '}';
}

void AppendInputElement(
    std::ostringstream& stream,
    const ProbeInputElement& element) {
    bool first = true;
    stream << '{';
    KeyString(stream, first, "semantic_name", element.semantic_name);
    KeyValue(stream, first, "semantic_index", element.semantic_index);
    KeyString(stream, first, "format", element.format);
    KeyValue(stream, first, "input_slot", element.input_slot);
    KeyValue(
        stream,
        first,
        "aligned_byte_offset",
        element.aligned_byte_offset);
    KeyString(stream, first, "classification", element.classification);
    KeyValue(
        stream,
        first,
        "instance_data_step_rate",
        element.instance_data_step_rate);
    stream << '}';
}

void AppendBuffer(
    std::ostringstream& stream,
    const ProbeBufferBinding& buffer) {
    bool first = true;
    stream << '{';
    KeyString(stream, first, "role", buffer.role);
    KeyValue(stream, first, "slot", buffer.slot);
    KeyPointer(stream, first, "pointer", buffer.pointer);
    KeyOptionalHash(stream, first, "hash", buffer.hash);
    KeyString(stream, first, "hash_status", buffer.hash_status);
    KeyString(stream, first, "format", buffer.format);
    KeyValue(stream, first, "offset", buffer.offset);
    KeyValue(stream, first, "stride", buffer.stride);
    KeyValue(stream, first, "byte_width", buffer.byte_width);
    KeyValue(stream, first, "bind_flags", buffer.bind_flags);
    KeyValue(stream, first, "usage", buffer.usage);
    stream << '}';
}

void AppendResource(
    std::ostringstream& stream,
    const ProbeResourceInfo& resource) {
    bool first = true;
    stream << '{';
    KeyString(stream, first, "stage", resource.stage);
    KeyValue(stream, first, "slot", resource.slot);
    KeyPointer(stream, first, "view_pointer", resource.view_pointer);
    KeyPointer(
        stream,
        first,
        "resource_pointer",
        resource.resource_pointer);
    KeyOptionalHash(stream, first, "hash", resource.hash);
    KeyOptionalHash(
        stream,
        first,
        "original_hash",
        resource.original_hash);
    KeyString(stream, first, "hash_status", resource.hash_status);
    KeyString(stream, first, "view_dimension", resource.view_dimension);
    KeyString(
        stream,
        first,
        "resource_dimension",
        resource.resource_dimension);
    KeyString(stream, first, "format", resource.format);
    KeyValue(stream, first, "width", resource.width);
    KeyValue(stream, first, "height", resource.height);
    KeyValue(stream, first, "depth", resource.depth);
    KeyValue(stream, first, "mip_levels", resource.mip_levels);
    KeyValue(stream, first, "array_size", resource.array_size);
    KeyValue(stream, first, "sample_count", resource.sample_count);
    KeyValue(stream, first, "bind_flags", resource.bind_flags);
    KeyValue(stream, first, "usage", resource.usage);
    KeyValue(stream, first, "byte_width", resource.byte_width);
    KeyValue(
        stream,
        first,
        "structure_stride",
        resource.structure_stride);
    stream << '}';
}

void AppendStateValue(
    std::ostringstream& stream,
    const ProbeStateValue& value) {
    bool first = true;
    stream << '{';
    KeyString(stream, first, "name", value.name);
    KeyString(stream, first, "value", value.value);
    stream << '}';
}

void AppendLimitsHit(std::ostringstream& stream) {
    stream << "\"limits_hit\":[";
}

}  // namespace

std::string SerializeProbeSnapshot(const LiveProbeSnapshot& snapshot) {
    std::vector<std::string> limits = snapshot.limits_hit;
    if (snapshot.resources.size() > kMaxResources)
        limits.emplace_back("resources");
    if (snapshot.vertex_buffers.size() > kMaxVertexBuffers)
        limits.emplace_back("vertex_buffers");
    if (snapshot.input_elements.size() > kMaxInputElements)
        limits.emplace_back("input_elements");
    if (snapshot.states.size() > kMaxStateValues)
        limits.emplace_back("states");
    if (snapshot.shaders.size() > kMaxShaders)
        limits.emplace_back("shaders");
    if (snapshot.missing_fields.size() > kMaxListValues)
        limits.emplace_back("missing_fields");
    if (limits.size() > kMaxListValues)
        limits.resize(kMaxListValues);

    std::ostringstream stream;
    bool first = true;
    stream << '{';
    KeyValue(stream, first, "format_version", snapshot.format_version);
    KeyString(stream, first, "kind", snapshot.kind);
    KeyValue(stream, first, "frame", snapshot.frame);
    KeyValue(
        stream,
        first,
        "captured_unix_ms",
        snapshot.captured_unix_ms);
    KeyValue(stream, first, "pid", snapshot.pid);
    KeyString(stream, first, "d3d11_sha256", snapshot.d3d11_sha256);
    KeyString(stream, first, "mcp_sha256", snapshot.mcp_sha256);
    KeyString(stream, first, "process_path", snapshot.process_path);
    KeyString(stream, first, "dll_path", snapshot.dll_path);
    KeyString(stream, first, "config_path", snapshot.config_path);
    KeyString(stream, first, "log_path", snapshot.log_path);
    KeyString(stream, first, "analysis_root", snapshot.analysis_root);
    KeyOptional(stream, first, "hunting", snapshot.hunting);
    if (snapshot.analysis_active)
        KeyValue(
            stream,
            first,
            "analysis_active",
            *snapshot.analysis_active ? "true" : "false");
    KeyPointer(stream, first, "context_pointer", snapshot.context_pointer);
    KeyPointer(
        stream,
        first,
        "before_draw_pointer",
        snapshot.before_draw_pointer);

    if (!first)
        stream << ',';
    first = false;
    stream << "\"draw\":{";
    bool first_draw = true;
    KeyString(stream, first_draw, "type", snapshot.draw.type);
    KeyValue(
        stream,
        first_draw,
        "vertex_count",
        snapshot.draw.vertex_count);
    KeyValue(
        stream,
        first_draw,
        "index_count",
        snapshot.draw.index_count);
    KeyValue(
        stream,
        first_draw,
        "instance_count",
        snapshot.draw.instance_count);
    KeyValue(
        stream,
        first_draw,
        "first_vertex",
        snapshot.draw.first_vertex);
    KeyValue(
        stream,
        first_draw,
        "first_index",
        snapshot.draw.first_index);
    KeyValue(
        stream,
        first_draw,
        "first_instance",
        snapshot.draw.first_instance);
    KeyValue(
        stream,
        first_draw,
        "thread_group_x",
        snapshot.draw.thread_group_x);
    KeyValue(
        stream,
        first_draw,
        "thread_group_y",
        snapshot.draw.thread_group_y);
    KeyValue(
        stream,
        first_draw,
        "thread_group_z",
        snapshot.draw.thread_group_z);
    KeyValue(stream, first_draw, "skip", snapshot.draw.skip);
    KeyValue(
        stream,
        first_draw,
        "hunting_skip",
        snapshot.draw.hunting_skip);
    KeyValue(
        stream,
        first_draw,
        "post_command_slots",
        [&snapshot] {
            std::ostringstream values;
            values << '[';
            for (std::size_t index = 0;
                 index < std::min<std::size_t>(
                     snapshot.draw.post_command_slots.size(), 16);
                 ++index) {
                if (index)
                    values << ',';
                values << snapshot.draw.post_command_slots[index];
            }
            values << ']';
            return values.str();
        }());
    stream << '}';

    auto append_array = [&stream](
                            const char* name,
                            auto begin,
                            auto end,
                            auto append) {
        stream << ',' << JsonString(name) << ":[";
        bool first_value = true;
        for (auto iterator = begin; iterator != end; ++iterator) {
            if (!first_value)
                stream << ',';
            first_value = false;
            append(stream, *iterator);
        }
        stream << ']';
    };

    append_array(
        "shaders",
        snapshot.shaders.begin(),
        snapshot.shaders.begin() +
            std::min(snapshot.shaders.size(), kMaxShaders),
        AppendShader);
    append_array(
        "input_elements",
        snapshot.input_elements.begin(),
        snapshot.input_elements.begin() +
            std::min(snapshot.input_elements.size(), kMaxInputElements),
        AppendInputElement);
    append_array(
        "vertex_buffers",
        snapshot.vertex_buffers.begin(),
        snapshot.vertex_buffers.begin() +
            std::min(snapshot.vertex_buffers.size(), kMaxVertexBuffers),
        AppendBuffer);
    if (snapshot.index_buffer) {
        stream << ",\"index_buffer\":";
        AppendBuffer(stream, *snapshot.index_buffer);
    } else {
        stream << ",\"index_buffer\":null";
    }
    append_array(
        "resources",
        snapshot.resources.begin(),
        snapshot.resources.begin() +
            std::min(snapshot.resources.size(), kMaxResources),
        AppendResource);
    append_array(
        "states",
        snapshot.states.begin(),
        snapshot.states.begin() +
            std::min(snapshot.states.size(), kMaxStateValues),
        AppendStateValue);

    AppendStringArray(stream, "missing_fields", snapshot.missing_fields);
    AppendStringArray(stream, "limits_hit", limits);
    AppendStringArray(stream, "warnings", snapshot.warnings);
    stream << '}';

    auto result = stream.str();
    if (result.size() <= kMaxSnapshotBytes)
        return result;

    std::ostringstream oversized;
    bool first_oversized = true;
    oversized << '{';
    KeyValue(oversized, first_oversized, "format_version", 1);
    KeyString(oversized, first_oversized, "kind", snapshot.kind);
    KeyString(
        oversized,
        first_oversized,
        "error",
        "snapshot_size_limit");
    KeyValue(
        oversized,
        first_oversized,
        "original_size",
        result.size());
    oversized << '}';
    return oversized.str();
}

bool WriteProbeSnapshot(
    const std::filesystem::path& path,
    const LiveProbeSnapshot& snapshot,
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
        error = "could not open snapshot file";
        return false;
    }
    const auto json = SerializeProbeSnapshot(snapshot);
    stream.write(
        json.data(),
        static_cast<std::streamsize>(json.size()));
    if (!stream) {
        error = "could not write snapshot file";
        return false;
    }
    return true;
}

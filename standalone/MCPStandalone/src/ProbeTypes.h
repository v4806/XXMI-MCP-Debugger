#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ProbeShaderInfo {
    std::string stage;
    std::uintptr_t pointer = 0;
    std::optional<std::uint64_t> hash;
    std::string hash_status = "unavailable";
};

struct ProbeInputElement {
    std::string semantic_name;
    std::uint32_t semantic_index = 0;
    std::string format;
    std::uint32_t input_slot = 0;
    std::uint32_t aligned_byte_offset = 0;
    std::string classification;
    std::uint32_t instance_data_step_rate = 0;
};

struct ProbeBufferBinding {
    std::string role;
    std::uint32_t slot = 0;
    std::uintptr_t pointer = 0;
    std::optional<std::uint32_t> hash;
    std::string hash_status = "unavailable";
    std::string format;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
    std::uint32_t byte_width = 0;
    std::uint32_t bind_flags = 0;
    std::uint32_t usage = 0;
};

struct ProbeResourceInfo {
    std::string stage;
    std::uint32_t slot = 0;
    std::uintptr_t view_pointer = 0;
    std::uintptr_t resource_pointer = 0;
    std::optional<std::uint32_t> hash;
    std::optional<std::uint32_t> original_hash;
    std::string hash_status = "unavailable";
    std::string view_dimension;
    std::string resource_dimension;
    std::string format;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t mip_levels = 0;
    std::uint32_t array_size = 0;
    std::uint32_t sample_count = 0;
    std::uint32_t bind_flags = 0;
    std::uint32_t usage = 0;
    std::uint32_t byte_width = 0;
    std::uint32_t structure_stride = 0;
};

struct ProbeStateValue {
    std::string name;
    std::string value;
};

struct ProbeDrawInfo {
    std::string type;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t first_vertex = 0;
    std::uint32_t first_index = 0;
    std::uint32_t first_instance = 0;
    std::uint32_t thread_group_x = 0;
    std::uint32_t thread_group_y = 0;
    std::uint32_t thread_group_z = 0;
    bool skip = false;
    bool hunting_skip = false;
    std::vector<std::uint32_t> post_command_slots;
};

struct LiveProbeSnapshot {
    std::uint32_t format_version = 1;
    std::string kind;
    std::uint64_t frame = 0;
    std::uint64_t captured_unix_ms = 0;
    std::uint32_t pid = 0;
    std::string d3d11_sha256;
    std::string mcp_sha256;
    std::string process_path;
    std::string dll_path;
    std::string config_path;
    std::string log_path;
    std::string analysis_root;
    std::optional<std::uint32_t> hunting;
    std::optional<bool> analysis_active;
    std::uint64_t context_pointer = 0;
    std::uint64_t before_draw_pointer = 0;
    ProbeDrawInfo draw;
    std::vector<ProbeShaderInfo> shaders;
    std::vector<ProbeInputElement> input_elements;
    std::vector<ProbeBufferBinding> vertex_buffers;
    std::optional<ProbeBufferBinding> index_buffer;
    std::vector<ProbeResourceInfo> resources;
    std::vector<ProbeStateValue> states;
    std::vector<std::string> missing_fields;
    std::vector<std::string> limits_hit;
    std::vector<std::string> warnings;
};


#pragma once

#include <filesystem>
#include <string>

#include "ProbeTypes.h"

std::string SerializeProbeSnapshot(const LiveProbeSnapshot& snapshot);

bool WriteProbeSnapshot(
    const std::filesystem::path& path,
    const LiveProbeSnapshot& snapshot,
    std::string& error);


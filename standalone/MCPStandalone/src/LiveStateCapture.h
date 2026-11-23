#pragma once

#include <Windows.h>
#include <d3d11_1.h>
#include <dxgi.h>

#include <filesystem>
#include <string>

#include "ProbeTypes.h"

std::string ProbeFormatName(DXGI_FORMAT format);
std::string ProbeResourceDimensionName(D3D11_RESOURCE_DIMENSION dimension);
std::string ProbeSnapshotDirectory(const std::string& analysis_root);

bool RetainNewestProbeSnapshots(
    const std::filesystem::path& directory,
    std::size_t keep,
    std::string& error);

bool ProbeHackerInputLayoutElements(
    const ID3D11InputLayout* layout,
    D3D11_INPUT_ELEMENT_DESC* elements,
    std::size_t capacity,
    UINT& count);

bool CaptureImmediateContext(
    ID3D11Device1* device,
    LiveProbeSnapshot& snapshot,
    std::string& error);

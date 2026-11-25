#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "LiveStateCapture.h"
#include "TestHarness.h"

namespace {

// Mirrors the member order of 3DMigoto's HackerInputLayout, whose element list
// is private and non-virtual, so the probe can only reach it through the object
// image.
class FakeHackerInputLayout final : public ID3D11InputLayout {
public:
    explicit FakeHackerInputLayout(
        std::vector<D3D11_INPUT_ELEMENT_DESC> elements)
        : elements_(std::move(elements)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID,
        void**) override {
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++reference_count_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return --reference_count_;
    }
    void STDMETHODCALLTYPE GetDevice(
        ID3D11Device**) override {}
    HRESULT STDMETHODCALLTYPE GetPrivateData(
        REFGUID,
        UINT*,
        void*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(
        REFGUID,
        UINT,
        const void*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
        REFGUID,
        const IUnknown*) override {
        return E_NOTIMPL;
    }

private:
    std::atomic<ULONG> reference_count_{1};
    ID3D11InputLayout* origin_ = nullptr;
    std::vector<std::uint8_t> signature_;
    std::uint64_t layout_hash_ = 0;
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements_;
    std::vector<std::string> semantic_names_;
};

}  // namespace

TEST_CASE(LiveStateCaptureNamesKnownFormats) {
    REQUIRE_EQ(
        ProbeFormatName(DXGI_FORMAT_R8G8B8A8_UNORM),
        std::string("R8G8B8A8_UNORM"));
    REQUIRE_EQ(
        ProbeFormatName(DXGI_FORMAT_BC7_UNORM),
        std::string("BC7_UNORM"));
    REQUIRE_EQ(
        ProbeFormatName(DXGI_FORMAT_D24_UNORM_S8_UINT),
        std::string("D24_UNORM_S8_UINT"));
    REQUIRE_EQ(
        ProbeResourceDimensionName(D3D11_RESOURCE_DIMENSION_TEXTURE2D),
        std::string("texture2d"));
    REQUIRE_EQ(
        ProbeResourceDimensionName(D3D11_RESOURCE_DIMENSION_BUFFER),
        std::string("buffer"));
}

TEST_CASE(LiveStateCaptureBuildsBoundedSnapshotDirectory) {
    const auto path = ProbeSnapshotDirectory("D:\\game\\XXMI");
    REQUIRE(path.find("xxmi_mcp_snapshots") != std::string::npos);
    REQUIRE(path.find("D:\\game\\XXMI") == 0);
}

TEST_CASE(LiveStateCaptureCopiesOfficialInputLayoutDeclaration) {
    std::array<D3D11_INPUT_ELEMENT_DESC, 1> source = {{
        "POSITION",
        0,
        DXGI_FORMAT_R32G32B32_FLOAT,
        0,
        0,
        D3D11_INPUT_PER_VERTEX_DATA,
        0,
    }};
    FakeHackerInputLayout layout(
        std::vector<D3D11_INPUT_ELEMENT_DESC>(
            source.begin(), source.end()));
    std::array<D3D11_INPUT_ELEMENT_DESC, 32> elements = {};
    UINT count = 0;
    REQUIRE(ProbeHackerInputLayoutElements(
        &layout, elements.data(), elements.size(), count));
    REQUIRE_EQ(count, 1U);
    REQUIRE_EQ(
        std::string(elements[0].SemanticName),
        std::string("POSITION"));
    REQUIRE_EQ(
        elements[0].Format,
        DXGI_FORMAT_R32G32B32_FLOAT);
}

TEST_CASE(LiveStateCaptureRejectsAnUnreadableInputLayout) {
    std::array<D3D11_INPUT_ELEMENT_DESC, 32> elements = {};
    UINT count = 1;
    REQUIRE(!ProbeHackerInputLayoutElements(
        nullptr, elements.data(), elements.size(), count));
    REQUIRE_EQ(count, 0U);
}

TEST_CASE(LiveStateCaptureRetainsNewestSnapshotFiles) {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("xxmi-mcp-retention-" +
         std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    REQUIRE(!error);

    const auto base_time =
        std::filesystem::file_time_type::clock::now();
    for (int index = 0; index < 22; ++index) {
        const auto path =
            directory /
            ("live-context-" + std::to_string(index) + ".json");
        std::ofstream(path) << "{}";
        std::filesystem::last_write_time(
            path,
            base_time + std::chrono::seconds(index),
            error);
        REQUIRE(!error);
    }
    std::ofstream(directory / "unrelated.txt") << "keep";

    std::string retain_error;
    REQUIRE(RetainNewestProbeSnapshots(
        directory, 20, retain_error));
    REQUIRE(std::filesystem::exists(
        directory / "live-context-21.json"));
    REQUIRE(!std::filesystem::exists(
        directory / "live-context-0.json"));
    REQUIRE(std::filesystem::exists(
        directory / "unrelated.txt"));

    std::filesystem::remove_all(directory, error);
}

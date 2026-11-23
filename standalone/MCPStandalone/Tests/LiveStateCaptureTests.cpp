#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "LiveStateCapture.h"
#include "TestHarness.h"

namespace {

class FakeProbeInputLayout final : public ID3D11InputLayout {
public:
    explicit FakeProbeInputLayout(
        std::vector<D3D11_INPUT_ELEMENT_DESC> elements)
        : elements_(std::move(elements)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID,
        void**) override {
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return --references_;
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

    virtual ID3D11InputLayout* GetOrigInputLayout() const {
        return nullptr;
    }
    virtual const void* GetShaderSignature() const {
        return nullptr;
    }
    virtual SIZE_T GetShaderSignatureSize() const {
        return 0;
    }
    virtual UINT GetElementCount() const {
        return static_cast<UINT>(elements_.size());
    }
    virtual const D3D11_INPUT_ELEMENT_DESC* GetElements() const {
        return elements_.data();
    }

private:
    ULONG references_ = 1;
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements_;
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
    FakeProbeInputLayout layout(
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


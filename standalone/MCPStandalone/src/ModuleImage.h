#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct FunctionRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

struct SectionView {
    std::string name;
    std::uintptr_t address = 0;
    std::span<const std::uint8_t> bytes;
    std::uint32_t characteristics = 0;
};

class ModuleImage {
public:
    static std::optional<ModuleImage> Load(HMODULE module);
    static std::string LastLoadError();

    std::uintptr_t Base() const;
    std::size_t Size() const;
    std::uint32_t Timestamp() const;
    std::uint32_t Checksum() const;

    std::optional<SectionView> Section(std::string_view name) const;
    std::optional<SectionView> SectionContaining(
        std::uintptr_t address, std::size_t size = 1) const;
    bool Read(std::uintptr_t address, void* destination,
              std::size_t size) const;
    bool ContainsAddress(std::uintptr_t address, std::size_t size) const;
    bool IsWritable(std::uintptr_t address, std::size_t size) const;
    bool IsFunctionStart(std::uintptr_t address) const;
    std::optional<FunctionRange> FunctionContaining(
        std::uintptr_t address) const;
    std::vector<FunctionRange> FunctionRanges() const;
    std::optional<std::uintptr_t> FindBytes(
        std::string_view section, std::span<const std::uint8_t> bytes) const;

private:
    struct SectionRecord {
        std::string name;
        std::uintptr_t address = 0;
        std::span<const std::uint8_t> bytes;
        std::uint32_t characteristics = 0;
    };

    std::uintptr_t base_ = 0;
    std::size_t size_ = 0;
    std::uint32_t timestamp_ = 0;
    std::uint32_t checksum_ = 0;
    std::span<const std::uint8_t> exception_table_;
    std::vector<SectionRecord> sections_;
};

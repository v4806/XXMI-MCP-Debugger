#include "ModuleImage.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>

namespace {

std::mutex g_error_lock;
std::string g_last_load_error;

std::optional<ModuleImage> Fail(std::string message) {
    std::lock_guard lock(g_error_lock);
    g_last_load_error = std::move(message);
    return std::nullopt;
}

bool AddWouldOverflow(std::uintptr_t value, std::size_t size) {
    return size > std::numeric_limits<std::uintptr_t>::max() - value;
}

}  // namespace

std::optional<ModuleImage> ModuleImage::Load(HMODULE module) {
    {
        std::lock_guard lock(g_error_lock);
        g_last_load_error.clear();
    }
    if (!module)
        return Fail("module handle is null");

    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return Fail("invalid DOS header");

    const auto nt_address = base + static_cast<std::uintptr_t>(dos->e_lfanew);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(nt_address);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return Fail("module is not a PE32+ image");

    ModuleImage image;
    image.base_ = base;
    image.size_ = nt->OptionalHeader.SizeOfImage;
    image.timestamp_ = nt->FileHeader.TimeDateStamp;
    image.checksum_ = nt->OptionalHeader.CheckSum;

    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections;
         ++index, ++section) {
        char raw_name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
        std::memcpy(raw_name, section->Name, IMAGE_SIZEOF_SHORT_NAME);

        const auto virtual_size =
            std::max(section->Misc.VirtualSize, section->SizeOfRawData);
        if (virtual_size == 0)
            continue;

        const auto section_address =
            base + static_cast<std::uintptr_t>(section->VirtualAddress);
        if (AddWouldOverflow(section_address, virtual_size) ||
            section_address < base ||
            section_address + virtual_size > base + image.size_) {
            return Fail("section lies outside SizeOfImage");
        }

        image.sections_.push_back({
            raw_name,
            section_address,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(section_address),
                virtual_size),
            section->Characteristics,
        });
    }

    const auto& exception_directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exception_directory.VirtualAddress != 0 &&
        exception_directory.Size >= sizeof(RUNTIME_FUNCTION)) {
        const auto exception_address =
            base + exception_directory.VirtualAddress;
        if (AddWouldOverflow(exception_address, exception_directory.Size) ||
            exception_address < base ||
            exception_address + exception_directory.Size > base + image.size_) {
            std::ostringstream message;
            message << "exception directory lies outside SizeOfImage: rva=0x"
                    << std::hex << exception_directory.VirtualAddress
                    << " size=0x" << exception_directory.Size
                    << " image=0x" << image.size_
                    << " base=0x" << base
                    << " exception_end=0x"
                    << (exception_address + exception_directory.Size)
                    << " image_end=0x" << (base + image.size_)
                    << " overflow="
                    << AddWouldOverflow(exception_address,
                                        exception_directory.Size);
            return Fail(message.str());
        }
        image.exception_table_ = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(exception_address),
            exception_directory.Size);
    }

    if (image.sections_.empty())
        return Fail("module has no sections");
    return image;
}

std::string ModuleImage::LastLoadError() {
    std::lock_guard lock(g_error_lock);
    return g_last_load_error;
}

std::uintptr_t ModuleImage::Base() const {
    return base_;
}

std::size_t ModuleImage::Size() const {
    return size_;
}

std::uint32_t ModuleImage::Timestamp() const {
    return timestamp_;
}

std::uint32_t ModuleImage::Checksum() const {
    return checksum_;
}

std::optional<SectionView> ModuleImage::Section(std::string_view name) const {
    for (const auto& section : sections_) {
        if (section.name == name)
            return SectionView{section.name, section.address, section.bytes,
                               section.characteristics};
    }
    return std::nullopt;
}

std::optional<SectionView> ModuleImage::SectionContaining(
    std::uintptr_t address,
    std::size_t size) const {
    if (size == 0 || AddWouldOverflow(address, size))
        return std::nullopt;
    for (const auto& section : sections_) {
        const auto section_end = section.address + section.bytes.size();
        if (address >= section.address &&
            address + size <= section_end) {
            return SectionView{
                section.name,
                section.address,
                section.bytes,
                section.characteristics,
            };
        }
    }
    return std::nullopt;
}

bool ModuleImage::Read(std::uintptr_t address, void* destination,
                       std::size_t size) const {
    if (!destination || size == 0 || address < base_ ||
        AddWouldOverflow(address, size) || address + size > base_ + size_) {
        return false;
    }

    for (const auto& section : sections_) {
        const auto section_end = section.address + section.bytes.size();
        if (address >= section.address && address + size <= section_end) {
            std::memcpy(destination, reinterpret_cast<const void*>(address), size);
            return true;
        }
    }
    return false;
}

bool ModuleImage::ContainsAddress(
    std::uintptr_t address,
    std::size_t size) const {
    if (size == 0 || address < base_ || AddWouldOverflow(address, size) ||
        address + size > base_ + size_) {
        return false;
    }
    return std::any_of(
        sections_.begin(), sections_.end(),
        [address, size](const auto& section) {
            return address >= section.address &&
                   address + size <= section.address + section.bytes.size();
        });
}

bool ModuleImage::IsWritable(std::uintptr_t address, std::size_t size) const {
    if (size == 0 || address < base_ || AddWouldOverflow(address, size) ||
        address + size > base_ + size_) {
        return false;
    }
    for (const auto& section : sections_) {
        const auto section_end = section.address + section.bytes.size();
        if (address < section.address || address + size > section_end)
            continue;
        return (section.characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    }
    return false;
}

bool ModuleImage::IsFunctionStart(std::uintptr_t address) const {
    const auto ranges = FunctionRanges();
    return std::any_of(
        ranges.begin(), ranges.end(),
        [address](const auto& function) { return function.begin == address; });
}

std::optional<FunctionRange> ModuleImage::FunctionContaining(
    std::uintptr_t address) const {
    const auto ranges = FunctionRanges();
    for (const auto& range : ranges) {
        if (range.begin <= address && address < range.end)
            return range;
    }
    return std::nullopt;
}

std::vector<FunctionRange> ModuleImage::FunctionRanges() const {
    std::vector<FunctionRange> ranges;
    const auto count =
        exception_table_.size() / sizeof(RUNTIME_FUNCTION);
    ranges.reserve(count);

    const auto* functions =
        reinterpret_cast<const RUNTIME_FUNCTION*>(exception_table_.data());
    for (std::size_t index = 0; index < count; ++index) {
        const auto begin = base_ + functions[index].BeginAddress;
        const auto end = base_ + functions[index].EndAddress;
        if (begin < end && begin >= base_ && end <= base_ + size_)
            ranges.push_back({begin, end});
    }

    std::sort(ranges.begin(), ranges.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.begin < rhs.begin;
              });
    return ranges;
}

std::optional<std::uintptr_t> ModuleImage::FindBytes(
    std::string_view section_name,
    std::span<const std::uint8_t> bytes) const {
    if (bytes.empty())
        return std::nullopt;
    const auto section = Section(section_name);
    if (!section || bytes.size() > section->bytes.size())
        return std::nullopt;

    const auto end = section->bytes.end() - bytes.size() + 1;
    const auto found = std::search(section->bytes.begin(), end,
                                   bytes.begin(), bytes.end());
    if (found == end)
        return std::nullopt;
    return section->address +
           static_cast<std::uintptr_t>(found - section->bytes.begin());
}

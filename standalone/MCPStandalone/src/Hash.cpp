#include "Hash.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

std::string ErrorMessage(const char* operation, NTSTATUS status) {
    std::ostringstream stream;
    stream << operation << " failed with NTSTATUS 0x" << std::hex
           << static_cast<unsigned long>(status);
    return stream.str();
}

}  // namespace

bool ComputeFileSha256(
    const std::filesystem::path& path,
    std::string& digest_hex,
    std::string& error) {
    digest_hex.clear();
    error.clear();

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "could not open file for SHA-256";
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> hash_object;
    std::vector<std::uint8_t> digest(32);

    auto status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        error = ErrorMessage("BCryptOpenAlgorithmProvider", status);
        return false;
    }

    DWORD object_size = 0;
    DWORD result_size = 0;
    status = BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size),
        sizeof(object_size), &result_size, 0);
    if (status < 0) {
        error = ErrorMessage("BCryptGetProperty", status);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    hash_object.resize(object_size);

    status = BCryptCreateHash(
        algorithm, &hash, hash_object.data(), object_size,
        nullptr, 0, 0);
    if (status < 0) {
        error = ErrorMessage("BCryptCreateHash", status);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    std::array<char, 64 * 1024> buffer = {};
    while (stream) {
        stream.read(buffer.data(), buffer.size());
        const auto count = stream.gcount();
        if (count > 0) {
            status = BCryptHashData(
                hash,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count),
                0);
            if (status < 0) {
                error = ErrorMessage("BCryptHashData", status);
                BCryptDestroyHash(hash);
                BCryptCloseAlgorithmProvider(algorithm, 0);
                return false;
            }
        }
    }
    if (!stream.eof()) {
        error = "could not read file for SHA-256";
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    status = BCryptFinishHash(
        hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        error = ErrorMessage("BCryptFinishHash", status);
        return false;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    digest_hex.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        digest_hex[index * 2] = kHex[digest[index] >> 4];
        digest_hex[index * 2 + 1] = kHex[digest[index] & 0x0F];
    }
    return true;
}

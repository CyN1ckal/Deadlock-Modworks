#include "content_hash.hpp"

#include "error.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <limits>
#include <vector>

namespace skeleton_repair {
namespace {

class Sha256Hasher {
public:
    Sha256Hasher() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            throw IoError("SHA-256 is unavailable.");
        }
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw IoError("SHA-256 is unavailable.");
        }
    }

    ~Sha256Hasher() {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    void Update(const uint8_t* data, size_t size) {
        size_t done = 0;
        while (done < size) {
            const ULONG chunk = static_cast<ULONG>(std::min<size_t>(size - done, std::numeric_limits<ULONG>::max()));
            if (BCryptHashData(hash_, const_cast<PUCHAR>(data + done), chunk, 0) < 0) {
                throw IoError("SHA-256 update failed.");
            }
            done += chunk;
        }
    }

    std::array<uint8_t, 32> Finish() {
        std::array<uint8_t, 32> out{};
        if (BCryptFinishHash(hash_, out.data(), static_cast<ULONG>(out.size()), 0) < 0) {
            throw IoError("SHA-256 finish failed.");
        }
        return out;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    ~FileHandle() { reset(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }

    void reset() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

}  // namespace

std::array<uint8_t, 32> Sha256(std::span<const uint8_t> data) {
    Sha256Hasher hasher;
    if (!data.empty()) {
        hasher.Update(data.data(), data.size());
    }
    return hasher.Finish();
}

std::array<uint8_t, 32> Sha256File(const std::filesystem::path& path) {
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        throw IoError("Could not read " + path.string() + ".");
    }

    Sha256Hasher hasher;
    std::vector<uint8_t> buffer(1024 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
            throw IoError("Could not read " + path.string() + ".");
        }
        if (got == 0) {
            break;
        }
        hasher.Update(buffer.data(), got);
    }
    return hasher.Finish();
}

std::string ToLowerHex(std::span<const uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex(bytes.size() * 2, '\0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        hex[i * 2] = kDigits[bytes[i] >> 4];
        hex[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
    }
    return hex;
}

}  // namespace skeleton_repair

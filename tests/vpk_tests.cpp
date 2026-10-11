#include "archive_transaction.hpp"
#include "vpk.hpp"

#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <stdexcept>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using skeleton_repair::ArchiveTransaction;
using skeleton_repair::CancellationSource;
using skeleton_repair::InvalidData;
using skeleton_repair::IoError;
using skeleton_repair::OperationCancelled;
using skeleton_repair::VpkArchive;
using skeleton_repair::VpkChange;

class TempDir {
public:
    TempDir() {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = fs::temp_directory_path() / ("dsr-" + std::to_string(ticks));
        fs::create_directories(dir_);
    }

    ~TempDir() {
        std::error_code error;
        fs::remove_all(dir_, error);
    }

    const fs::path& Path() const { return dir_; }

private:
    fs::path dir_;
};

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output.good()) {
        throw std::runtime_error("Could not write " + path.string());
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!output.good()) {
        throw std::runtime_error("Could not write " + path.string());
    }
}

std::vector<uint8_t> ReadBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.good());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void AppendU16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

uint32_t LoadU32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset])
        | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

std::array<uint8_t, 16> Md5Of(const uint8_t* data, size_t size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0
        || BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        ADD_FAILURE() << "MD5 is unavailable";
        return {};
    }
    if (size != 0 && BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) < 0) {
        ADD_FAILURE() << "MD5 update failed";
    }
    std::array<uint8_t, 16> out{};
    if (BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0) < 0) {
        ADD_FAILURE() << "MD5 finish failed";
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return out;
}

fs::path WriteSeed(const fs::path& path) {
    std::vector<uint8_t> bytes;
    AppendU32(bytes, 0x55AA1234u);
    AppendU32(bytes, 2u);
    AppendU32(bytes, 1u);
    AppendU32(bytes, 0u);
    bytes.resize(28, 0);
    bytes.push_back(0);
    WriteBytes(path, bytes);
    return path;
}

VpkArchive ArchiveWith(const fs::path& path, const std::vector<VpkChange>& changes) {
    WriteSeed(path);
    VpkArchive::Open(path).Write(path, changes);
    return VpkArchive::Open(path);
}

const skeleton_repair::VpkEntry& Only(const VpkArchive& archive) {
    EXPECT_EQ(archive.Entries().size(), 1u);
    return archive.Entries().front();
}

}  // namespace

TEST(Vpk, CrcMatchesZipPolynomial) {
    const char* text = "123456789";
    EXPECT_EQ(VpkArchive::Crc(reinterpret_cast<const uint8_t*>(text), 9), 0xCBF43926u);
    const std::vector<uint8_t> bytes{1, 2, 3, 4};
    EXPECT_EQ(VpkArchive::Crc(bytes), VpkArchive::Crc(bytes.data(), bytes.size()));
}

TEST(Vpk, RoundTripPreservesBytesAndChecksumFooter) {
    TempDir dir;
    const fs::path path = dir.Path() / "mod.vpk";
    const std::vector<uint8_t> model{9, 8, 7};
    const std::vector<uint8_t> note{1, 2, 3, 4};
    const VpkArchive archive = ArchiveWith(path, {
        VpkChange{"models/heroes/test/test.vmdl_c", model},
        VpkChange{"materials/custom.txt", note},
    });
    archive.Validate();
    EXPECT_EQ(archive.Read(archive.Entries().at(0)).size() + archive.Read(archive.Entries().at(1)).size(), model.size() + note.size());
    const auto model_entry = std::find_if(archive.Entries().begin(), archive.Entries().end(), [](const auto& entry) {
        return entry.FullPath == "models/heroes/test/test.vmdl_c";
    });
    const auto note_entry = std::find_if(archive.Entries().begin(), archive.Entries().end(), [](const auto& entry) {
        return entry.FullPath == "materials/custom.txt";
    });
    ASSERT_NE(model_entry, archive.Entries().end());
    ASSERT_NE(note_entry, archive.Entries().end());
    EXPECT_EQ(archive.Read(*model_entry), model);
    EXPECT_EQ(archive.Read(*note_entry), note);
    EXPECT_EQ(model_entry->ArchiveIndex, VpkArchive::EmbeddedArchive);

    const std::vector<uint8_t> file = ReadBytes(path);
    const uint32_t tree_size = LoadU32(file, 8);
    const uint32_t data_size = LoadU32(file, 12);
    ASSERT_EQ(file.size(), 28u + tree_size + data_size + 48u);
    EXPECT_EQ(LoadU32(file, 20), 48u);
    EXPECT_EQ(LoadU32(file, 16), 0u);
    const auto tree_md5 = Md5Of(file.data() + 28, tree_size);
    EXPECT_TRUE(std::equal(tree_md5.begin(), tree_md5.end(), file.end() - 48));
    const auto empty_md5 = Md5Of(nullptr, 0);
    EXPECT_TRUE(std::equal(empty_md5.begin(), empty_md5.end(), file.end() - 32));
    const auto whole_md5 = Md5Of(file.data(), file.size() - 16);
    EXPECT_TRUE(std::equal(whole_md5.begin(), whole_md5.end(), file.end() - 16));
}

TEST(Vpk, CrcCorruptionIsRejected) {
    TempDir dir;
    const fs::path path = dir.Path() / "corrupt.vpk";
    ArchiveWith(path, {VpkChange{"a.txt", std::vector<uint8_t>{1, 2, 3}}});
    auto bytes = ReadBytes(path);
    bytes[28 + LoadU32(bytes, 8)] ^= 0xFF;
    WriteBytes(path, bytes);
    EXPECT_THROW(VpkArchive::Open(path).Validate(), InvalidData);
}

TEST(Vpk, TruncatedDirectoriesAreRejected) {
    TempDir dir;
    const fs::path path = dir.Path() / "truncated.vpk";
    ArchiveWith(path, {VpkChange{"a.txt", std::vector<uint8_t>{1}}});
    auto bytes = ReadBytes(path);
    bytes.resize(28);
    WriteBytes(path, bytes);
    EXPECT_THROW(VpkArchive::Open(path), InvalidData);
}

TEST(Vpk, RejectsAFileThatIsNotASource2Vpk) {
    TempDir dir;
    const fs::path path = dir.Path() / "notes.vpk";
    WriteBytes(path, std::vector<uint8_t>(29, 0));
    try {
        VpkArchive::Open(path);
        FAIL() << "Expected InvalidData";
    } catch (const InvalidData& error) {
        EXPECT_NE(std::string(error.what()).find("notes.vpk is not a Source 2 VPK."), std::string::npos);
    }
}

TEST(Vpk, PreloadBytesSurviveRepacking) {
    TempDir dir;
    const fs::path path = dir.Path() / "preload.vpk";
    const std::vector<uint8_t> payload{1, 2, 3, 4};
    std::vector<uint8_t> tree;
    auto add_string = [&](const char* text) {
        tree.insert(tree.end(), text, text + std::char_traits<char>::length(text));
        tree.push_back(0);
    };
    add_string("txt");
    add_string(" ");
    add_string("sample");
    AppendU32(tree, VpkArchive::Crc(payload));
    AppendU16(tree, 2);
    AppendU16(tree, VpkArchive::EmbeddedArchive);
    AppendU32(tree, 0);
    AppendU32(tree, 2);
    AppendU16(tree, 0xFFFF);
    tree.push_back(1);
    tree.push_back(2);
    tree.insert(tree.end(), 3, 0);

    std::vector<uint8_t> file;
    AppendU32(file, 0x55AA1234u);
    AppendU32(file, 2u);
    AppendU32(file, static_cast<uint32_t>(tree.size()));
    AppendU32(file, 2u);
    file.resize(28, 0);
    file.insert(file.end(), tree.begin(), tree.end());
    file.push_back(3);
    file.push_back(4);
    WriteBytes(path, file);

    const VpkArchive preload = VpkArchive::Open(path);
    EXPECT_EQ(preload.Read(Only(preload)), payload);
    const fs::path repacked = dir.Path() / "preload-repacked.vpk";
    ArchiveTransaction::Write(preload, path, repacked, {}, false);
    const VpkArchive restored = VpkArchive::Open(repacked);
    EXPECT_EQ(restored.Read(Only(restored)), payload);
    restored.Validate();
    EXPECT_EQ(Only(restored).ArchiveIndex, VpkArchive::EmbeddedArchive);
    EXPECT_EQ(Only(restored).PreloadBytes, 0);
}

TEST(Vpk, ChunkedReadRepacksIntoOneArchive) {
    TempDir dir;
    const fs::path directory = dir.Path() / "pak01_dir.vpk";
    const std::vector<uint8_t> payload{4, 5, 6, 7};
    std::vector<uint8_t> tree;
    auto add_string = [&](const char* text) {
        tree.insert(tree.end(), text, text + std::char_traits<char>::length(text));
        tree.push_back(0);
    };
    add_string("txt");
    add_string(" ");
    add_string("hero");
    AppendU32(tree, VpkArchive::Crc(payload));
    AppendU16(tree, 0);
    AppendU16(tree, 0);
    AppendU32(tree, 0);
    AppendU32(tree, static_cast<uint32_t>(payload.size()));
    AppendU16(tree, 0xFFFF);
    tree.insert(tree.end(), 3, 0);

    std::vector<uint8_t> file;
    AppendU32(file, 0x55AA1234u);
    AppendU32(file, 2u);
    AppendU32(file, static_cast<uint32_t>(tree.size()));
    AppendU32(file, 0u);
    file.resize(28, 0);
    file.insert(file.end(), tree.begin(), tree.end());
    WriteBytes(directory, file);
    WriteBytes(dir.Path() / "pak01_000.vpk", payload);

    const VpkArchive archive = VpkArchive::Open(directory);
    EXPECT_EQ(archive.Read(Only(archive)), payload);
    const fs::path embedded = dir.Path() / "embedded.vpk";
    ArchiveTransaction::Write(archive, directory, embedded, {}, false);
    const VpkArchive restored = VpkArchive::Open(embedded);
    EXPECT_EQ(restored.Read(Only(restored)), payload);
    EXPECT_EQ(Only(restored).ArchiveIndex, VpkArchive::EmbeddedArchive);
}

TEST(Vpk, ExistingOutputIsNeverOverwritten) {
    TempDir dir;
    const fs::path source = dir.Path() / "mod.vpk";
    const fs::path output = dir.Path() / "out.vpk";
    const VpkArchive archive = ArchiveWith(source, {VpkChange{"a.txt", std::vector<uint8_t>{1, 2, 3}}});
    ArchiveWith(output, {VpkChange{"a.txt", std::vector<uint8_t>{9}}});
    const auto before = ReadBytes(output);
    EXPECT_THROW(ArchiveTransaction::Write(archive, source, output, {}, false), IoError);
    EXPECT_EQ(ReadBytes(output), before);
}

TEST(Vpk, ReplacementCreatesAnExactOriginalBackup) {
    TempDir dir;
    const fs::path source = dir.Path() / "mod.vpk";
    const auto original = [&] {
        ArchiveWith(source, {VpkChange{"a.txt", std::vector<uint8_t>{1, 2, 3}}});
        return ReadBytes(source);
    }();
    const VpkArchive archive = VpkArchive::Open(source);
    const std::vector<uint8_t> replacement{8, 7, 6};
    const auto backup = ArchiveTransaction::Write(archive, source, source,
        {VpkChange{"a.txt", replacement}}, true);
    ASSERT_TRUE(backup.has_value());
    EXPECT_NE(backup->filename().string().find(".bak-"), std::string::npos);
    EXPECT_EQ(ReadBytes(*backup), original);
    const VpkArchive repaired = VpkArchive::Open(source);
    EXPECT_EQ(repaired.Read(Only(repaired)), replacement);
    EXPECT_EQ(fs::directory_iterator(dir.Path())->path().empty(), false);
    for (const auto& entry : fs::directory_iterator(dir.Path())) {
        EXPECT_FALSE(entry.path().filename().string().ends_with(".new"));
    }
}

TEST(Vpk, CancellationLeavesTheSourceUntouched) {
    TempDir dir;
    const fs::path source = dir.Path() / "mod.vpk";
    ArchiveWith(source, {VpkChange{"a.txt", std::vector<uint8_t>{1, 2, 3}}});
    const auto before = ReadBytes(source);
    CancellationSource cancellation;
    cancellation.Cancel();
    EXPECT_THROW(ArchiveTransaction::Write(VpkArchive::Open(source), source, source, {}, true, cancellation.Token()), OperationCancelled);
    EXPECT_EQ(ReadBytes(source), before);
    for (const auto& entry : fs::directory_iterator(dir.Path())) {
        EXPECT_FALSE(entry.path().filename().string().ends_with(".new"));
    }
}

TEST(Vpk, FailedCopyNeverInstallsAPartialArchive) {
    TempDir dir;
    const fs::path source = dir.Path() / "bad-copy.vpk";
    ArchiveWith(source, {VpkChange{"a.txt", std::vector<uint8_t>{1, 2, 3}}});
    auto bytes = ReadBytes(source);
    bytes[28 + LoadU32(bytes, 8)] ^= 0xFF;
    WriteBytes(source, bytes);
    EXPECT_THROW(ArchiveTransaction::Write(VpkArchive::Open(source), source, source, {}, true), InvalidData);
    EXPECT_EQ(ReadBytes(source), bytes);
    for (const auto& entry : fs::directory_iterator(dir.Path())) {
        EXPECT_FALSE(entry.path().filename().string().ends_with(".new"));
    }
}

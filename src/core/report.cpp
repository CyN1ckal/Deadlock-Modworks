#include "report.hpp"

#include "error.hpp"
#include "strings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace skeleton_repair {
namespace {

using Json = nlohmann::ordered_json;

std::wstring FullPath(const std::wstring& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        throw IoError("Could not resolve a full path.");
    }
    std::wstring buffer(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) {
        throw IoError("Could not resolve a full path.");
    }
    buffer.resize(written);
    return buffer;
}

std::string NewId() {
    uint8_t bytes[16];
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw IoError("Could not create a report id.");
    }
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(32, '0');
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        text[i * 2] = kDigits[bytes[i] >> 4];
        text[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
    }
    return text;
}

std::string UtcTimestamp() {
    FILETIME file_time{};
    GetSystemTimePreciseAsFileTime(&file_time);
    SYSTEMTIME time{};
    if (FileTimeToSystemTime(&file_time, &time) == 0) {
        throw IoError("Could not read the clock.");
    }
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    const unsigned fraction = static_cast<unsigned>(ticks.QuadPart % 10000000ULL);

    char buffer[48];
    if (fraction == 0) {
        sprintf_s(buffer, "%04u-%02u-%02uT%02u:%02u:%02u+00:00",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        return buffer;
    }
    char digits[8];
    sprintf_s(digits, "%07u", fraction);
    size_t end = 7;
    while (end > 1 && digits[end - 1] == '0') {
        --end;
    }
    digits[end] = '\0';
    sprintf_s(buffer, "%04u-%02u-%02uT%02u:%02u:%02u.%s+00:00",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, digits);
    return buffer;
}

const char* OutcomeName(ModelOutcome outcome) {
    switch (outcome) {
    case ModelOutcome::Repairable: return "Repairable";
    case ModelOutcome::NeedsReview: return "NeedsReview";
    case ModelOutcome::Updated: return "Updated";
    case ModelOutcome::Current: return "Current";
    case ModelOutcome::Skipped: return "Skipped";
    case ModelOutcome::Failed: return "Failed";
    }
    return "Failed";
}

Json ResultJson(const ModelResult& result) {
    Json json = Json::object();
    json["Model"] = result.Model;
    json["Outcome"] = OutcomeName(result.Outcome);
    json["Detail"] = result.Detail;
    if (!result.Warnings) {
        json["Warnings"] = nullptr;
    } else {
        json["Warnings"] = Json::array();
        for (const std::string& warning : *result.Warnings) {
            json["Warnings"].push_back(warning);
        }
    }
    if (!result.BoneRenames) {
        json["BoneRenames"] = nullptr;
    } else {
        json["BoneRenames"] = Json::object();
        for (const auto& rename : *result.BoneRenames) {
            json["BoneRenames"][rename.first] = rename.second;
        }
    }
    return json;
}

Json ReportJson(const UpdateReport& report) {
    Json json = Json::object();
    json["Results"] = Json::array();
    for (const ModelResult& result : report.Results) {
        json["Results"].push_back(ResultJson(result));
    }
    json["WroteFile"] = report.WroteFile;
    json["OutputPath"] = report.OutputPath ? Json(*report.OutputPath) : Json(nullptr);
    json["BackupPath"] = report.BackupPath ? Json(*report.BackupPath) : Json(nullptr);
    json["HasFailures"] = report.HasFailures();
    json["HasBlockers"] = report.HasBlockers();
    json["CanRepair"] = report.CanRepair();
    return json;
}

bool EndsWithIgnoreCase(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && EqualsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
}

}  // namespace

std::string ReportExporter::Serialize(std::string_view game_directory, const std::vector<ArchiveReport>& archives) {
    Json root = Json::object();
    root["SchemaVersion"] = 2;
    root["GeneratedUtc"] = UtcTimestamp();
    root["GameDirectory"] = std::string(game_directory);
    root["Scope"] = Scope;
    root["Archives"] = Json::array();
    for (const ArchiveReport& archive : archives) {
        Json item = Json::object();
        item["ModPath"] = archive.ModPath;
        item["Report"] = ReportJson(archive.Report);
        root["Archives"].push_back(std::move(item));
    }
    return root.dump(2);
}

std::string ReportExporter::Write(const std::string& path, std::string_view game_directory, const std::vector<ArchiveReport>& archives) {
    if (!EndsWithIgnoreCase(path, ".json")) {
        throw std::invalid_argument("Reports must be saved as .json files.");
    }
    const std::wstring full = FullPath(Widen(path));
    const std::wstring parent = std::filesystem::path(full).parent_path().wstring();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(parent), error);
        if (error) {
            throw IoError("Could not create the report directory.");
        }
    }

    const std::string text = Serialize(game_directory, archives);
    const std::wstring temp = full + L"." + Widen(NewId()) + L".tmp";
    try {
        {
            std::ofstream output(std::filesystem::path(temp), std::ios::binary);
            if (!output) {
                throw IoError("Could not create the report file.");
            }
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!output) {
                throw IoError("Could not create the report file.");
            }
        }
        if (MoveFileExW(temp.c_str(), full.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
            throw IoError("Could not replace the report file.");
        }
    } catch (...) {
        DeleteFileW(temp.c_str());
        throw;
    }
    DeleteFileW(temp.c_str());
    return Narrow(full);
}

}  // namespace skeleton_repair

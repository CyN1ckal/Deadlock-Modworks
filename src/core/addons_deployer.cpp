#include "addons_deployer.hpp"

#include "error.hpp"
#include "strings.hpp"
#include "vpk.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace skeleton_repair {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

constexpr const wchar_t* kTempPrefix = L".modworks-";

int64_t Stamp(const fs::path& path) { return fs::last_write_time(path).time_since_epoch().count(); }

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

void WriteReplacing(const fs::path& file, const std::string& text) {
    auto temp = file;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) throw IoError("Could not write " + Narrow(file.wstring()) + ".");
    }
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError();
        std::error_code error;
        fs::remove(temp, error);
        throw IoError("Could not write " + Narrow(file.wstring()) + ". Windows error " + std::to_string(code) + ".");
    }
}

std::vector<AddonFile> ReadManifest(const fs::path& addons, std::vector<std::string>& notes) {
    const auto file = addons / kAddonsManifestName;
    std::error_code error;
    if (!fs::exists(file, error)) return {};
    std::ifstream in(file, std::ios::binary);
    const auto parsed = json::parse(std::string((std::istreambuf_iterator<char>(in)), {}), nullptr, false);
    std::vector<AddonFile> result;
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("files") || !parsed["files"].is_array()) {
        notes.push_back("The Modworks record in the addons folder is damaged, so every mod there is treated as installed by something else.");
        return result;
    }
    for (const auto& item : parsed["files"]) {
        try {
            AddonFile entry{item.at("slot").get<int>(), item.at("content_id").get<std::string>(), item.value("name", std::string()),
                item.at("size").get<uint64_t>(), item.at("stamp").get<int64_t>()};
            if (entry.Slot >= kFirstAddonSlot && entry.Slot <= kLastAddonSlot) result.push_back(std::move(entry));
        } catch (const json::exception&) {}
    }
    return result;
}

void WriteManifest(const fs::path& addons, std::vector<AddonFile> files) {
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.Slot < b.Slot; });
    json list = json::array();
    for (const auto& file : files)
        list.push_back({{"slot", file.Slot}, {"content_id", file.ContentId}, {"name", file.Name}, {"size", file.Size}, {"stamp", file.Stamp}});
    std::error_code error;
    if (files.empty() && !fs::exists(addons / kAddonsManifestName, error)) return;
    WriteReplacing(addons / kAddonsManifestName, json{{"version", 1}, {"files", list}}.dump(2));
}

AddonFile Record(const fs::path& addons, int slot, const DeployMod& mod) {
    const auto path = addons / Widen(AddonSlotName(slot));
    return {slot, mod.ContentId, mod.Name, fs::file_size(path), Stamp(path)};
}

struct CopyState {
    CancellationToken* Cancellation;
    const DeployProgress* Progress;
    uint64_t Done;
    uint64_t Total;
};

DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID data) {
    auto& state = *static_cast<CopyState*>(data);
    if (state.Cancellation->IsCancelled()) return PROGRESS_CANCEL;
    if (*state.Progress) (*state.Progress)(state.Done + static_cast<uint64_t>(transferred.QuadPart), state.Total);
    return PROGRESS_CONTINUE;
}

// Tokens of one gameinfo.gi line: comments dropped, quotes removed.
std::vector<std::string> Tokens(std::string_view line) {
    if (const auto comment = line.find("//"); comment != std::string_view::npos) line = line.substr(0, comment);
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    for (const char c : line) {
        if (c == '"') { quoted = !quoted; continue; }
        if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) tokens.push_back(std::move(current)), current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

std::vector<std::string_view> Lines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start + 1));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return lines;
}

// The line range strictly inside the SearchPaths block, as [first, last).
std::optional<std::pair<size_t, size_t>> SearchPathsBlock(const std::vector<std::string_view>& lines) {
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto tokens = Tokens(lines[i]);
        if (tokens.empty() || !EqualsIgnoreCase(tokens[0], "SearchPaths")) continue;
        size_t open = i;
        if (tokens.size() < 2 || tokens[1] != "{") {
            open = i + 1;
            while (open < lines.size() && Tokens(lines[open]).empty()) ++open;
            if (open >= lines.size() || Tokens(lines[open]) != std::vector<std::string>{"{"}) return std::nullopt;
        }
        int depth = 1;
        for (size_t j = open + 1; j < lines.size(); ++j) {
            for (const auto& token : Tokens(lines[j])) {
                if (token == "{") ++depth;
                if (token == "}") --depth;
            }
            if (depth == 0) return std::pair{open + 1, j};
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool IsAddonsLine(const std::vector<std::string>& tokens) {
    if (tokens.size() < 2 || !EqualsIgnoreCase(tokens[0], "Game")) return false;
    auto path = tokens[1];
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.back() == '/') path.pop_back();
    return EqualsIgnoreCase(path, "citadel/addons");
}
}  // namespace

std::string AddonSlotName(int slot) {
    char name[32];
    std::snprintf(name, sizeof(name), "pak%02d_dir.vpk", slot);
    return name;
}

std::optional<int> AddonSlotOf(std::string_view file_name) {
    const auto name = Lower(std::string(file_name));
    // pakNN_dir.vpk or pakNN_DDD.vpk.
    if (name.size() != 13 || !name.starts_with("pak") || !name.ends_with(".vpk") || name[5] != '_') return std::nullopt;
    if (!std::isdigit(static_cast<unsigned char>(name[3])) || !std::isdigit(static_cast<unsigned char>(name[4]))) return std::nullopt;
    const auto tail = name.substr(6, 3);
    if (tail != "dir" && !std::all_of(tail.begin(), tail.end(), [](unsigned char c) { return std::isdigit(c); })) return std::nullopt;
    const int slot = (name[3] - '0') * 10 + (name[4] - '0');
    if (slot < kFirstAddonSlot || slot > kLastAddonSlot) return std::nullopt;
    return slot;
}

AddonsScan ScanAddons(const fs::path& addons) {
    AddonsScan scan;
    std::error_code error;
    if (!fs::is_directory(addons, error)) return scan;
    const auto recorded = ReadManifest(addons, scan.Notes);
    std::map<int, std::vector<fs::path>> by_slot;
    for (const auto& entry : fs::directory_iterator(addons)) {
        if (!entry.is_regular_file(error)) continue;
        const auto name = entry.path().filename().wstring();
        // A temporary copy left by a deploy that was interrupted.
        if (name.starts_with(kTempPrefix) && name.ends_with(L".tmp")) { fs::remove(entry.path(), error); continue; }
        if (const auto slot = AddonSlotOf(Narrow(name))) by_slot[*slot].push_back(entry.path());
    }
    for (const auto& file : recorded) {
        const auto found = by_slot.find(file.Slot);
        if (found == by_slot.end()) continue;  // Removed by hand. Nothing to manage there any more.
        const auto path = addons / Widen(AddonSlotName(file.Slot));
        const bool alone = found->second.size() == 1 && EqualsIgnoreCase(Narrow(found->second.front().filename().wstring()), AddonSlotName(file.Slot));
        if (alone && fs::file_size(path, error) == file.Size && !error && Stamp(path) == file.Stamp) {
            scan.Managed.push_back(file);
            by_slot.erase(found);
        } else {
            scan.Notes.push_back(AddonSlotName(file.Slot) + " changed since Modworks installed " + file.Name + ", so it is left alone.");
        }
    }
    for (const auto& [slot, files] : by_slot) {
        scan.Foreign.insert(slot);
        for (const auto& file : files) scan.ForeignFiles.push_back(Narrow(file.filename().wstring()));
    }
    return scan;
}

std::vector<int> AssignAddonSlots(const std::set<int>& foreign, size_t count) {
    std::vector<int> slots;
    for (int slot = kFirstAddonSlot; slot <= kLastAddonSlot && slots.size() < count; ++slot)
        if (!foreign.contains(slot)) slots.push_back(slot);
    if (slots.size() < count)
        throw InvalidOperation("Deadlock loads at most " + std::to_string(kLastAddonSlot) + " addon files, and " + std::to_string(foreign.size())
            + " are used by mods Modworks did not install. Disable some mods.");
    return slots;
}

DeployResult DeployAddons(const fs::path& addons, const std::vector<DeployMod>& enabled, CancellationToken cancellation,
    const DeployProgress& progress) {
    for (size_t i = 0; i < enabled.size(); ++i)
        for (size_t j = i + 1; j < enabled.size(); ++j)
            if (enabled[i].ContentId == enabled[j].ContentId) throw InvalidOperation(enabled[i].Name + " is listed twice.");
    fs::create_directories(addons);
    auto scan = ScanAddons(addons);
    const auto slots = AssignAddonSlots(scan.Foreign, enabled.size());
    DeployResult result;
    result.Notes = scan.Notes;

    // What stays in place, what moves from another slot and what is copied from the library.
    std::vector<AddonFile> record = scan.Managed;
    std::vector<bool> used(record.size(), false);
    struct Move { size_t From; int Slot; const DeployMod* Mod; };
    struct Copy { int Slot; const DeployMod* Mod; };
    std::vector<Move> moves;
    std::vector<Copy> copies;
    std::vector<AddonFile> kept;
    const auto find = [&](const std::string& content, std::optional<int> slot) -> std::optional<size_t> {
        for (size_t m = 0; m < record.size(); ++m)
            if (!used[m] && record[m].ContentId == content && (!slot || record[m].Slot == *slot)) return m;
        return std::nullopt;
    };
    for (size_t i = 0; i < enabled.size(); ++i) {
        if (const auto same = find(enabled[i].ContentId, slots[i])) {
            used[*same] = true;
            kept.push_back(record[*same]);
        } else if (const auto other = find(enabled[i].ContentId, std::nullopt)) {
            used[*other] = true;
            moves.push_back({*other, slots[i], &enabled[i]});
        } else {
            copies.push_back({slots[i], &enabled[i]});
        }
    }
    uint64_t total = 0;
    for (const auto& copy : copies) total += fs::file_size(copy.Mod->Source);

    std::vector<AddonFile> done = kept;
    std::vector<std::pair<fs::path, int>> staged;  // Temporary file and the slot it is for.
    std::vector<const DeployMod*> staged_mods;
    int counter = 0;
    const auto temp_name = [&] { return addons / (std::wstring(kTempPrefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter) + L".tmp"); };
    // Modworks files that are neither kept nor moved yet. They stay recorded until they are gone.
    std::vector<AddonFile> pending;
    for (size_t m = 0; m < record.size(); ++m) if (!used[m] || std::any_of(moves.begin(), moves.end(), [&](const Move& mv) { return mv.From == m; })) pending.push_back(record[m]);
    const auto drop_pending = [&](int slot) {
        pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const AddonFile& f) { return f.Slot == slot; }), pending.end());
    };
    try {
        for (size_t m = 0; m < record.size(); ++m) {
            if (used[m]) continue;
            cancellation.ThrowIfCancelled();
            fs::remove(addons / Widen(AddonSlotName(record[m].Slot)));
            drop_pending(record[m].Slot);
            ++result.Removed;
        }
        for (const auto& move : moves) {
            cancellation.ThrowIfCancelled();
            const auto temp = temp_name();
            fs::rename(addons / Widen(AddonSlotName(record[move.From].Slot)), temp);
            drop_pending(record[move.From].Slot);
            staged.emplace_back(temp, move.Slot);
            staged_mods.push_back(move.Mod);
            ++result.Moved;
        }
        uint64_t copied = 0;
        for (const auto& copy : copies) {
            cancellation.ThrowIfCancelled();
            const auto temp = temp_name();
            CopyState state{&cancellation, &progress, copied, total};
            BOOL cancel = FALSE;
            if (!CopyFileExW(copy.Mod->Source.c_str(), temp.c_str(), CopyProgress, &state, &cancel, COPY_FILE_FAIL_IF_EXISTS)) {
                const auto code = GetLastError();
                std::error_code error;
                fs::remove(temp, error);
                if (code == ERROR_REQUEST_ABORTED) throw OperationCancelled();
                throw IoError("Could not copy " + copy.Mod->Name + " into the addons folder. Windows error " + std::to_string(code) + ".");
            }
            copied += fs::file_size(temp);
            staged.emplace_back(temp, copy.Slot);
            staged_mods.push_back(copy.Mod);
            ++result.Copied;
        }
        for (size_t i = 0; i < staged.size(); ++i) {
            const auto target = addons / Widen(AddonSlotName(staged[i].second));
            if (!MoveFileExW(staged[i].first.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
                throw IoError("Could not place " + staged_mods[i]->Name + " as " + AddonSlotName(staged[i].second) + ". Windows error "
                    + std::to_string(GetLastError()) + ".");
            done.push_back(Record(addons, staged[i].second, *staged_mods[i]));
        }
        staged.clear();
    } catch (...) {
        std::error_code error;
        for (const auto& [temp, slot] : staged) fs::remove(temp, error);
        auto partial = done;
        for (const auto& file : pending) if (fs::exists(addons / Widen(AddonSlotName(file.Slot)), error)) partial.push_back(file);
        try { WriteManifest(addons, partial); } catch (const std::exception&) {}
        throw;
    }
    WriteManifest(addons, done);
    if (progress && total) progress(total, total);
    result.Kept = kept.size();
    std::sort(done.begin(), done.end(), [](const auto& a, const auto& b) { return a.Slot < b.Slot; });
    result.Deployed = std::move(done);
    return result;
}

bool IsDeadlockRunning() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !running; more = Process32NextW(snapshot, &entry))
        running = EqualsIgnoreCase(Narrow(entry.szExeFile), "deadlock.exe");
    CloseHandle(snapshot);
    return running;
}

std::vector<ModConflict> FindModConflicts(const std::vector<DeployMod>& enabled, CancellationToken cancellation) {
    std::map<std::string, size_t> owner;
    std::map<std::pair<size_t, size_t>, ModConflict> pairs;
    for (size_t i = 0; i < enabled.size(); ++i) {
        cancellation.ThrowIfCancelled();
        std::vector<std::string> paths;
        try {
            const auto archive = VpkArchive::Open(enabled[i].Source);
            for (const auto& entry : archive.Entries()) paths.push_back(Lower(entry.FullPath));
        } catch (const OperationCancelled&) { throw; }
        catch (const std::exception&) { continue; }
        for (const auto& path : paths) {
            const auto [found, inserted] = owner.emplace(path, i);
            if (inserted || found->second == i) continue;
            auto& conflict = pairs[{found->second, i}];
            conflict.Winner = enabled[found->second].ContentId;
            conflict.Loser = enabled[i].ContentId;
            ++conflict.Files;
            if (conflict.Examples.size() < 5) conflict.Examples.push_back(path);
        }
    }
    std::vector<ModConflict> result;
    for (auto& [key, conflict] : pairs) result.push_back(std::move(conflict));
    return result;
}

bool GameInfoMountsAddons(std::string_view gameinfo) {
    const auto lines = Lines(gameinfo);
    const auto block = SearchPathsBlock(lines);
    if (!block) return false;
    for (size_t i = block->first; i < block->second; ++i) if (IsAddonsLine(Tokens(lines[i]))) return true;
    return false;
}

std::string AddAddonsToGameInfo(std::string_view gameinfo) {
    if (GameInfoMountsAddons(gameinfo)) return std::string(gameinfo);
    const auto lines = Lines(gameinfo);
    const auto block = SearchPathsBlock(lines);
    if (!block) throw InvalidData("gameinfo.gi has no SearchPaths section Modworks recognizes.");
    const std::string newline = gameinfo.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    size_t insert = block->first;
    std::string indent = "\t\t\t";
    for (size_t i = block->first; i < block->second; ++i) {
        const auto tokens = Tokens(lines[i]);
        if (tokens.size() >= 2 && EqualsIgnoreCase(tokens[0], "Game") && EqualsIgnoreCase(tokens[1], "citadel")) {
            insert = i;
            indent = std::string(lines[i].substr(0, lines[i].find_first_not_of(" \t")));
            break;
        }
    }
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i == insert) result += indent + "Game\t\t\t\tcitadel/addons" + newline;
        result += lines[i];
    }
    return result;
}

std::optional<fs::path> CitadelFolder(const fs::path& game) {
    if (game.empty()) return std::nullopt;
    std::error_code error;
    fs::path folder = fs::is_directory(game, error) ? game : game.parent_path();
    for (const auto& candidate : {folder, folder / L"citadel", folder / L"game" / L"citadel"})
        if (fs::is_regular_file(candidate / L"gameinfo.gi", error)) return candidate;
    return std::nullopt;
}

void PatchGameInfo(const fs::path& citadel) {
    const auto file = citadel / L"gameinfo.gi";
    std::ifstream in(file, std::ios::binary);
    if (!in) throw IoError("Could not read " + Narrow(file.wstring()) + ".");
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    in.close();
    if (GameInfoMountsAddons(text)) return;
    const auto patched = AddAddonsToGameInfo(text);
    const auto backup = citadel / L"gameinfo.gi.modworks.bak";
    std::error_code error;
    if (!fs::exists(backup, error)) fs::copy_file(file, backup);
    WriteReplacing(file, patched);
}

}  // namespace skeleton_repair

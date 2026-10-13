#include "mod_state.hpp"

#include "error.hpp"
#include "strings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <set>

namespace skeleton_repair {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

constexpr int kVersion = 1;
constexpr const char* kDefaultProfile = "Default";

bool Contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}
void Erase(std::vector<std::string>& list, const std::string& value) {
    list.erase(std::remove(list.begin(), list.end(), value), list.end());
}
std::string Trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::vector<std::string> Strings(const json& value) {
    std::vector<std::string> result;
    if (!value.is_array()) throw InvalidData("The mod list is damaged.");
    for (const auto& item : value) {
        if (!item.is_string()) throw InvalidData("The mod list is damaged.");
        if (!Contains(result, item.get<std::string>())) result.push_back(item.get<std::string>());
    }
    return result;
}
}  // namespace

ModState::ModState() : profiles_{{kDefaultProfile, {}}}, active_(kDefaultProfile) {}

ModState ModState::Load(const fs::path& file) {
    std::error_code error;
    if (!fs::exists(file, error)) return {};
    std::ifstream in(file, std::ios::binary);
    if (!in) throw IoError("Could not read " + Narrow(file.wstring()) + ".");
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    const auto parsed = json::parse(text, nullptr, false);
    const std::string damaged = "The mod list " + Narrow(file.wstring()) + " is damaged. It was left as it is.";
    if (parsed.is_discarded() || !parsed.is_object()) throw InvalidData(damaged);
    if (parsed.value("version", 0) > kVersion) throw InvalidData("The mod list " + Narrow(file.wstring()) + " was written by a newer Modworks.");
    ModState state;
    try {
        state.order_ = Strings(parsed.value("order", json::array()));
        state.profiles_.clear();
        for (const auto& item : parsed.value("profiles", json::array())) {
            ModProfile profile{Trimmed(item.at("name").get<std::string>()), Strings(item.value("enabled", json::array()))};
            if (profile.Name.empty() || std::any_of(state.profiles_.begin(), state.profiles_.end(), [&](const auto& p) { return p.Name == profile.Name; }))
                continue;
            for (const auto& id : profile.Enabled) if (!Contains(state.order_, id)) state.order_.push_back(id);
            state.profiles_.push_back(std::move(profile));
        }
        if (state.profiles_.empty()) state.profiles_.push_back({kDefaultProfile, {}});
        state.active_ = parsed.value("active", std::string());
        if (std::none_of(state.profiles_.begin(), state.profiles_.end(), [&](const auto& p) { return p.Name == state.active_; }))
            state.active_ = state.profiles_.front().Name;
        const auto origins = parsed.value("origins", json::object());
        for (const auto& [id, item] : origins.items()) {
            ModOrigin origin;
            origin.GameBananaMod = item.value("gamebanana_mod", int64_t{0});
            origin.GameBananaFile = item.value("gamebanana_file", int64_t{0});
            origin.FileName = item.value("file_name", std::string());
            origin.PageUrl = item.value("page_url", std::string());
            origin.DateModified = item.value("date_modified", int64_t{0});
            origin.Built = item.value("built", false);
            state.origins_[id] = std::move(origin);
        }
    } catch (const json::exception&) {
        throw InvalidData(damaged);
    }
    return state;
}

void ModState::Save(const fs::path& file) const {
    json profiles = json::array();
    for (const auto& profile : profiles_) profiles.push_back({{"name", profile.Name}, {"enabled", profile.Enabled}});
    json origins = json::object();
    for (const auto& [id, origin] : origins_) {
        origins[id] = {{"gamebanana_mod", origin.GameBananaMod}, {"gamebanana_file", origin.GameBananaFile},
            {"file_name", origin.FileName}, {"page_url", origin.PageUrl}, {"date_modified", origin.DateModified}, {"built", origin.Built}};
    }
    const json document = {{"version", kVersion}, {"order", order_}, {"active", active_}, {"profiles", profiles}, {"origins", origins}};
    fs::create_directories(file.parent_path());
    auto temp = file;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << document.dump(2);
        out.flush();
        if (!out) throw IoError("Could not save the mod list to " + Narrow(file.wstring()) + ".");
    }
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError();
        std::error_code error;
        fs::remove(temp, error);
        throw IoError("Could not save the mod list to " + Narrow(file.wstring()) + ". Windows error " + std::to_string(code) + ".");
    }
}

void ModState::Sync(const std::vector<std::string>& present) {
    for (const auto& id : present) if (!id.empty() && !Contains(order_, id)) order_.push_back(id);
}

std::vector<std::string> ModState::Ordered(const std::vector<std::string>& present) const {
    const std::set<std::string> wanted(present.begin(), present.end());
    std::vector<std::string> result;
    for (const auto& id : order_) if (wanted.contains(id)) result.push_back(id);
    return result;
}

std::vector<std::string> ModState::EnabledInOrder(const std::vector<std::string>& present) const {
    auto result = Ordered(present);
    const auto& enabled = Active().Enabled;
    std::erase_if(result, [&](const auto& id) { return !Contains(enabled, id); });
    return result;
}

bool ModState::Enabled(const std::string& id) const { return Contains(Active().Enabled, id); }

void ModState::SetEnabled(const std::string& id, bool enabled) {
    if (id.empty()) return;
    if (!Contains(order_, id)) order_.push_back(id);
    auto& list = Active().Enabled;
    if (enabled && !Contains(list, id)) list.push_back(id);
    if (!enabled) Erase(list, id);
}

void ModState::MoveBefore(const std::string& id, const std::string& before) {
    if (id == before || !Contains(order_, id)) return;
    Erase(order_, id);
    const auto target = before.empty() ? order_.end() : std::find(order_.begin(), order_.end(), before);
    order_.insert(target, id);
}

void ModState::Step(const std::string& id, bool up, const std::vector<std::string>& present) {
    const auto visible = Ordered(present);
    const auto at = std::find(visible.begin(), visible.end(), id);
    if (at == visible.end()) return;
    if (up) {
        if (at != visible.begin()) MoveBefore(id, *(at - 1));
    } else if (at + 1 != visible.end()) {
        // Moving down past the next mod is moving that mod up past this one.
        MoveBefore(*(at + 1), id);
    }
}

void ModState::Forget(const std::string& id) {
    Erase(order_, id);
    for (auto& profile : profiles_) Erase(profile.Enabled, id);
    origins_.erase(id);
}

void ModState::Replace(const std::string& id, const std::string& replacement) {
    if (id == replacement || replacement.empty()) return;
    const auto at = std::find(order_.begin(), order_.end(), id);
    if (at == order_.end()) { Sync({replacement}); return; }
    Erase(order_, replacement);
    *std::find(order_.begin(), order_.end(), id) = replacement;
    for (auto& profile : profiles_) {
        const auto enabled = std::find(profile.Enabled.begin(), profile.Enabled.end(), id);
        if (enabled == profile.Enabled.end()) continue;
        Erase(profile.Enabled, replacement);
        *std::find(profile.Enabled.begin(), profile.Enabled.end(), id) = replacement;
    }
    origins_.erase(id);
}

void ModState::SwitchProfile(const std::string& name) {
    if (std::none_of(profiles_.begin(), profiles_.end(), [&](const auto& p) { return p.Name == name; }))
        throw InvalidOperation("There is no profile named " + name + ".");
    active_ = name;
}

void ModState::CreateProfile(const std::string& raw, bool copy_enabled) {
    const auto name = Trimmed(raw);
    if (name.empty()) throw InvalidOperation("Give the profile a name.");
    if (std::any_of(profiles_.begin(), profiles_.end(), [&](const auto& p) { return EqualsIgnoreCase(p.Name, name); }))
        throw InvalidOperation("A profile named " + name + " already exists.");
    profiles_.push_back({name, copy_enabled ? Active().Enabled : std::vector<std::string>{}});
    active_ = name;
}

void ModState::RenameProfile(const std::string& from, const std::string& raw) {
    const auto to = Trimmed(raw);
    if (to.empty()) throw InvalidOperation("Give the profile a name.");
    const auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& p) { return p.Name == from; });
    if (found == profiles_.end()) throw InvalidOperation("There is no profile named " + from + ".");
    if (std::any_of(profiles_.begin(), profiles_.end(), [&](const auto& p) { return &p != &*found && EqualsIgnoreCase(p.Name, to); }))
        throw InvalidOperation("A profile named " + to + " already exists.");
    if (active_ == from) active_ = to;
    found->Name = to;
}

void ModState::DeleteProfile(const std::string& name) {
    if (profiles_.size() <= 1) throw InvalidOperation("Keep at least one profile.");
    const auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& p) { return p.Name == name; });
    if (found == profiles_.end()) return;
    profiles_.erase(found);
    if (active_ == name) active_ = profiles_.front().Name;
}

std::optional<ModOrigin> ModState::Origin(const std::string& id) const {
    const auto found = origins_.find(id);
    if (found == origins_.end()) return std::nullopt;
    return found->second;
}

void ModState::SetOrigin(const std::string& id, const ModOrigin& origin) {
    if (!id.empty()) origins_[id] = origin;
}

std::vector<std::string> ModState::FromGameBanana(int64_t mod) const {
    std::vector<std::string> result;
    for (const auto& [id, origin] : origins_) if (origin.GameBananaMod == mod) result.push_back(id);
    return result;
}

ModProfile& ModState::Active() {
    return *std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& p) { return p.Name == active_; });
}
const ModProfile& ModState::Active() const {
    return *std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& p) { return p.Name == active_; });
}

}  // namespace skeleton_repair

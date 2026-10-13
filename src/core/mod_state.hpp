#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace skeleton_repair {

// Where a stored mod was downloaded from. Mods added from local files have none.
struct ModOrigin {
    int64_t GameBananaMod = 0;
    int64_t GameBananaFile = 0;
    std::string FileName;
    std::string PageUrl;
    // GameBanana's modification time of the mod when it was downloaded, in Unix seconds.
    int64_t DateModified = 0;
    // Built by the VPK builder as a named mod. Building again under the same name replaces it.
    bool Built = false;
    bool operator==(const ModOrigin&) const = default;
};

// A named set of enabled mods. Every profile shares one load order.
struct ModProfile {
    std::string Name;
    std::vector<std::string> Enabled;
    bool operator==(const ModProfile&) const = default;
};

// Which stored mods are enabled, their load order and named profiles, saved as mods.json in the data root.
// Mods are identified by their content id (LocalModStore::ContentId), which does not change between launches.
// The first mod in the load order has the highest priority: its files win over those of later mods.
class ModState {
public:
    ModState();
    // A missing file gives the default state with one profile, "Default". InvalidData when the file is damaged.
    static ModState Load(const std::filesystem::path& file);
    // Writes a temporary file and replaces `file`, so an interrupted save leaves the previous state.
    void Save(const std::filesystem::path& file) const;

    // Adds the ids not known yet to the end of the load order. Known ids that are not present keep their place.
    void Sync(const std::vector<std::string>& present);
    // The load order restricted to `present`, highest priority first.
    std::vector<std::string> Ordered(const std::vector<std::string>& present) const;
    // The enabled mods among `present`, highest priority first.
    std::vector<std::string> EnabledInOrder(const std::vector<std::string>& present) const;
    const std::vector<std::string>& Order() const { return order_; }

    bool Enabled(const std::string& id) const;
    void SetEnabled(const std::string& id, bool enabled);
    // Moves `id` so it sits directly before `before`, or to the end when `before` is empty.
    void MoveBefore(const std::string& id, const std::string& before);
    // Moves `id` one place up (toward higher priority) or down among `present`.
    void Step(const std::string& id, bool up, const std::vector<std::string>& present);
    // Removes a mod that left the library from the order, every profile and the origins.
    void Forget(const std::string& id);
    // Puts `replacement` where `id` was: its place in the load order and its enabled state in every profile.
    // `id` is then forgotten. A replacement that was already listed moves to that place.
    void Replace(const std::string& id, const std::string& replacement);

    const std::vector<ModProfile>& Profiles() const { return profiles_; }
    const std::string& ActiveProfile() const { return active_; }
    void SwitchProfile(const std::string& name);
    // InvalidOperation for an empty or duplicate name. The new profile starts with the active profile's mods
    // when `copy_enabled` is set, and with none otherwise. It becomes the active profile.
    void CreateProfile(const std::string& name, bool copy_enabled);
    void RenameProfile(const std::string& from, const std::string& to);
    // The last profile cannot be deleted. Deleting the active one switches to the first remaining one.
    void DeleteProfile(const std::string& name);

    std::optional<ModOrigin> Origin(const std::string& id) const;
    void SetOrigin(const std::string& id, const ModOrigin& origin);
    // The stored mods downloaded from a GameBanana mod.
    std::vector<std::string> FromGameBanana(int64_t mod) const;

    bool operator==(const ModState&) const = default;

private:
    ModProfile& Active();
    const ModProfile& Active() const;
    std::vector<std::string> order_;
    std::vector<ModProfile> profiles_;
    std::string active_;
    std::map<std::string, ModOrigin> origins_;
};

}  // namespace skeleton_repair

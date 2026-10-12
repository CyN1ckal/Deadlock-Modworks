#include "model_library.hpp"
#include "archive_transaction.hpp"
#include "resource.hpp"
#include "kv3.hpp"
#include "strings.hpp"
#include "error.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <set>
#include <algorithm>

namespace skeleton_repair {
namespace {
struct Less { bool operator()(const std::string& a, const std::string& b) const { return CompareIgnoreCase(a, b) < 0; } };
using Index = std::map<std::string, const VpkEntry*, Less>;
Index Entries(const VpkArchive& archive) {
    Index result;
    for (const auto& entry : archive.Entries()) {
        if (!result.emplace(VpkArchive::Normalize(entry.FullPath), &entry).second)
            throw InvalidData("Archive contains duplicate resource paths: " + entry.FullPath);
    }
    return result;
}
std::string Compiled(std::string path) {
    path = VpkArchive::Normalize(std::move(path));
    if (path.size() < 2 || !EqualsIgnoreCase(path.substr(path.size() - 2), "_c")) path += "_c";
    return path;
}
void KvReferences(const KvValue& value, std::vector<std::string>& names) {
    if (value.Kind == KvKind::String && (value.Flag == KvFlag::Resource || value.Flag == KvFlag::ResourceName)) {
        if (!value.String().empty()) names.push_back(value.String());
    } else if (value.IsArray()) {
        for (const auto& child : value.Items()) KvReferences(child, names);
    } else if (value.IsObject()) {
        for (const auto& [key, child] : value.Fields()) {
            // Some model skeleton/animation references are plain strings.
            if (key == "m_vecNmSkeletonRefs" || key == "m_refAnimIncludeModels" || key == "m_refAnimGroups" || key == "m_refPhysicsData") {
                if (child.IsArray()) for (const auto& item : child.Items())
                    if (item.Kind == KvKind::String && !item.String().empty()) names.push_back(item.String());
            }
            KvReferences(child, names);
        }
    }
}
std::vector<std::string> References(const std::vector<uint8_t>& bytes) {
    auto names = ExternalReferenceNames(bytes);
    // DATA can be a non-KV3 format (textures, meshes, animation data).
    const auto data = FindDataBlock(bytes);
    if (data.size() >= 4) {
        const uint32_t magic = uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
        if (IsBinaryKv3(magic)) KvReferences(ReadKv3(data).Root, names);
    }
    return names;
}
std::filesystem::path Absolute(const std::filesystem::path& path) { return std::filesystem::absolute(path).lexically_normal(); }
}

void ModelLibrary::Save(const std::filesystem::path& path) const {
    if (!EqualsIgnoreCase(Narrow(path.extension().wstring()), ".json")) throw InvalidData("Save the model library as a .json file.");
    nlohmann::json json = {{"schemaVersion", 1}, {"sources", nlohmann::json::array()}, {"selected", nlohmann::json::array()}};
    for (const auto& source : Sources) json["sources"].push_back(Narrow(Absolute(source).wstring()));
    for (const auto& model : Selected) json["selected"].push_back({{"archive", Narrow(Absolute(model.Archive).wstring())}, {"model", model.Model}});
    if (!GameArchive.empty()) json["gameArchive"] = Narrow(Absolute(GameArchive).wstring());
    std::ofstream out(path, std::ios::binary);
    out << json.dump(2) << '\n';
    out.flush();
    if (!out) throw IoError("Could not save model library.");
}
ModelLibrary ModelLibrary::Load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw IoError("Could not open model library.");
    nlohmann::json json; in >> json;
    if (json.at("schemaVersion").get<int>() != 1) throw InvalidData("Unsupported model library version.");
    ModelLibrary result;
    const auto resolve = [&](const std::string& name) {
        auto p = std::filesystem::path(Widen(name));
        return Absolute(p.is_absolute() ? p : path.parent_path() / p);
    };
    for (const auto& source : json.at("sources")) result.Sources.push_back(resolve(source.get<std::string>()));
    if (json.contains("gameArchive")) result.GameArchive = resolve(json.at("gameArchive").get<std::string>());
    for (const auto& item : json.at("selected")) {
        LibraryModel model{resolve(item.at("archive").get<std::string>()), VpkArchive::Normalize(item.at("model").get<std::string>())};
        if (std::none_of(result.Sources.begin(), result.Sources.end(), [&](const auto& source) { return EqualsIgnoreCase(Narrow(source.wstring()), Narrow(model.Archive.wstring())); }))
            throw InvalidData("Selected model is not in the library sources.");
        result.Selected.push_back(std::move(model));
    }
    return result;
}

size_t ExportModelLibrary(const std::vector<LibraryModel>& models, const std::filesystem::path& game_archive,
    const std::filesystem::path& destination, CancellationToken cancellation) {
    if (models.empty()) throw InvalidData("Select at least one model.");
    if (!EqualsIgnoreCase(Narrow(destination.extension().wstring()), ".vpk")) throw InvalidData("Output must be a .vpk file.");
    std::optional<VpkArchive> game;
    std::optional<ReadLease> game_lease;
    Index game_index;
    if (!game_archive.empty()) {
        game.emplace(VpkArchive::Open(game_archive));
        game_lease.emplace(game->LockSources());
        game_index = Entries(*game);
    }
    struct Output { std::vector<uint8_t> Bytes; std::string Source; };
    std::map<std::string, Output, Less> output;
    std::vector<VpkArchive> archives;
    std::vector<ReadLease> leases;
    std::map<std::string, size_t, Less> archive_ids;
    for (const auto& model : models) {
        cancellation.ThrowIfCancelled();
        const auto path = Absolute(model.Archive);
        const auto name = Narrow(path.wstring());
        if (EqualsIgnoreCase(name, Narrow(Absolute(destination).wstring()))) throw InvalidData("Choose an output distinct from every source archive.");
        if (!archive_ids.contains(name)) {
            archive_ids[name] = archives.size();
            archives.push_back(VpkArchive::Open(path));
            leases.push_back(archives.back().LockSources());
        }
    }
    for (const auto& model : models) {
        const auto source_name = Narrow(Absolute(model.Archive).wstring());
        const auto& source = archives.at(archive_ids.at(source_name));
        const auto index = Entries(source);
        const auto root = VpkArchive::Normalize(model.Model);
        if (!root.ends_with(".vmdl_c") || !index.contains(root)) throw InvalidData("Selected model is missing: " + source_name + " / " + root);
        std::vector<std::string> queue{root};
        std::set<std::string, Less> visited;
        for (size_t i = 0; i < queue.size(); ++i) {
            cancellation.ThrowIfCancelled();
            const auto resource = queue[i];
            if (!visited.insert(resource).second) continue;
            const auto mod_entry = index.find(resource);
            const auto game_entry = game_index.find(resource);
            if (mod_entry == index.end() && game_entry == game_index.end())
                throw InvalidData("Missing dependency " + resource + " required by " + root + " in " + source_name + ". Connect the installed game or use a complete source VPK.");
            const auto bytes = mod_entry != index.end() ? source.Read(*mod_entry->second, cancellation) : game->Read(*game_entry->second, cancellation);
            if (mod_entry != index.end()) {
                const auto found = output.find(resource);
                if (found != output.end() && found->second.Bytes != bytes)
                    throw InvalidData("Conflicting file " + resource + "\n" + found->second.Source + "\n" + source_name + "\nDeselect one of the models using this file.");
                if (found == output.end()) output.emplace(resource, Output{bytes, source_name});
            }
            if (resource.ends_with("_c")) {
                try {
                    for (const auto& reference : References(bytes)) if (!reference.empty()) queue.push_back(Compiled(reference));
                } catch (const InvalidData& error) {
                    throw InvalidData("Cannot inspect dependencies of " + resource + " for " + root + " in " + source_name + ": " + error.what());
                }
            }
        }
    }
    // Remove unselected files from the seed archive; every retained file is explicit.
    std::vector<VpkChange> changes;
    for (const auto& entry : archives.front().Entries()) if (!output.contains(entry.FullPath)) changes.push_back({entry.FullPath, std::nullopt});
    for (auto& [path, file] : output) changes.push_back({path, std::move(file.Bytes)});
    ArchiveTransaction::Write(archives.front(), models.front().Archive, destination, changes, false, cancellation);
    return output.size();
}
}

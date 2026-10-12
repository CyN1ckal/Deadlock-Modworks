#include "model.hpp"

#include "error.hpp"
#include "kv3.hpp"
#include "resource.hpp"
#include "skeleton.hpp"
#include "strings.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace skeleton_repair {
namespace {

bool IsBlank(std::string_view text) {
    for (unsigned char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v') {
            return false;
        }
    }
    return true;
}

std::string Join(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += parts[i];
    }
    return out;
}

MigrationEdit WithOutcome(ModelOutcome outcome, std::string detail, std::vector<std::string> warnings = {}) {
    MigrationEdit edit;
    edit.Outcome = outcome;
    edit.Detail = std::move(detail);
    edit.Warnings = std::move(warnings);
    return edit;
}

uint32_t ReadU32(std::span<const uint8_t> bytes, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

int32_t ReadI32(std::span<const uint8_t> bytes, size_t offset) {
    int32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

bool HasPositiveBlock(std::span<const uint8_t> file, const char type[4]) {
    if (file.size() < 16) {
        return false;
    }
    const auto block_offset = ReadU32(file, 8);
    const auto block_count = ReadI32(file, 12);
    if (block_count <= 0) {
        return false;
    }
    const size_t table = 8u + block_offset;
    if (table > file.size() || static_cast<size_t>(block_count) * 12 > file.size() - table) {
        return false;
    }
    for (int32_t i = 0; i < block_count; ++i) {
        const size_t pos = table + static_cast<size_t>(i) * 12;
        if (std::memcmp(file.data() + pos, type, 4) == 0 && ReadI32(file, pos + 8) > 0) {
            return true;
        }
    }
    return false;
}

const KvValue& RequireArray(const KvValue& skeleton, const char* key, size_t count, const char* who) {
    const KvValue* value = skeleton.Find(key);
    if (value == nullptr) {
        throw InvalidData(std::string(who) + " skeleton is missing " + key + ".");
    }
    if (!value->IsArray() || value->Count() != count) {
        const size_t actual = value->IsArray() ? value->Count() : 0;
        throw InvalidData(std::string(who) + " " + key + " has " + std::to_string(actual) + " items, expected " + std::to_string(count) + ".");
    }
    return *value;
}

Vec3 ReadVec3(const KvValue& value) {
    if (!value.IsArray() || value.Count() != 3) {
        throw InvalidData("Model bone position is not a vector.");
    }
    return Vec3{
        static_cast<float>(value.At(0).ToDouble()),
        static_cast<float>(value.At(1).ToDouble()),
        static_cast<float>(value.At(2).ToDouble()),
    };
}

Quat ReadQuat(const KvValue& value) {
    if (!value.IsArray() || value.Count() != 4) {
        throw InvalidData("Model bone rotation is not a quaternion.");
    }
    return Quat{
        static_cast<float>(value.At(0).ToDouble()),
        static_cast<float>(value.At(1).ToDouble()),
        static_cast<float>(value.At(2).ToDouble()),
        static_cast<float>(value.At(3).ToDouble()),
    };
}

struct ModelBones {
    std::vector<std::string> Names;
    std::vector<int> Parents;
    std::vector<std::optional<std::string>> ParentNames;
    std::vector<int> Flags;
    std::vector<BoneShape> Shapes;
    std::unordered_map<std::string, int> Index;
};

ModelBones ReadBones(const KvValue& root, const char* duplicate_message) {
    const KvValue* skeleton = root.IsObject() ? root.Find("m_modelSkeleton") : nullptr;
    if (skeleton == nullptr || !skeleton->IsObject()) {
        throw InvalidData("Model has no supported embedded skeleton.");
    }
    const KvValue* names = skeleton->Find("m_boneName");
    if (names == nullptr || !names->IsArray()) {
        throw InvalidData("Skeleton has no bone-name array.");
    }

    ModelBones bones;
    const KvValue& parents = RequireArray(*skeleton, "m_nParent", names->Count(), "Model");
    std::unordered_set<std::string> unique;
    for (size_t i = 0; i < names->Count(); ++i) {
        const std::string name = names->At(i).String();
        if (IsBlank(name) || !unique.insert(name).second) {
            throw InvalidData("Skeleton has an empty or duplicate bone name.");
        }
        std::unordered_set<int> visited;
        visited.insert(static_cast<int>(i));
        int parent = parents.At(i).ToInt32();
        while (parent != -1) {
            if (parent < 0 || static_cast<size_t>(parent) >= names->Count()) {
                throw InvalidData("Bone " + name + " has an out-of-range parent index.");
            }
            if (!visited.insert(parent).second) {
                throw InvalidData("Skeleton has a parent cycle at " + name + ".");
            }
            parent = parents.At(static_cast<size_t>(parent)).ToInt32();
        }
    }

    const KvValue& flags = RequireArray(*skeleton, "m_nFlag", names->Count(), "Model");
    const KvValue& spheres = RequireArray(*skeleton, "m_boneSphere", names->Count(), "Model");
    const KvValue& positions = RequireArray(*skeleton, "m_bonePosParent", names->Count(), "Model");
    const KvValue& rotations = RequireArray(*skeleton, "m_boneRotParent", names->Count(), "Model");
    const KvValue& scales = RequireArray(*skeleton, "m_boneScaleParent", names->Count(), "Model");
    (void)spheres;

    if (const KvValue* remap = root.Find("m_remappingTable")) {
        if (!remap->IsArray()) {
            throw InvalidData("Mesh remapping table is not an array.");
        }
        for (size_t i = 0; i < remap->Count(); ++i) {
            const int value = remap->At(i).ToInt32();
            if (value < -1 || value >= static_cast<int>(names->Count())) {
                throw InvalidData("Mesh remapping entry " + std::to_string(i) + " points outside the skeleton.");
            }
        }
    }

    bones.Names.reserve(names->Count());
    for (size_t i = 0; i < names->Count(); ++i) {
        const std::string name = names->At(i).String();
        if (!bones.Index.emplace(name, static_cast<int>(i)).second) {
            throw InvalidData(std::string(duplicate_message) + name + ".");
        }
        bones.Names.push_back(name);
        bones.Parents.push_back(parents.At(i).ToInt32());
        bones.Flags.push_back(flags.At(i).ToInt32());
    }
    for (size_t i = 0; i < bones.Names.size(); ++i) {
        const int parent = bones.Parents[i];
        std::optional<std::string> parent_name;
        if (parent >= 0) {
            parent_name = bones.Names[static_cast<size_t>(parent)];
        }
        bones.ParentNames.push_back(parent_name);
        BoneShape shape;
        shape.Name = bones.Names[i];
        shape.Parent = parent_name;
        shape.Position = ReadVec3(positions.At(i));
        shape.Rotation = ReadQuat(rotations.At(i));
        shape.Scale = static_cast<float>(scales.At(i).ToDouble());
        bones.Shapes.push_back(std::move(shape));
    }
    return bones;
}

KvDocument LoadModel(std::span<const uint8_t> bytes, std::string_view path) {
    const std::span<const uint8_t> data = FindDataBlock(bytes);
    if (data.size() < 4 || !IsBinaryKv3(ReadU32(data, 0))) {
        throw InvalidData(std::string(path) + " is not a compiled model.");
    }
    return ReadKv3(data);
}

const KvValue& SkeletonOf(const KvValue& root) {
    const KvValue* skeleton = root.Find("m_modelSkeleton");
    if (skeleton == nullptr || !skeleton->IsObject()) {
        throw InvalidData("Model is missing m_modelSkeleton.");
    }
    return *skeleton;
}

std::vector<std::string> UnknownSkeletonKeys(const KvValue& skeleton) {
    std::vector<std::string> unknown;
    for (const auto& field : skeleton.Fields()) {
        const std::string& key = field.first;
        if (key != "m_boneName" && key != "m_nParent" && key != "m_boneSphere" && key != "m_nFlag"
            && key != "m_bonePosParent" && key != "m_boneRotParent" && key != "m_boneScaleParent") {
            unknown.push_back(key);
        }
    }
    return unknown;
}

KvValue CopiedArray(const std::vector<KvValue>& values) {
    return KvValue::Array(values);
}

void VerifyRewritten(std::span<const uint8_t> patched, const std::vector<std::string>& new_names,
    const std::vector<int>& mod_only, const ModelBones& mod, const ModelBones& game,
    const std::vector<int>& new_index_of_old, bool had_remap, size_t old_remap_count) {
    const KvDocument checked = LoadModel(patched, "model.vmdl_c");
    const ModelBones bones = ReadBones(checked.Root, "Duplicate bone name ");
    if (bones.Names.size() != new_names.size()) {
        throw InvalidData("Verified bone count is " + std::to_string(bones.Names.size()) + ", expected " + std::to_string(new_names.size()) + ".");
    }
    for (size_t i = 0; i < game.Names.size(); ++i) {
        if (bones.Names[i] != game.Names[i]) {
            throw InvalidData("Bone " + std::to_string(i) + " is " + bones.Names[i] + ", expected " + game.Names[i] + ".");
        }
        if (!mod.Index.contains(game.Names[i]) && bones.ParentNames[i] != game.ParentNames[i]) {
            throw InvalidData("Parent of game bone " + game.Names[i] + " changed.");
        }
    }
    for (size_t i = 0; i < mod_only.size(); ++i) {
        const std::string& expect = mod.Names[static_cast<size_t>(mod_only[i])];
        if (bones.Names[game.Names.size() + i] != expect) {
            throw InvalidData("Mod-only bone " + expect + " was not kept in order.");
        }
    }
    for (size_t i = 0; i < mod.Names.size(); ++i) {
        const std::optional<std::string>& parent = bones.ParentNames[static_cast<size_t>(new_index_of_old[i])];
        if (parent != mod.ParentNames[i]) {
            const std::string from = mod.ParentNames[i].has_value() ? *mod.ParentNames[i] : "(none)";
            const std::string to = parent.has_value() ? *parent : "(none)";
            throw InvalidData("Parent of " + mod.Names[i] + " changed from " + from + " to " + to + ".");
        }
    }
    if (!had_remap) {
        return;
    }
    const KvValue* remap = checked.Root.Find("m_remappingTable");
    if (remap == nullptr) {
        throw InvalidData("Remap table disappeared.");
    }
    if (remap->Count() != old_remap_count) {
        throw InvalidData("Remap table length changed.");
    }
    for (size_t i = 0; i < remap->Count(); ++i) {
        const int value = remap->At(i).ToInt32();
        if (value >= static_cast<int>(bones.Names.size())) {
            throw InvalidData("Remap entry " + std::to_string(i) + " points at bone " + std::to_string(value) + ", past the skeleton.");
        }
    }
}

MigrationEdit Rewrite(KvDocument mod_document, const KvDocument& game_document, std::span<const uint8_t> mod_bytes,
    const ModelBones& mod, const ModelBones& game, const SkeletonAssessment& assessment, std::vector<std::string> warnings) {
    KvValue& mod_root = mod_document.Root;
    const KvValue& game_root = game_document.Root;
    KvValue* mod_skeleton_value = mod_root.Find("m_modelSkeleton");
    if (mod_skeleton_value == nullptr || !mod_skeleton_value->IsObject()) {
        throw InvalidData("Model is missing m_modelSkeleton.");
    }
    KvValue& mod_skeleton = *mod_skeleton_value;
    const KvValue& game_skeleton = SkeletonOf(game_root);

    const KvValue& old_sphere = RequireArray(mod_skeleton, "m_boneSphere", mod.Names.size(), "Mod");
    const KvValue& old_flag = RequireArray(mod_skeleton, "m_nFlag", mod.Names.size(), "Mod");
    const KvValue& old_pos = RequireArray(mod_skeleton, "m_bonePosParent", mod.Names.size(), "Mod");
    const KvValue& old_rot = RequireArray(mod_skeleton, "m_boneRotParent", mod.Names.size(), "Mod");
    const KvValue& old_scale = RequireArray(mod_skeleton, "m_boneScaleParent", mod.Names.size(), "Mod");
    const KvValue& game_sphere = RequireArray(game_skeleton, "m_boneSphere", game.Names.size(), "Game");
    const KvValue& game_flag = RequireArray(game_skeleton, "m_nFlag", game.Names.size(), "Game");
    const KvValue& game_pos = RequireArray(game_skeleton, "m_bonePosParent", game.Names.size(), "Game");
    const KvValue& game_rot = RequireArray(game_skeleton, "m_boneRotParent", game.Names.size(), "Game");
    const KvValue& game_scale = RequireArray(game_skeleton, "m_boneScaleParent", game.Names.size(), "Game");
    const KvValue& game_parents = RequireArray(game_skeleton, "m_nParent", game.Names.size(), "Game");
    RequireArray(mod_skeleton, "m_boneName", mod.Names.size(), "Mod");
    RequireArray(mod_skeleton, "m_nParent", mod.Names.size(), "Mod");

    // Game bones stay at the front, so a game parent index is already the new index.
    // Mod-only helpers are appended and do not shift game bone numbers.
    std::vector<std::string> new_names = game.Names;
    std::vector<int> mod_only;
    for (size_t i = 0; i < mod.Names.size(); ++i) {
        if (game.Index.contains(mod.Names[i])) {
            continue;
        }
        mod_only.push_back(static_cast<int>(i));
        new_names.push_back(mod.Names[i]);
    }

    std::vector<int> new_index_of_old(mod.Names.size(), 0);
    for (size_t i = 0; i < new_names.size(); ++i) {
        const auto found = mod.Index.find(new_names[i]);
        if (found != mod.Index.end()) {
            new_index_of_old[static_cast<size_t>(found->second)] = static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < mod.Names.size(); ++i) {
        if (new_names[static_cast<size_t>(new_index_of_old[i])] != mod.Names[i]) {
            throw InvalidData("Bone " + mod.Names[i] + " was not placed in the new skeleton.");
        }
    }

    auto take = [&](const KvValue& mod_array, const KvValue& game_array, size_t index) {
        const std::string& name = new_names[index];
        const auto mod_bone = mod.Index.find(name);
        if (mod_bone != mod.Index.end()) {
            return mod_array.At(static_cast<size_t>(mod_bone->second));
        }
        return game_array.At(static_cast<size_t>(game.Index.at(name)));
    };

    auto new_parent = [&](size_t index) {
        const std::string& name = new_names[index];
        const auto mod_bone = mod.Index.find(name);
        if (mod_bone != mod.Index.end()) {
            const int parent = mod.Parents[static_cast<size_t>(mod_bone->second)];
            if (parent < 0) {
                return -1;
            }
            if (static_cast<size_t>(parent) >= new_index_of_old.size()) {
                throw InvalidData("Bone " + name + " has parent index " + std::to_string(parent) + " outside the skeleton.");
            }
            return new_index_of_old[static_cast<size_t>(parent)];
        }
        const int game_parent = game_parents.At(static_cast<size_t>(game.Index.at(name))).ToInt32();
        if (game_parent < 0) {
            return -1;
        }
        if (static_cast<size_t>(game_parent) >= game.Names.size()) {
            throw InvalidData("Game bone " + name + " has parent index " + std::to_string(game_parent) + " outside the skeleton.");
        }
        const std::string& parent_name = game.Names[static_cast<size_t>(game_parent)];
        const auto parent_index = game.Index.find(parent_name);
        if (parent_index == game.Index.end()) {
            throw InvalidData("Game bone " + name + " parent " + parent_name + " is missing.");
        }
        return parent_index->second;
    };

    const size_t count = new_names.size();
    std::vector<KvValue> name_values;
    std::vector<KvValue> parent_values;
    std::vector<KvValue> sphere_values;
    std::vector<KvValue> flag_values;
    std::vector<KvValue> pos_values;
    std::vector<KvValue> rot_values;
    std::vector<KvValue> scale_values;
    name_values.reserve(count);
    parent_values.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        name_values.push_back(KvValue::String(new_names[i]));
        parent_values.push_back(KvValue::Int32(new_parent(i)));
        sphere_values.push_back(take(old_sphere, game_sphere, i));
        flag_values.push_back(take(old_flag, game_flag, i));
        pos_values.push_back(take(old_pos, game_pos, i));
        rot_values.push_back(take(old_rot, game_rot, i));
        scale_values.push_back(take(old_scale, game_scale, i));
    }
    mod_skeleton.Set("m_boneName", CopiedArray(name_values));
    mod_skeleton.Set("m_nParent", CopiedArray(parent_values));
    mod_skeleton.Set("m_boneSphere", CopiedArray(sphere_values));
    mod_skeleton.Set("m_nFlag", CopiedArray(flag_values));
    mod_skeleton.Set("m_bonePosParent", CopiedArray(pos_values));
    mod_skeleton.Set("m_boneRotParent", CopiedArray(rot_values));
    mod_skeleton.Set("m_boneScaleParent", CopiedArray(scale_values));

    const KvValue* old_remap = mod_root.Find("m_remappingTable");
    const bool had_remap = old_remap != nullptr;
    size_t old_remap_count = 0;
    if (had_remap) {
        if (!old_remap->IsArray()) {
            throw InvalidData("m_remappingTable is not an array.");
        }
        old_remap_count = old_remap->Count();
        std::vector<KvValue> new_remap;
        new_remap.reserve(old_remap_count);
        for (size_t i = 0; i < old_remap_count; ++i) {
            const int value = old_remap->At(i).ToInt32();
            if (value < 0) {
                new_remap.push_back(KvValue::Int32(value));
            } else if (static_cast<size_t>(value) >= new_index_of_old.size()) {
                throw InvalidData("Remap entry " + std::to_string(i) + " points at bone " + std::to_string(value) + ", which is outside the skeleton.");
            } else {
                new_remap.push_back(KvValue::Int32(new_index_of_old[static_cast<size_t>(value)]));
            }
        }
        mod_root.Set("m_remappingTable", KvValue::Array(std::move(new_remap)));
    }

    const std::vector<uint8_t> patched = ReplaceDataBlock(mod_bytes, WriteKv3(mod_document));
    VerifyRewritten(patched, new_names, mod_only, mod, game, new_index_of_old, had_remap, old_remap_count);

    const std::string remap_note = had_remap
        ? "Mesh bone indices were remapped."
        : "This model has no mesh bone remap table.";
    std::string detail = "Bones " + std::to_string(mod.Names.size()) + " to " + std::to_string(new_names.size()) + " ("
        + std::to_string(game.Names.size()) + " from the game, " + std::to_string(mod_only.size()) + " kept from the mod). "
        + remap_note + " " + assessment.Detail + " Test animations and attachments in-game before keeping the repair.";
    MigrationEdit edit = WithOutcome(ModelOutcome::Updated, std::move(detail), std::move(warnings));
    edit.Bytes = patched;
    return edit;
}

}  // namespace

MigrationEdit ModelMigration::Migrate(std::span<const uint8_t> mod_bytes, std::string_view mod_path,
    std::span<const uint8_t> game_bytes, std::string_view game_path) {
    try {
        KvDocument mod_document = LoadModel(mod_bytes, mod_path);
        const KvDocument game_document = LoadModel(game_bytes, game_path);
        const ModelBones mod = ReadBones(mod_document.Root, "Duplicate bone name ");
        const ModelBones game = ReadBones(game_document.Root, "The game model has a duplicate bone name ");
        if (mod.Names.empty()) {
            return WithOutcome(ModelOutcome::Skipped, "No bones.");
        }
        if (game.Names.empty()) {
            throw InvalidData("The game model has no bones.");
        }
        if (!EqualsIgnoreCase(mod_path, game_path)) {
            return WithOutcome(ModelOutcome::NeedsReview,
                "Possible model move to " + std::string(game_path) + ". Path migration requires checking dependent resources; this MVP will not guess.");
        }

        const KvValue* external = mod_document.Root.Find("m_vecNmSkeletonRefs");
        const bool uses_external = external != nullptr && external->IsArray() && external->Count() > 0;
        const SkeletonAssessment assessment = SkeletonCompatibility::Assess(mod.Shapes, game.Shapes, uses_external);
        if (assessment.NeedsReview) {
            return WithOutcome(ModelOutcome::NeedsReview, assessment.Detail, assessment.Warnings);
        }
        // AG2 render bones are not rewritten. Copying the stock rig would change a working custom model.
        if (uses_external) {
            return WithOutcome(ModelOutcome::Current,
                "External animation skeleton bindings are used. No render-skeleton rewrite is needed. " + assessment.Detail,
                assessment.Warnings);
        }

        std::vector<std::string> warnings = assessment.Warnings;
        std::vector<std::string> flag_changes;
        for (size_t i = 0; i < game.Names.size(); ++i) {
            const auto found = mod.Index.find(game.Names[i]);
            if (found != mod.Index.end() && game.Flags[i] != mod.Flags[static_cast<size_t>(found->second)]) {
                flag_changes.push_back(game.Names[i]);
            }
        }
        if (!flag_changes.empty()) {
            warnings.push_back("Authored bone usage/LOD flags differ (" + std::to_string(flag_changes.size()) + "); mod flags are preserved.");
        }

        bool aligned = mod.Names.size() >= game.Names.size();
        if (aligned) {
            for (size_t i = 0; i < game.Names.size(); ++i) {
                if (mod.Names[i] != game.Names[i]) {
                    aligned = false;
                    break;
                }
            }
        }
        if (aligned) {
            return WithOutcome(ModelOutcome::Current,
                "No supported skeleton differences found. In-game animation is not verified. " + assessment.Detail, warnings);
        }

        const KvValue* remap = mod_document.Root.Find("m_remappingTable");
        if (remap == nullptr || remap->Count() == 0) {
            return WithOutcome(ModelOutcome::NeedsReview,
                "Skeleton differs, but no mesh bone remap table is available. Direct mesh indices require author review.", warnings);
        }
        const std::vector<std::string> unknown = UnknownSkeletonKeys(SkeletonOf(mod_document.Root));
        if (!unknown.empty()) {
            return WithOutcome(ModelOutcome::NeedsReview,
                "Skeleton contains unsupported metadata: " + Join(unknown) + ". Its bone references need author review.", warnings);
        }
        std::vector<std::string> embedded;
        for (const char* type : {"PHYS", "ANIM", "ASEQ", "AGRP"}) {
            if (HasPositiveBlock(mod_bytes, type)) {
                embedded.emplace_back(type);
            }
        }
        if (!embedded.empty()) {
            return WithOutcome(ModelOutcome::NeedsReview,
                "Skeleton differs and the model has embedded " + Join(embedded) + " data. Its bone references need author review.", warnings);
        }
        return Rewrite(std::move(mod_document), game_document, mod_bytes, mod, game, assessment, std::move(warnings));
    } catch (const InvalidData& ex) {
        const std::string message = ex.what();
        if (!IsBlank(message)) {
            return WithOutcome(ModelOutcome::Failed, message);
        }
        return WithOutcome(ModelOutcome::Failed, message);
    } catch (const std::exception& ex) {
        return WithOutcome(ModelOutcome::Failed, ex.what());
    }
}

}  // namespace skeleton_repair

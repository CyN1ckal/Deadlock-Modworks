#include "skeleton.hpp"

#include "error.hpp"
#include "strings.hpp"

#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

std::string Join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

std::string Examples(const std::vector<std::string>& names) {
    std::vector<std::string> head;
    const size_t count = names.size() < 6 ? names.size() : 6;
    head.insert(head.end(), names.begin(), names.begin() + static_cast<std::ptrdiff_t>(count));
    return Join(head, ", ");
}

float LengthSquared(Quat rotation) {
    return rotation.X * rotation.X + rotation.Y * rotation.Y + rotation.Z * rotation.Z + rotation.W * rotation.W;
}

Quat Normalize(Quat rotation) {
    const float length = std::sqrt(LengthSquared(rotation));
    const float inverse = 1.0f / length;
    return Quat{rotation.X * inverse, rotation.Y * inverse, rotation.Z * inverse, rotation.W * inverse};
}

float Dot(Quat left, Quat right) {
    return left.X * right.X + left.Y * right.Y + left.Z * right.Z + left.W * right.W;
}

float Distance(Vec3 left, Vec3 right) {
    const float dx = left.X - right.X;
    const float dy = left.Y - right.Y;
    const float dz = left.Z - right.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool SamePose(const BoneShape& left, const BoneShape& right) {
    return Distance(left.Position, right.Position) <= 0.001f
        && std::fabs(Dot(Normalize(left.Rotation), Normalize(right.Rotation))) >= 0.99999f
        && std::fabs(left.Scale - right.Scale) <= 0.0001f;
}

void Validate(const std::vector<BoneShape>& bones, const std::string& label) {
    std::unordered_map<std::string, const BoneShape*> by_name;
    for (const BoneShape& bone : bones) {
        if (IsBlank(bone.Name) || !by_name.emplace(bone.Name, &bone).second) {
            throw InvalidData(label + " has an empty or duplicate bone name.");
        }
        const float rotation_length = LengthSquared(bone.Rotation);
        if (!std::isfinite(bone.Position.X) || !std::isfinite(bone.Position.Y) || !std::isfinite(bone.Position.Z)
            || !std::isfinite(rotation_length) || rotation_length < 0.000001f
            || !std::isfinite(bone.Scale) || bone.Scale <= 0) {
            throw InvalidData(label + " bone " + bone.Name + " has an invalid transform.");
        }
    }

    for (const BoneShape& bone : bones) {
        std::unordered_set<std::string> seen;
        seen.insert(bone.Name);
        std::optional<std::string> parent = bone.Parent;
        while (parent.has_value()) {
            const auto ancestor = by_name.find(*parent);
            if (ancestor == by_name.end()) {
                throw InvalidData(label + " bone " + bone.Name + " has a missing parent " + *parent + ".");
            }
            if (!seen.insert(*parent).second) {
                throw InvalidData(label + " skeleton contains a parent cycle at " + *parent + ".");
            }
            parent = ancestor->second->Parent;
        }
    }
}

const std::vector<std::string>* FindGroup(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& groups,
    const std::string& name) {
    for (const auto& group : groups) {
        if (EqualsIgnoreCase(group.first, name)) {
            return &group.second;
        }
    }
    return nullptr;
}

int GroupCount(const std::vector<std::pair<std::string, int>>& groups, const std::string& name) {
    for (const auto& group : groups) {
        if (EqualsIgnoreCase(group.first, name)) {
            return group.second;
        }
    }
    return 0;
}

}  // namespace

SkeletonAssessment SkeletonCompatibility::Assess(const std::vector<BoneShape>& mod, const std::vector<BoneShape>& game,
    bool uses_external_animation_skeleton) {
    Validate(mod, "Mod");
    Validate(game, "Game");

    std::unordered_map<std::string, const BoneShape*> old_bones;
    for (const BoneShape& bone : mod) {
        old_bones.emplace(bone.Name, &bone);
    }
    std::unordered_map<std::string, const BoneShape*> current_bones;
    for (const BoneShape& bone : game) {
        current_bones.emplace(bone.Name, &bone);
    }

    std::vector<std::string> added;
    for (const BoneShape& bone : game) {
        if (!old_bones.contains(bone.Name)) {
            added.push_back(bone.Name);
        }
    }
    std::vector<std::string> extra;
    for (const BoneShape& bone : mod) {
        if (!current_bones.contains(bone.Name)) {
            extra.push_back(bone.Name);
        }
    }
    std::vector<const BoneShape*> shared;
    for (const BoneShape& bone : game) {
        if (old_bones.contains(bone.Name)) {
            shared.push_back(&bone);
        }
    }

    std::vector<std::string> parents;
    std::vector<std::string> poses;
    for (const BoneShape* bone : shared) {
        const BoneShape& previous = *old_bones.at(bone->Name);
        if (bone->Parent != previous.Parent) {
            parents.push_back(bone->Name);
        }
        if (!SamePose(*bone, previous)) {
            poses.push_back(bone->Name);
        }
    }

    std::vector<std::string> reasons;
    std::vector<std::string> warnings;
    if (shared.empty()) {
        reasons.emplace_back("No shared bones; a different rig or renamed skeleton needs author review.");
    }
    if (!parents.empty()) {
        const std::string note = "Authored hierarchy differs (" + std::to_string(parents.size()) + "): "
            + Examples(parents) + ". Mod parents are preserved.";
        if (uses_external_animation_skeleton) {
            warnings.push_back(note);
        } else {
            reasons.push_back(note);
        }
    }
    if (!poses.empty()) {
        warnings.push_back("Custom rest poses (" + std::to_string(poses.size()) + "): " + Examples(poses)
            + ". Mod transforms are preserved; this is not evidence of a broken mod.");
    }
    if (!added.empty() && !extra.empty()) {
        if (uses_external_animation_skeleton) {
            warnings.emplace_back("Stock-only bones and custom helpers differ. The render skeleton does not need to copy the stock mesh's bone list.");
        } else {
            reasons.emplace_back("An embedded/legacy skeleton has both missing and extra bones; a rename mapping cannot be inferred safely.");
        }
    }

    std::string summary = "Game-only bones: " + std::to_string(added.size()) + "; mod-only bones: " + std::to_string(extra.size()) + ".";
    if (!added.empty()) {
        summary += " Game-only examples: " + Examples(added) + ".";
    }
    if (!extra.empty()) {
        summary += " Mod-only examples: " + Examples(extra) + ".";
    }
    const bool needs_review = !reasons.empty();
    reasons.push_back(summary);
    return SkeletonAssessment{needs_review, Join(reasons, " "), std::move(warnings)};
}

BoneNameMappingResult BoneNameMapping::Resolve(const std::vector<std::string>& old_names, const std::vector<std::string>& current_names) {
    std::unordered_set<std::string> exact(current_names.begin(), current_names.end());

    std::vector<std::pair<std::string, int>> old_groups;
    for (const std::string& name : old_names) {
        bool found = false;
        for (auto& group : old_groups) {
            if (EqualsIgnoreCase(group.first, name)) {
                ++group.second;
                found = true;
                break;
            }
        }
        if (!found) {
            old_groups.emplace_back(name, 1);
        }
    }

    std::vector<std::pair<std::string, std::vector<std::string>>> new_groups;
    for (const std::string& name : current_names) {
        bool found = false;
        for (auto& group : new_groups) {
            if (EqualsIgnoreCase(group.first, name)) {
                group.second.push_back(name);
                found = true;
                break;
            }
        }
        if (!found) {
            new_groups.push_back({name, {name}});
        }
    }

    std::unordered_set<std::string> used;
    for (const std::string& name : old_names) {
        if (exact.contains(name)) {
            used.insert(name);
        }
    }

    BoneNameMappingResult result;
    for (const std::string& name : old_names) {
        if (exact.contains(name)) {
            continue;
        }
        const std::vector<std::string>* candidates = FindGroup(new_groups, name);
        if (candidates == nullptr) {
            result.Unmatched.push_back(name);
            continue;
        }
        const std::string& chosen = candidates->front();
        if (candidates->size() != 1 || GroupCount(old_groups, name) != 1 || !used.insert(chosen).second) {
            result.Unmatched.push_back(name);
            result.Ambiguous.push_back(name);
            continue;
        }
        result.Aliases.emplace_back(name, chosen);
    }
    return result;
}

}  // namespace skeleton_repair

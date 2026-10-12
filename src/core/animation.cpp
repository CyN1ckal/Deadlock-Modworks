#include "animation.hpp"

#include "error.hpp"
#include "kv3.hpp"
#include "resource.hpp"
#include "skeleton.hpp"
#include "strings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace skeleton_repair {
namespace {

constexpr float kDecomposeEpsilon = 0.0001f;

// Row-major, matching System.Numerics.Matrix4x4 (row vectors).
struct Mat4 {
    float M[4][4]{};
};

bool IsBlank(std::string_view text) {
    for (unsigned char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v') {
            return false;
        }
    }
    return true;
}

std::string ListUpTo(const std::vector<std::string>& names) {
    std::string out;
    const size_t count = std::min<size_t>(names.size(), 6);
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += names[i];
    }
    return out;
}

MigrationEdit Review(std::string detail) {
    MigrationEdit edit;
    edit.Outcome = ModelOutcome::NeedsReview;
    edit.Detail = std::move(detail);
    return edit;
}

MigrationEdit Failed(const std::string& detail) {
    MigrationEdit edit;
    edit.Outcome = ModelOutcome::Failed;
    edit.Detail = detail;
    return edit;
}

const std::string* AliasTarget(const std::vector<std::pair<std::string, std::string>>& aliases, const std::string& name) {
    for (const auto& alias : aliases) {
        if (alias.first == name) {
            return &alias.second;
        }
    }
    return nullptr;
}

std::string Canonical(const std::string& name, const std::vector<std::pair<std::string, std::string>>& aliases) {
    if (const std::string* target = AliasTarget(aliases, name)) {
        return *target;
    }
    return name;
}

bool IsKnownAnimationKey(std::string_view key) {
    return key == "m_ID" || key == "m_boneIDs" || key == "m_parentIndices" || key == "m_parentSpaceReferencePose"
        || key == "m_modelSpaceReferencePose" || key == "m_numBonesToSampleAtLowLOD" || key == "m_maskDefinitions"
        || key == "m_secondarySkeletons" || key == "m_bIsPropSkeleton";
}

uint32_t ReadU32(std::span<const uint8_t> bytes, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::array<float, 8> Numbers(const KvValue& pose) {
    if (!pose.IsArray() || pose.Count() != 8) {
        throw InvalidData("Animation reference pose is not an eight-component transform.");
    }
    std::array<float, 8> values{};
    for (size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<float>(pose.At(i).ToDouble());
        if (!std::isfinite(values[i])) {
            throw InvalidData("Animation reference pose has non-finite values.");
        }
    }
    const float length = values[4] * values[4] + values[5] * values[5] + values[6] * values[6] + values[7] * values[7];
    if (values[3] <= 0.0f || !std::isfinite(length) || length < 0.000001f) {
        throw InvalidData("Animation reference pose has an invalid scale or rotation.");
    }
    return values;
}

int32_t ReadLod(const KvValue& value, size_t bone_count) {
    int32_t lod = 0;
    try {
        lod = value.ToInt32();
    } catch (const InvalidData& error) {
        if (std::string(error.what()) == "KV3 integer does not fit.") {
            throw InvalidData("Animation LOD sampling count is outside the skeleton.");
        }
        throw;
    }
    if (lod < 0 || static_cast<size_t>(lod) > bone_count) {
        throw InvalidData("Animation LOD sampling count is outside the skeleton.");
    }
    return lod;
}

struct SkeletonData {
    std::vector<std::string> Names;
    std::vector<int> Parents;
    int Lod = 0;
};

SkeletonData Validate(const KvValue& root) {
    const KvValue* id = root.IsObject() ? root.Find("m_ID") : nullptr;
    const KvValue* bone_ids = root.IsObject() ? root.Find("m_boneIDs") : nullptr;
    if (id == nullptr || bone_ids == nullptr || !bone_ids->IsArray()) {
        throw InvalidData("Animation skeleton has no identity or bone-ID array.");
    }

    SkeletonData data;
    std::unordered_set<std::string> seen;
    for (const KvValue& value : bone_ids->Items()) {
        const std::string name = value.String();
        if (IsBlank(name) || !seen.insert(name).second) {
            throw InvalidData("Animation skeleton has empty or duplicate bone IDs.");
        }
        data.Names.push_back(name);
    }
    if (data.Names.empty()) {
        throw InvalidData("Animation skeleton has empty or duplicate bone IDs.");
    }

    for (const char* key : {"m_parentIndices", "m_parentSpaceReferencePose", "m_modelSpaceReferencePose"}) {
        const KvValue* value = root.Find(key);
        if (value == nullptr || !value->IsArray() || value->Count() != data.Names.size()) {
            throw InvalidData(std::string("Animation skeleton ") + key + " does not match its bone count.");
        }
    }

    data.Parents.reserve(data.Names.size());
    std::vector<BoneShape> shapes;
    shapes.reserve(data.Names.size());
    for (size_t i = 0; i < data.Names.size(); ++i) {
        const int parent = root.Find("m_parentIndices")->At(i).ToInt32();
        if (parent < -1 || parent >= static_cast<int>(data.Names.size())) {
            throw InvalidData("Animation parent index is outside the skeleton.");
        }
        data.Parents.push_back(parent);
        const std::array<float, 8> local = Numbers(root.Find("m_parentSpaceReferencePose")->At(i));
        Numbers(root.Find("m_modelSpaceReferencePose")->At(i));
        BoneShape shape;
        shape.Name = data.Names[i];
        if (parent >= 0) {
            shape.Parent = data.Names[static_cast<size_t>(parent)];
        }
        shape.Position = Vec3{local[0], local[1], local[2]};
        shape.Rotation = Quat{local[4], local[5], local[6], local[7]};
        shape.Scale = local[3];
        shapes.push_back(std::move(shape));
    }
    SkeletonCompatibility::Assess(shapes, shapes);

    const KvValue* lod = root.Find("m_numBonesToSampleAtLowLOD");
    if (lod == nullptr) {
        throw InvalidData("Animation skeleton has no LOD sampling count.");
    }
    data.Lod = ReadLod(*lod, data.Names.size());
    return data;
}

Mat4 Identity() {
    Mat4 result;
    result.M[0][0] = 1.0f;
    result.M[1][1] = 1.0f;
    result.M[2][2] = 1.0f;
    result.M[3][3] = 1.0f;
    return result;
}

Mat4 Mul(const Mat4& left, const Mat4& right) {
    Mat4 result;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            result.M[row][col] = left.M[row][0] * right.M[0][col] + left.M[row][1] * right.M[1][col]
                + left.M[row][2] * right.M[2][col] + left.M[row][3] * right.M[3][col];
        }
    }
    return result;
}

Mat4 ScaleMatrix(float scale) {
    Mat4 result = Identity();
    result.M[0][0] = scale;
    result.M[1][1] = scale;
    result.M[2][2] = scale;
    return result;
}

Mat4 TranslationMatrix(float x, float y, float z) {
    Mat4 result = Identity();
    result.M[3][0] = x;
    result.M[3][1] = y;
    result.M[3][2] = z;
    return result;
}

Mat4 QuaternionMatrix(float x, float y, float z, float w) {
    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float wz = z * w;
    const float xz = x * z;
    const float wy = y * w;
    const float yz = y * z;
    const float wx = x * w;
    Mat4 result = Identity();
    result.M[0][0] = 1.0f - 2.0f * (yy + zz);
    result.M[0][1] = 2.0f * (xy + wz);
    result.M[0][2] = 2.0f * (xz - wy);
    result.M[1][0] = 2.0f * (xy - wz);
    result.M[1][1] = 1.0f - 2.0f * (zz + xx);
    result.M[1][2] = 2.0f * (yz + wx);
    result.M[2][0] = 2.0f * (xz + wy);
    result.M[2][1] = 2.0f * (yz - wx);
    result.M[2][2] = 1.0f - 2.0f * (yy + xx);
    return result;
}

float RowLength(const Mat4& matrix, int row) {
    return std::sqrt(matrix.M[row][0] * matrix.M[row][0] + matrix.M[row][1] * matrix.M[row][1]
        + matrix.M[row][2] * matrix.M[row][2]);
}

void SetRow(Mat4& matrix, int row, float x, float y, float z) {
    matrix.M[row][0] = x;
    matrix.M[row][1] = y;
    matrix.M[row][2] = z;
    matrix.M[row][3] = 0.0f;
}

void NormalizeRow(Mat4& matrix, int row) {
    const float length = RowLength(matrix, row);
    matrix.M[row][0] /= length;
    matrix.M[row][1] /= length;
    matrix.M[row][2] /= length;
    matrix.M[row][3] = 0.0f;
}

bool Decompose(const Mat4& source, float& px, float& py, float& pz, float& scale, float& qx, float& qy, float& qz, float& qw) {
    px = source.M[3][0];
    py = source.M[3][1];
    pz = source.M[3][2];

    Mat4 basis = Identity();
    for (int row = 0; row < 3; ++row) {
        SetRow(basis, row, source.M[row][0], source.M[row][1], source.M[row][2]);
    }
    float scales[3] = {RowLength(basis, 0), RowLength(basis, 1), RowLength(basis, 2)};

    int a = 0;
    int b = 1;
    int c = 2;
    const float x = scales[0];
    const float y = scales[1];
    const float z = scales[2];
    if (x < y) {
        if (y < z) {
            a = 2;
            b = 1;
            c = 0;
        } else if (x < z) {
            a = 1;
            b = 2;
            c = 0;
        } else {
            a = 1;
            b = 0;
            c = 2;
        }
    } else if (x < z) {
        a = 2;
        b = 0;
        c = 1;
    } else if (y < z) {
        a = 0;
        b = 2;
        c = 1;
    }

    if (scales[a] < kDecomposeEpsilon) {
        if (a == 0) {
            SetRow(basis, a, 1.0f, 0.0f, 0.0f);
        } else if (a == 1) {
            SetRow(basis, a, 0.0f, 1.0f, 0.0f);
        } else {
            SetRow(basis, a, 0.0f, 0.0f, 1.0f);
        }
    }
    NormalizeRow(basis, a);

    if (scales[b] < kDecomposeEpsilon) {
        const float abs_x = std::fabs(basis.M[a][0]);
        const float abs_y = std::fabs(basis.M[a][1]);
        const float abs_z = std::fabs(basis.M[a][2]);
        int canonical = 2;
        if (abs_x < abs_y) {
            canonical = (abs_y < abs_z || abs_x < abs_z) ? 0 : 2;
        } else {
            canonical = (abs_x < abs_z || abs_y < abs_z) ? 1 : 2;
        }
        const float bx = canonical == 0 ? 1.0f : 0.0f;
        const float by = canonical == 1 ? 1.0f : 0.0f;
        const float bz = canonical == 2 ? 1.0f : 0.0f;
        SetRow(basis, b,
            basis.M[a][1] * bz - basis.M[a][2] * by,
            basis.M[a][2] * bx - basis.M[a][0] * bz,
            basis.M[a][0] * by - basis.M[a][1] * bx);
    }
    NormalizeRow(basis, b);

    if (scales[c] < kDecomposeEpsilon) {
        SetRow(basis, c,
            basis.M[a][1] * basis.M[b][2] - basis.M[a][2] * basis.M[b][1],
            basis.M[a][2] * basis.M[b][0] - basis.M[a][0] * basis.M[b][2],
            basis.M[a][0] * basis.M[b][1] - basis.M[a][1] * basis.M[b][0]);
    }
    NormalizeRow(basis, c);

    float det = basis.M[0][0] * (basis.M[1][1] * basis.M[2][2] - basis.M[1][2] * basis.M[2][1])
        - basis.M[0][1] * (basis.M[1][0] * basis.M[2][2] - basis.M[1][2] * basis.M[2][0])
        + basis.M[0][2] * (basis.M[1][0] * basis.M[2][1] - basis.M[1][1] * basis.M[2][0]);
    if (std::signbit(det)) {
        scales[a] = -scales[a];
        basis.M[a][0] = -basis.M[a][0];
        basis.M[a][1] = -basis.M[a][1];
        basis.M[a][2] = -basis.M[a][2];
        det = -det;
    }
    det -= 1.0f;
    det *= det;
    scale = scales[0];
    if (kDecomposeEpsilon < det) {
        qx = 0.0f;
        qy = 0.0f;
        qz = 0.0f;
        qw = 1.0f;
        return false;
    }

    const float trace = basis.M[0][0] + basis.M[1][1] + basis.M[2][2];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f);
        const float inverse = 0.5f / s;
        qx = (basis.M[1][2] - basis.M[2][1]) * inverse;
        qy = (basis.M[2][0] - basis.M[0][2]) * inverse;
        qz = (basis.M[0][1] - basis.M[1][0]) * inverse;
        qw = s * 0.5f;
    } else if (basis.M[0][0] >= basis.M[1][1] && basis.M[0][0] >= basis.M[2][2]) {
        const float s = std::sqrt(1.0f + basis.M[0][0] - basis.M[1][1] - basis.M[2][2]);
        const float inverse = 0.5f / s;
        qx = 0.5f * s;
        qy = (basis.M[0][1] + basis.M[1][0]) * inverse;
        qz = (basis.M[0][2] + basis.M[2][0]) * inverse;
        qw = (basis.M[1][2] - basis.M[2][1]) * inverse;
    } else if (basis.M[1][1] > basis.M[2][2]) {
        const float s = std::sqrt(1.0f + basis.M[1][1] - basis.M[0][0] - basis.M[2][2]);
        const float inverse = 0.5f / s;
        qx = (basis.M[1][0] + basis.M[0][1]) * inverse;
        qy = 0.5f * s;
        qz = (basis.M[2][1] + basis.M[1][2]) * inverse;
        qw = (basis.M[2][0] - basis.M[0][2]) * inverse;
    } else {
        const float s = std::sqrt(1.0f + basis.M[2][2] - basis.M[0][0] - basis.M[1][1]);
        const float inverse = 0.5f / s;
        qx = (basis.M[2][0] + basis.M[0][2]) * inverse;
        qy = (basis.M[2][1] + basis.M[1][2]) * inverse;
        qz = 0.5f * s;
        qw = (basis.M[0][1] - basis.M[1][0]) * inverse;
    }
    return true;
}

KvValue PoseValue(float px, float py, float pz, float scale, float qx, float qy, float qz, float qw) {
    return KvValue::Array({
        KvValue::Float(px), KvValue::Float(py), KvValue::Float(pz), KvValue::Float(scale),
        KvValue::Float(qx), KvValue::Float(qy), KvValue::Float(qz), KvValue::Float(qw),
    });
}

std::vector<KvValue> WorldPoses(const std::vector<int>& parents, const std::vector<std::array<float, 8>>& local) {
    struct Resolver {
        const std::vector<int>& Parents;
        const std::vector<std::array<float, 8>>& Local;
        std::vector<Mat4> World;
        std::vector<char> Done;

        const Mat4& Resolve(size_t index) {
            if (Done[index] != 0) {
                return World[index];
            }
            const std::array<float, 8>& pose = Local[index];
            Mat4 matrix = Mul(Mul(ScaleMatrix(pose[3]), QuaternionMatrix(pose[4], pose[5], pose[6], pose[7])),
                TranslationMatrix(pose[0], pose[1], pose[2]));
            if (Parents[index] >= 0) {
                matrix = Mul(matrix, Resolve(static_cast<size_t>(Parents[index])));
            }
            World[index] = matrix;
            Done[index] = 1;
            return World[index];
        }
    };

    Resolver resolver{parents, local, std::vector<Mat4>(local.size()), std::vector<char>(local.size(), 0)};
    std::vector<KvValue> world;
    world.reserve(local.size());
    for (size_t index = 0; index < local.size(); ++index) {
        float px = 0;
        float py = 0;
        float pz = 0;
        float scale = 0;
        float qx = 0;
        float qy = 0;
        float qz = 0;
        float qw = 1;
        if (!Decompose(resolver.Resolve(index), px, py, pz, scale, qx, qy, qz, qw)) {
            throw InvalidData("Animation world reference pose could not be rebuilt.");
        }
        world.push_back(PoseValue(px, py, pz, scale, qx, qy, qz, qw));
    }
    return world;
}

int RenameMaskBoneIds(KvValue& node, const std::vector<std::pair<std::string, std::string>>& aliases, const std::string& skeleton_id) {
    if (node.IsArray()) {
        int count = 0;
        for (KvValue& child : node.Items()) {
            count += RenameMaskBoneIds(child, aliases, skeleton_id);
        }
        return count;
    }
    if (!node.IsObject()) {
        return 0;
    }
    if (const KvValue* skeleton = node.Find("m_skeletonName")) {
        if (skeleton->Kind == KvKind::String && !skeleton->String().empty() && skeleton->String() != skeleton_id) {
            return 0;
        }
    }

    int count = 0;
    const size_t fields = node.Count();
    for (size_t i = 0; i < fields; ++i) {
        auto& field = node.Fields()[i];
        if (field.first == "m_boneIDs" && field.second.IsArray()) {
            std::vector<KvValue> replaced;
            replaced.reserve(field.second.Count());
            for (const KvValue& value : field.second.Items()) {
                const std::string* target = value.Kind == KvKind::String ? AliasTarget(aliases, value.String()) : nullptr;
                if (target == nullptr) {
                    replaced.push_back(value);
                } else {
                    ++count;
                    replaced.push_back(KvValue::String(*target));
                }
            }
            field.second = KvValue::Array(std::move(replaced));
        } else {
            count += RenameMaskBoneIds(field.second, aliases, skeleton_id);
        }
    }
    return count;
}

int CountMaskBoneIds(const KvValue& node, const std::unordered_set<std::string>& names, const std::string& skeleton_id) {
    if (node.IsArray()) {
        int count = 0;
        for (const KvValue& child : node.Items()) {
            count += CountMaskBoneIds(child, names, skeleton_id);
        }
        return count;
    }
    if (!node.IsObject()) {
        return 0;
    }
    if (const KvValue* skeleton = node.Find("m_skeletonName")) {
        if (skeleton->Kind == KvKind::String && !skeleton->String().empty() && skeleton->String() != skeleton_id) {
            return 0;
        }
    }

    int count = 0;
    for (const auto& field : node.Fields()) {
        if (field.first == "m_boneIDs" && field.second.IsArray()) {
            for (const KvValue& value : field.second.Items()) {
                if (value.Kind == KvKind::String && names.contains(value.String())) {
                    ++count;
                }
            }
        } else {
            count += CountMaskBoneIds(field.second, names, skeleton_id);
        }
    }
    return count;
}

bool ReadSupportedKv3(std::span<const uint8_t> resource, KvDocument& document) {
    const std::span<const uint8_t> data = FindDataBlock(resource);
    if (data.size() < 4 || !IsBinaryKv3(ReadU32(data, 0))) {
        return false;
    }
    document = ReadKv3(data);
    return true;
}

std::optional<std::string> ParentName(const SkeletonData& skeleton, int index) {
    const int parent = skeleton.Parents[static_cast<size_t>(index)];
    if (parent < 0) {
        return std::nullopt;
    }
    return skeleton.Names[static_cast<size_t>(parent)];
}

}  // namespace

MigrationEdit AnimationSkeletonMigration::Migrate(std::span<const uint8_t> mod_bytes, std::string_view mod_path,
    std::span<const uint8_t> game_bytes, std::string_view game_path) {
    try {
        if (!EqualsIgnoreCase(mod_path, game_path)) {
            return Review("Animation skeleton path migration is not supported.");
        }

        KvDocument mod_document;
        KvDocument game_document;
        if (!ReadSupportedKv3(mod_bytes, mod_document) || !ReadSupportedKv3(game_bytes, game_document)) {
            return Review("Animation skeleton does not use a supported KV3 DATA block.");
        }

        const SkeletonData old = Validate(mod_document.Root);
        const SkeletonData current = Validate(game_document.Root);
        if (mod_document.Root.Find("m_ID")->String() != game_document.Root.Find("m_ID")->String()) {
            return Review("Animation skeleton identities differ; this may be a custom skeleton.");
        }

        const BoneNameMappingResult mapping = BoneNameMapping::Resolve(old.Names, current.Names);
        if (!mapping.Unmatched.empty()) {
            std::string detail = "Animation override has custom or removed bones: " + ListUpTo(mapping.Unmatched)
                + ". An explicit mapping is needed.";
            if (!mapping.Ambiguous.empty()) {
                detail += " Case matching is ambiguous; multiple names would collide.";
            }
            return Review(std::move(detail));
        }

        std::unordered_map<std::string, int> old_index;
        for (size_t i = 0; i < old.Names.size(); ++i) {
            if (!old_index.emplace(Canonical(old.Names[i], mapping.Aliases), static_cast<int>(i)).second) {
                throw InvalidData("An item with the same key has already been added.");
            }
        }

        std::vector<std::string> changed_parents;
        for (size_t index = 0; index < current.Names.size(); ++index) {
            const auto found = old_index.find(current.Names[index]);
            if (found == old_index.end()) {
                continue;
            }
            const std::optional<std::string> old_parent = ParentName(old, found->second);
            const std::optional<std::string> old_canonical = old_parent.has_value()
                ? std::optional<std::string>(Canonical(*old_parent, mapping.Aliases))
                : std::nullopt;
            if (old_canonical != ParentName(current, static_cast<int>(index))) {
                changed_parents.push_back(current.Names[index]);
            }
        }
        if (!changed_parents.empty()) {
            return Review("Animation override parents changed: " + ListUpTo(changed_parents) + ". Retargeting needs author review.");
        }

        if (old.Names == current.Names && old.Parents == current.Parents && old.Lod == current.Lod) {
            MigrationEdit edit;
            edit.Outcome = ModelOutcome::Current;
            edit.Detail = "Animation bone order, parent bindings and low-LOD sampling match the installed game. Authored poses and masks are preserved.";
            return edit;
        }

        std::vector<std::string> unknown;
        for (const auto& field : mod_document.Root.Fields()) {
            if (!IsKnownAnimationKey(field.first)) {
                unknown.push_back(field.first);
            }
        }
        const KvValue* secondary = mod_document.Root.Find("m_secondarySkeletons");
        if (!unknown.empty() || (secondary != nullptr && secondary->Count() > 0)) {
            return Review("Animation override contains unsupported metadata or secondary skeletons.");
        }

        const KvValue& old_poses = *mod_document.Root.Find("m_parentSpaceReferencePose");
        const KvValue& game_poses = *game_document.Root.Find("m_parentSpaceReferencePose");
        std::vector<KvValue> local_poses;
        std::vector<std::array<float, 8>> local_numbers;
        local_poses.reserve(current.Names.size());
        local_numbers.reserve(current.Names.size());
        for (size_t index = 0; index < current.Names.size(); ++index) {
            const auto found = old_index.find(current.Names[index]);
            const KvValue& pose = found == old_index.end() ? game_poses.At(index) : old_poses.At(static_cast<size_t>(found->second));
            local_numbers.push_back(Numbers(pose));
            local_poses.push_back(pose);
        }

        mod_document.Root.Set("m_boneIDs", *game_document.Root.Find("m_boneIDs"));
        mod_document.Root.Set("m_parentIndices", *game_document.Root.Find("m_parentIndices"));
        mod_document.Root.Set("m_parentSpaceReferencePose", KvValue::Array(local_poses));
        mod_document.Root.Set("m_modelSpaceReferencePose", KvValue::Array(WorldPoses(current.Parents, local_numbers)));
        mod_document.Root.Set("m_numBonesToSampleAtLowLOD", *game_document.Root.Find("m_numBonesToSampleAtLowLOD"));

        const std::string skeleton_id = mod_document.Root.Find("m_ID")->String();
        int renamed_mask_references = 0;
        if (KvValue* masks = mod_document.Root.Find("m_maskDefinitions")) {
            renamed_mask_references = RenameMaskBoneIds(*masks, mapping.Aliases, skeleton_id);
        }

        const std::vector<uint8_t> patched = ReplaceDataBlock(mod_bytes, WriteKv3(mod_document));
        const KvDocument checked = ReadKv3(FindDataBlock(patched));
        const SkeletonData checked_data = Validate(checked.Root);
        if (checked_data.Names != current.Names || checked_data.Parents != current.Parents) {
            throw InvalidData("Animation skeleton binding verification failed.");
        }
        const KvValue& checked_poses = *checked.Root.Find("m_parentSpaceReferencePose");
        for (size_t i = 0; i < local_numbers.size(); ++i) {
            if (Numbers(checked_poses.At(i)) != local_numbers[i]) {
                throw InvalidData("An authored animation reference pose changed during serialization.");
            }
        }
        if (const KvValue* masks = checked.Root.Find("m_maskDefinitions")) {
            std::unordered_set<std::string> old_names;
            for (const auto& alias : mapping.Aliases) {
                old_names.insert(alias.first);
            }
            if (CountMaskBoneIds(*masks, old_names, skeleton_id) > 0) {
                throw InvalidData("An outdated bone name remains in the animation masks.");
            }
        }

        std::string detail = "Animation override binding layout updated: " + std::to_string(old.Names.size()) + " to "
            + std::to_string(current.Names.size()) + " bones; low-LOD sampling " + std::to_string(old.Lod) + " to "
            + std::to_string(current.Lod)
            + ". Authored parent-space poses and mask weights are preserved, world poses are rebuilt, and model geometry is untouched.";
        if (!mapping.Aliases.empty()) {
            detail += " Case-only bone renames (" + std::to_string(mapping.Aliases.size()) + "): ";
            for (size_t i = 0; i < mapping.Aliases.size(); ++i) {
                if (i != 0) {
                    detail += ", ";
                }
                detail += mapping.Aliases[i].first + " -> " + mapping.Aliases[i].second;
            }
            detail += "; updated " + std::to_string(renamed_mask_references) + " mask references.";
        }

        MigrationEdit edit;
        edit.Bytes = patched;
        edit.Outcome = ModelOutcome::Updated;
        edit.Detail = std::move(detail);
        edit.Warnings = std::vector<std::string>{
            "Test this animation-skeleton repair in-game; structural verification cannot confirm animation playback.",
        };
        edit.BoneAliases = mapping.Aliases;
        return edit;
    } catch (const std::exception& ex) {
        return Failed(ex.what());
    }
}

}  // namespace skeleton_repair

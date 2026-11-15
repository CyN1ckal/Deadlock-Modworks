#include "camera_attachment.hpp"

#include "error.hpp"
#include "kv3.hpp"
#include "resource.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skeleton_repair {
namespace {

constexpr double kFrameDot = 0.999;
constexpr double kRotationTolerance = 1e-3;

struct DQuat {
    double X = 0;
    double Y = 0;
    double Z = 0;
    double W = 1;
};

struct DVec {
    double X = 0;
    double Y = 0;
    double Z = 0;
};

struct BlockRef {
    int Index = 0;
    int Absolute = 0;
    int Size = 0;
    char Type[4] = {};
};

enum class MeshFrame {
    None,
    Current,
    Old,
    Unrecognized,
};

struct MeshInfo {
    int Block = -1;
    MeshFrame Frame = MeshFrame::None;
    int Influences = 0;
    DQuat Bind;
    std::optional<KvDocument> Document;
};

template <typename T>
T ReadPod(std::span<const uint8_t> bytes, size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

bool Finite(double value) {
    return std::isfinite(value);
}

std::optional<double> Number(const KvValue& value) {
    try {
        const double number = value.ToDouble();
        if (!Finite(number)) {
            return std::nullopt;
        }
        return number;
    } catch (const InvalidData&) {
        return std::nullopt;
    }
}

std::optional<int> InfluenceCount(const KvValue& value) {
    try {
        const int count = value.ToInt32();
        if (count < 0) {
            return std::nullopt;
        }
        return count;
    } catch (const InvalidData&) {
        return std::nullopt;
    }
}

// Unit quaternion with W >= 0. q and -q are the same rotation.
std::optional<DQuat> Normalize(DQuat rotation) {
    if (!Finite(rotation.X) || !Finite(rotation.Y) || !Finite(rotation.Z) || !Finite(rotation.W)) {
        return std::nullopt;
    }
    const double length = std::sqrt(rotation.X * rotation.X + rotation.Y * rotation.Y + rotation.Z * rotation.Z + rotation.W * rotation.W);
    if (!(length > 1e-8)) {
        return std::nullopt;
    }
    rotation.X /= length;
    rotation.Y /= length;
    rotation.Z /= length;
    rotation.W /= length;
    if (rotation.W < 0) {
        rotation.X = -rotation.X;
        rotation.Y = -rotation.Y;
        rotation.Z = -rotation.Z;
        rotation.W = -rotation.W;
    }
    return rotation;
}

double Dot(DQuat left, DQuat right) {
    return left.X * right.X + left.Y * right.Y + left.Z * right.Z + left.W * right.W;
}

DQuat Conjugate(DQuat rotation) {
    rotation.X = -rotation.X;
    rotation.Y = -rotation.Y;
    rotation.Z = -rotation.Z;
    return rotation;
}

// Hamilton product. The bind rotation is applied on the left.
DQuat Multiply(DQuat left, DQuat right) {
    return DQuat{
        left.W * right.X + left.X * right.W + left.Y * right.Z - left.Z * right.Y,
        left.W * right.Y - left.X * right.Z + left.Y * right.W + left.Z * right.X,
        left.W * right.Z + left.X * right.Y - left.Y * right.X + left.Z * right.W,
        left.W * right.W - left.X * right.X - left.Y * right.Y - left.Z * right.Z,
    };
}

// v' = v + w t + cross(q.xyz, t), where t = 2 cross(q.xyz, v).
DVec Rotate(DQuat rotation, DVec vector) {
    const double tx = 2.0 * (rotation.Y * vector.Z - rotation.Z * vector.Y);
    const double ty = 2.0 * (rotation.Z * vector.X - rotation.X * vector.Z);
    const double tz = 2.0 * (rotation.X * vector.Y - rotation.Y * vector.X);
    return DVec{
        vector.X + rotation.W * tx + (rotation.Y * tz - rotation.Z * ty),
        vector.Y + rotation.W * ty + (rotation.Z * tx - rotation.X * tz),
        vector.Z + rotation.W * tz + (rotation.X * ty - rotation.Y * tx),
    };
}

bool Near(double left, double right) {
    return std::abs(left - right) <= kRotationTolerance;
}

// Row-major inverse-bind 3x3. Translation in the 3x4 is ignored.
bool IsRotation(const double matrix[9]) {
    const double columns[3][3] = {
        {matrix[0], matrix[3], matrix[6]},
        {matrix[1], matrix[4], matrix[7]},
        {matrix[2], matrix[5], matrix[8]},
    };
    for (const auto& column : columns) {
        if (!Finite(column[0]) || !Finite(column[1]) || !Finite(column[2])) {
            return false;
        }
        const double length = std::sqrt(column[0] * column[0] + column[1] * column[1] + column[2] * column[2]);
        if (!Near(length, 1.0)) {
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        const int next = (i + 1) % 3;
        const double dot = columns[i][0] * columns[next][0] + columns[i][1] * columns[next][1] + columns[i][2] * columns[next][2];
        if (!Near(dot, 0.0)) {
            return false;
        }
    }
    const double determinant = matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7])
        - matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6])
        + matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
    return Finite(determinant) && Near(determinant, 1.0);
}

// Shepperd's method on the transpose of the inverse-bind rotation.
std::optional<DQuat> BindRotation(const double inverse[9]) {
    const double m00 = inverse[0];
    const double m01 = inverse[3];
    const double m02 = inverse[6];
    const double m10 = inverse[1];
    const double m11 = inverse[4];
    const double m12 = inverse[7];
    const double m20 = inverse[2];
    const double m21 = inverse[5];
    const double m22 = inverse[8];
    const double trace = m00 + m11 + m22;
    DQuat rotation;
    if (trace > 0.0) {
        const double scale = std::sqrt(trace + 1.0) * 2.0;
        rotation.W = 0.25 * scale;
        rotation.X = (m21 - m12) / scale;
        rotation.Y = (m02 - m20) / scale;
        rotation.Z = (m10 - m01) / scale;
    } else if (m00 > m11 && m00 > m22) {
        const double scale = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        rotation.W = (m21 - m12) / scale;
        rotation.X = 0.25 * scale;
        rotation.Y = (m01 + m10) / scale;
        rotation.Z = (m02 + m20) / scale;
    } else if (m11 > m22) {
        const double scale = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        rotation.W = (m02 - m20) / scale;
        rotation.X = (m01 + m10) / scale;
        rotation.Y = 0.25 * scale;
        rotation.Z = (m12 + m21) / scale;
    } else {
        const double scale = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        rotation.W = (m10 - m01) / scale;
        rotation.X = (m02 + m20) / scale;
        rotation.Y = (m12 + m21) / scale;
        rotation.Z = 0.25 * scale;
    }
    return Normalize(rotation);
}

bool ReadInverseBind(const KvValue& pose, double out[9]) {
    std::vector<double> values;
    if (!pose.IsArray()) {
        return false;
    }
    if (pose.Count() == 12) {
        values.resize(12);
        for (size_t i = 0; i < 12; ++i) {
            const std::optional<double> number = Number(pose.At(i));
            if (!number) {
                return false;
            }
            values[i] = *number;
        }
    } else if (pose.Count() == 3) {
        for (size_t row = 0; row < 3; ++row) {
            const KvValue& line = pose.At(row);
            if (!line.IsArray() || line.Count() != 4) {
                return false;
            }
            for (size_t column = 0; column < 4; ++column) {
                const std::optional<double> number = Number(line.At(column));
                if (!number) {
                    return false;
                }
                values.push_back(*number);
            }
        }
    } else {
        return false;
    }
    out[0] = values[0];
    out[1] = values[1];
    out[2] = values[2];
    out[3] = values[4];
    out[4] = values[5];
    out[5] = values[6];
    out[6] = values[8];
    out[7] = values[9];
    out[8] = values[10];
    return true;
}

struct BindPose {
    bool Present = false;
    bool Duplicate = false;
    bool Usable = false;
    DQuat Rotation;
};

BindPose FindBind(const KvValue& root) {
    BindPose bind;
    const KvValue* skeleton = root.IsObject() ? root.Find("m_skeleton") : nullptr;
    const KvValue* bones = skeleton != nullptr && skeleton->IsObject() ? skeleton->Find("m_bones") : nullptr;
    if (bones == nullptr || !bones->IsArray()) {
        return bind;
    }
    for (const KvValue& bone : bones->Items()) {
        const KvValue* name = bone.IsObject() ? bone.Find("m_boneName") : nullptr;
        if (name == nullptr || name->Kind != KvKind::String || name->String() != "root_motion") {
            continue;
        }
        if (bind.Present) {
            bind.Duplicate = true;
            bind.Usable = false;
            return bind;
        }
        bind.Present = true;
        const KvValue* pose = bone.Find("m_invBindPose");
        double matrix[9];
        if (pose == nullptr || !ReadInverseBind(*pose, matrix) || !IsRotation(matrix)) {
            return bind;
        }
        const std::optional<DQuat> rotation = BindRotation(matrix);
        if (!rotation) {
            return bind;
        }
        bind.Rotation = *rotation;
        bind.Usable = true;
    }
    return bind;
}

KvValue* BodyOf(KvValue& item) {
    if (KvValue* value = item.Find("value"); value != nullptr && value->IsObject() && value->Contains("m_influenceNames")) {
        return value;
    }
    return &item;
}

std::optional<std::string> AttachmentName(const KvValue& item) {
    if (const KvValue* value = item.Find("value"); value != nullptr && value->IsObject()) {
        if (const KvValue* name = value->Find("m_name"); name != nullptr && name->Kind == KvKind::String) {
            return name->String();
        }
    }
    if (const KvValue* key = item.Find("key"); key != nullptr && key->Kind == KvKind::String) {
        return key->String();
    }
    if (const KvValue* name = item.Find("m_name"); name != nullptr && name->Kind == KvKind::String) {
        return name->String();
    }
    return std::nullopt;
}

bool IsPivot(const std::optional<std::string>& name) {
    return name && (*name == "standing_pivot" || *name == "crouching_pivot");
}

std::optional<DQuat> ReadQuat(const KvValue& value) {
    if (!value.IsArray() || value.Count() != 4) {
        return std::nullopt;
    }
    DQuat rotation;
    const std::optional<double> x = Number(value.At(0));
    const std::optional<double> y = Number(value.At(1));
    const std::optional<double> z = Number(value.At(2));
    const std::optional<double> w = Number(value.At(3));
    if (!x || !y || !z || !w) {
        return std::nullopt;
    }
    rotation.X = *x;
    rotation.Y = *y;
    rotation.Z = *z;
    rotation.W = *w;
    return Normalize(rotation);
}

std::optional<DVec> ReadVec(const KvValue& value) {
    if (!value.IsArray() || value.Count() != 3) {
        return std::nullopt;
    }
    const std::optional<double> x = Number(value.At(0));
    const std::optional<double> y = Number(value.At(1));
    const std::optional<double> z = Number(value.At(2));
    if (!x || !y || !z) {
        return std::nullopt;
    }
    return DVec{*x, *y, *z};
}

MeshFrame FrameOf(DQuat rotation, DQuat bind) {
    const DQuat identity{0, 0, 0, 1};
    const bool current = std::abs(Dot(rotation, identity)) >= kFrameDot;
    const bool old = std::abs(Dot(rotation, Conjugate(bind))) >= kFrameDot;
    // An identity bind makes the two frames the same rotation. Leave it alone.
    if (current) {
        return MeshFrame::Current;
    }
    if (old) {
        return MeshFrame::Old;
    }
    return MeshFrame::Unrecognized;
}

// Reads one mesh. A flat influence list, a bad root_motion slot, or disagreeing pivots is Unrecognized.
MeshInfo InspectMesh(int block, KvDocument document) {
    MeshInfo info;
    info.Block = block;
    KvValue& root = document.Root;
    KvValue* attachments = root.IsObject() ? root.Find("m_attachments") : nullptr;
    if (attachments == nullptr) {
        return info;
    }
    if (!attachments->IsArray()) {
        info.Frame = MeshFrame::Unrecognized;
        return info;
    }

    const BindPose bind = FindBind(root);
    bool malformed = false;
    bool saw_pivot = false;
    bool standing_seen = false;
    bool crouching_seen = false;
    std::optional<MeshFrame> standing;
    std::optional<MeshFrame> crouching;
    int influences = 0;

    for (size_t index = 0; index < attachments->Count(); ++index) {
        KvValue& item = attachments->At(index);
        if (!item.IsObject()) {
            continue;
        }
        const std::optional<std::string> name = AttachmentName(item);
        const bool pivot = IsPivot(name);
        if (pivot) {
            saw_pivot = true;
        }
        KvValue* body = BodyOf(item);
        const KvValue* names = body->Find("m_influenceNames");
        const KvValue* rotations = body->Find("m_vInfluenceRotations");
        const KvValue* offsets = body->Find("m_vInfluenceOffsets");
        const KvValue* count_value = body->Find("m_nInfluences");
        if (names == nullptr && rotations == nullptr && offsets == nullptr && count_value == nullptr) {
            if (pivot) {
                malformed = true;
            }
            continue;
        }
        const std::optional<int> count = count_value != nullptr ? InfluenceCount(*count_value) : std::nullopt;
        if (!count || names == nullptr || rotations == nullptr || offsets == nullptr || !names->IsArray() || !rotations->IsArray() || !offsets->IsArray()
            || names->Count() < static_cast<size_t>(*count) || rotations->Count() < static_cast<size_t>(*count) || offsets->Count() < static_cast<size_t>(*count)) {
            malformed = true;
            continue;
        }

        std::optional<MeshFrame> pivot_frame;
        bool pivot_on_root = false;
        for (int slot = 0; slot < *count; ++slot) {
            const KvValue& bone = names->At(static_cast<size_t>(slot));
            if (bone.Kind != KvKind::String) {
                malformed = true;
                continue;
            }
            if (bone.String() != "root_motion") {
                continue;
            }
            const std::optional<DQuat> rotation = ReadQuat(rotations->At(static_cast<size_t>(slot)));
            const std::optional<DVec> offset = ReadVec(offsets->At(static_cast<size_t>(slot)));
            if (!rotation || !offset) {
                malformed = true;
                continue;
            }
            ++influences;
            if (pivot && !pivot_on_root) {
                pivot_on_root = true;
                if (bind.Usable && !bind.Duplicate) {
                    pivot_frame = FrameOf(*rotation, bind.Rotation);
                }
            }
        }
        if (pivot_on_root && name && *name == "standing_pivot" && !standing_seen) {
            standing_seen = true;
            standing = pivot_frame;
        }
        if (pivot_on_root && name && *name == "crouching_pivot" && !crouching_seen) {
            crouching_seen = true;
            crouching = pivot_frame;
        }
    }

    if (!standing_seen && !crouching_seen) {
        if (malformed && saw_pivot) {
            info.Frame = MeshFrame::Unrecognized;
        }
        return info;
    }
    const bool disagree = standing && crouching && *standing != *crouching;
    const bool unknown = (standing && *standing == MeshFrame::Unrecognized) || (crouching && *crouching == MeshFrame::Unrecognized);
    if (!bind.Usable || bind.Duplicate || malformed || disagree || unknown || (standing_seen && !standing) || (crouching_seen && !crouching)) {
        info.Frame = MeshFrame::Unrecognized;
        return info;
    }
    info.Frame = standing ? *standing : *crouching;
    if (info.Frame == MeshFrame::Old) {
        info.Influences = influences;
        info.Bind = bind.Rotation;
        info.Document = std::move(document);
    }
    return info;
}

KvValue FloatQuat(DQuat rotation) {
    return KvValue::Array({
        KvValue::Float(static_cast<float>(rotation.X)),
        KvValue::Float(static_cast<float>(rotation.Y)),
        KvValue::Float(static_cast<float>(rotation.Z)),
        KvValue::Float(static_cast<float>(rotation.W)),
    });
}

KvValue FloatVec(DVec vector) {
    return KvValue::Array({
        KvValue::Float(static_cast<float>(vector.X)),
        KvValue::Float(static_cast<float>(vector.Y)),
        KvValue::Float(static_cast<float>(vector.Z)),
    });
}

// q_new = q_bind * q_old, o_new = rotate(q_bind, o_old). Other bones and padding stay put.
std::optional<int> RebaseDocument(KvDocument& document, DQuat bind) {
    KvValue* attachments = document.Root.Find("m_attachments");
    if (attachments == nullptr || !attachments->IsArray()) {
        return std::nullopt;
    }
    int influences = 0;
    for (size_t index = 0; index < attachments->Count(); ++index) {
        KvValue& item = attachments->At(index);
        if (!item.IsObject()) {
            continue;
        }
        KvValue* body = BodyOf(item);
        KvValue* names = body->Find("m_influenceNames");
        KvValue* rotations = body->Find("m_vInfluenceRotations");
        KvValue* offsets = body->Find("m_vInfluenceOffsets");
        const KvValue* count_value = body->Find("m_nInfluences");
        if (names == nullptr || rotations == nullptr || offsets == nullptr || count_value == nullptr) {
            continue;
        }
        const std::optional<int> count = InfluenceCount(*count_value);
        if (!count || !names->IsArray() || !rotations->IsArray() || !offsets->IsArray()) {
            return std::nullopt;
        }
        for (int slot = 0; slot < *count; ++slot) {
            const KvValue& bone = names->At(static_cast<size_t>(slot));
            if (bone.Kind != KvKind::String || bone.String() != "root_motion") {
                continue;
            }
            const std::optional<DQuat> rotation = ReadQuat(rotations->At(static_cast<size_t>(slot)));
            const std::optional<DVec> offset = ReadVec(offsets->At(static_cast<size_t>(slot)));
            if (!rotation || !offset) {
                return std::nullopt;
            }
            const std::optional<DQuat> turned = Normalize(Multiply(bind, *rotation));
            if (!turned) {
                return std::nullopt;
            }
            rotations->At(static_cast<size_t>(slot)) = FloatQuat(*turned);
            offsets->At(static_cast<size_t>(slot)) = FloatVec(Rotate(bind, *offset));
            ++influences;
        }
    }
    return influences;
}

std::optional<std::vector<BlockRef>> ListBlocks(std::span<const uint8_t> file) {
    if (file.size() < 16 || ReadPod<uint16_t>(file, 4) != 12) {
        return std::nullopt;
    }
    const auto block_offset = ReadPod<uint32_t>(file, 8);
    const auto block_count = ReadPod<int32_t>(file, 12);
    if (block_count < 0 || block_offset < 8) {
        return std::nullopt;
    }
    const int64_t table_start = 8LL + block_offset;
    const int64_t table_bytes = static_cast<int64_t>(block_count) * 12;
    if (table_start > static_cast<int64_t>(file.size()) || table_bytes > static_cast<int64_t>(file.size()) - table_start) {
        return std::nullopt;
    }
    std::vector<BlockRef> blocks;
    blocks.reserve(static_cast<size_t>(block_count));
    for (int i = 0; i < block_count; ++i) {
        const int pos = static_cast<int>(table_start) + i * 12;
        BlockRef block;
        block.Index = i;
        std::memcpy(block.Type, file.data() + pos, 4);
        const auto relative = ReadPod<uint32_t>(file, static_cast<size_t>(pos + 4));
        const auto size = ReadPod<int32_t>(file, static_cast<size_t>(pos + 8));
        const int64_t absolute = static_cast<int64_t>(pos) + 4 + relative;
        if (size < 0 || absolute < 0 || absolute > static_cast<int64_t>(file.size())
            || static_cast<int64_t>(size) > static_cast<int64_t>(file.size()) - absolute) {
            return std::nullopt;
        }
        block.Absolute = static_cast<int>(absolute);
        block.Size = size;
        blocks.push_back(block);
    }
    return blocks;
}

bool IsMdat(const BlockRef& block) {
    return block.Type[0] == 'M' && block.Type[1] == 'D' && block.Type[2] == 'A' && block.Type[3] == 'T';
}

const char* kUnrecognized =
    "A camera pivot on root_motion does not match the old or the current frame, so those attachments were left unchanged.";

const char* kUnreadable =
    "A camera attachment on root_motion could not be read, so those attachments were left unchanged.";

std::string CountPhrase(int count, const char* singular, const char* plural) {
    return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

std::string OldDetail(int meshes, int influences) {
    return "Camera attachments on root_motion use the frame from before October 16, 2026, so the camera stays behind while the hero walks. "
        "The repair moves " + CountPhrase(influences, "influence", "influences") + " on " + CountPhrase(meshes, "mesh", "meshes")
        + " into the current frame. The mod's own offsets are kept and expressed in the current root_motion frame.";
}

CameraAttachmentEdit UnrecognizedEdit(const char* detail) {
    CameraAttachmentEdit edit;
    edit.Frame = CameraFrame::Unrecognized;
    edit.Detail = detail;
    return edit;
}

CameraAttachmentEdit Run(std::span<const uint8_t> model, bool write) {
    const std::optional<std::vector<BlockRef>> blocks = ListBlocks(model);
    if (!blocks) {
        if (model.size() >= 16 && ReadPod<uint16_t>(model, 4) == 12) {
            return UnrecognizedEdit(kUnreadable);
        }
        CameraAttachmentEdit edit;
        return edit;
    }

    bool any_old = false;
    bool any_current = false;
    bool any_unrecognized = false;
    int meshes = 0;
    int influences = 0;
    std::vector<MeshInfo> old_meshes;
    for (const BlockRef& block : *blocks) {
        if (!IsMdat(block) || block.Size < 4 || !IsBinaryKv3(ReadPod<uint32_t>(model, static_cast<size_t>(block.Absolute)))) {
            continue;
        }
        MeshInfo info;
        try {
            const KvDocument document = ReadKv3(model.subspan(static_cast<size_t>(block.Absolute), static_cast<size_t>(block.Size)));
            if (!document.Root.IsObject() || !document.Root.Contains("m_attachments")) {
                continue;
            }
            info = InspectMesh(block.Index, document);
        } catch (const std::exception&) {
            info.Frame = MeshFrame::Unrecognized;
        }
        if (info.Frame == MeshFrame::Old) {
            any_old = true;
            meshes += 1;
            influences += info.Influences;
            old_meshes.push_back(std::move(info));
        } else if (info.Frame == MeshFrame::Unrecognized) {
            any_unrecognized = true;
        } else if (info.Frame == MeshFrame::Current) {
            any_current = true;
        }
    }

    CameraAttachmentEdit edit;
    if (any_old) {
        edit.Frame = CameraFrame::Old;
        edit.Meshes = meshes;
        edit.Influences = influences;
        edit.Detail = OldDetail(meshes, influences);
        if (any_unrecognized) {
            edit.Warning = kUnrecognized;
        }
    } else if (any_unrecognized) {
        edit.Frame = CameraFrame::Unrecognized;
        edit.Detail = kUnrecognized;
        return edit;
    } else if (any_current) {
        edit.Frame = CameraFrame::Current;
        return edit;
    } else {
        return edit;
    }

    if (!write || edit.Frame != CameraFrame::Old) {
        return edit;
    }

    struct Replacement {
        int Block = 0;
        std::vector<uint8_t> Bytes;
    };
    std::vector<Replacement> replacements;
    int written = 0;
    for (MeshInfo& mesh : old_meshes) {
        if (!mesh.Document) {
            return UnrecognizedEdit(kUnreadable);
        }
        const std::optional<int> count = RebaseDocument(*mesh.Document, mesh.Bind);
        if (!count) {
            return UnrecognizedEdit(kUnreadable);
        }
        written += *count;
        try {
            replacements.push_back(Replacement{mesh.Block, WriteKv3(*mesh.Document)});
        } catch (const std::exception&) {
            return UnrecognizedEdit(kUnreadable);
        }
    }
    edit.Influences = written;
    edit.Detail = OldDetail(meshes, written);

    std::sort(replacements.begin(), replacements.end(), [](const Replacement& left, const Replacement& right) {
        return left.Block > right.Block;
    });
    try {
        std::vector<uint8_t> bytes(model.begin(), model.end());
        for (const Replacement& replacement : replacements) {
            bytes = ReplaceResourceBlock(bytes, replacement.Block, replacement.Bytes);
        }
        edit.Bytes = std::move(bytes);
    } catch (const std::exception&) {
        return UnrecognizedEdit(kUnreadable);
    }
    return edit;
}

}  // namespace

CameraAttachmentEdit CameraAttachmentRepair::Inspect(std::span<const uint8_t> model) {
    try {
        return Run(model, false);
    } catch (const std::exception&) {
        return UnrecognizedEdit(kUnreadable);
    }
}

CameraAttachmentEdit CameraAttachmentRepair::Apply(std::span<const uint8_t> model) {
    try {
        return Run(model, true);
    } catch (const std::exception&) {
        return UnrecognizedEdit(kUnreadable);
    }
}

}  // namespace skeleton_repair

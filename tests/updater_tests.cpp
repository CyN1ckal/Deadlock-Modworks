#include "command_line.hpp"
#include "product_version.hpp"
#include "kv3.hpp"
#include "report.hpp"
#include "resource.hpp"
#include "strings.hpp"
#include "updater.hpp"
#include "vpk.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using skeleton_repair::ArchiveReport;
using skeleton_repair::CancellationSource;
using skeleton_repair::CommandLine;
using skeleton_repair::EqualsIgnoreCase;
using skeleton_repair::HeroModelUpdater;
using skeleton_repair::InvalidOperation;
using skeleton_repair::IoError;
using skeleton_repair::KvDocument;
using skeleton_repair::KvValue;
using skeleton_repair::ModelOutcome;
using skeleton_repair::ModelResult;
using skeleton_repair::Narrow;
using skeleton_repair::ReportExporter;
using skeleton_repair::UpdateReport;
using skeleton_repair::VpkArchive;
using skeleton_repair::VpkChange;
using skeleton_repair::Widen;

constexpr const char* kModel = "models/heroes/test/test.vmdl_c";
constexpr const char* kSkeletonId = "models/heroes/test/test.vnmskel";
constexpr const char* kSkeleton = "models/heroes/test/test.vnmskel_c";

class TempDir {
public:
    TempDir() {
        static int sequence = 0;
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = fs::temp_directory_path() / ("dsr-up-" + std::to_string(++sequence) + "-" + std::to_string(ticks));
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

std::string Utf8(const fs::path& path) {
    return Narrow(path.wstring());
}

fs::path FromUtf8(const std::string& text) {
    return fs::path(Widen(text));
}

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output.good()) {
        throw std::runtime_error("Could not write " + Utf8(path));
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!output.good()) {
        throw std::runtime_error("Could not write " + Utf8(path));
    }
}

std::vector<uint8_t> ReadBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.good()) << Utf8(path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void WriteU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteI32(std::vector<uint8_t>& bytes, size_t offset, int32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void Archive(const fs::path& path, const std::vector<VpkChange>& changes) {
    std::vector<uint8_t> seed;
    AppendU32(seed, 0x55AA1234u);
    AppendU32(seed, 2u);
    AppendU32(seed, 1u);
    AppendU32(seed, 0u);
    seed.resize(28, 0);
    seed.push_back(0);
    WriteBytes(path, seed);
    VpkArchive::Open(path).Write(path, changes);
}

std::vector<uint8_t> RerlBlock(const std::vector<std::string>& names) {
    std::vector<uint8_t> block;
    const auto count = static_cast<uint32_t>(names.size());
    AppendU32(block, 8);
    AppendU32(block, count);
    std::string strings;
    std::vector<int32_t> offsets;
    size_t cursor = 8 + static_cast<size_t>(count) * 16;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t name_field = 16 + static_cast<size_t>(i) * 16;
        if (names[i].empty()) {
            offsets.push_back(0);
            continue;
        }
        offsets.push_back(static_cast<int32_t>(cursor - name_field));
        strings += names[i];
        strings.push_back('\0');
        cursor += names[i].size() + 1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        AppendU32(block, 0);
        AppendU32(block, 0);
        const size_t offset = block.size();
        block.resize(offset + 4);
        WriteI32(block, offset, offsets[i]);
        AppendU32(block, 0);
    }
    block.insert(block.end(), strings.begin(), strings.end());
    return block;
}

std::vector<uint8_t> Wrap(const std::vector<uint8_t>& data, const std::vector<std::string>& references) {
    if (references.empty()) {
        const size_t start = 28;
        std::vector<uint8_t> file(start + data.size());
        WriteU32(file, 0, static_cast<uint32_t>(file.size()));
        WriteU16(file, 4, 12);
        WriteU16(file, 6, 1);
        WriteU32(file, 8, 8);
        WriteI32(file, 12, 1);
        file[16] = 'D';
        file[17] = 'A';
        file[18] = 'T';
        file[19] = 'A';
        WriteU32(file, 20, static_cast<uint32_t>(start - 20));
        WriteI32(file, 24, static_cast<int32_t>(data.size()));
        std::memcpy(file.data() + start, data.data(), data.size());
        return file;
    }

    const std::vector<uint8_t> rerl = RerlBlock(references);
    const size_t start = 40;
    std::vector<uint8_t> file(start + data.size() + rerl.size());
    WriteU32(file, 0, static_cast<uint32_t>(file.size()));
    WriteU16(file, 4, 12);
    WriteU16(file, 6, 1);
    WriteU32(file, 8, 8);
    WriteI32(file, 12, 2);
    file[16] = 'D';
    file[17] = 'A';
    file[18] = 'T';
    file[19] = 'A';
    WriteU32(file, 20, 20);
    WriteI32(file, 24, static_cast<int32_t>(data.size()));
    file[28] = 'R';
    file[29] = 'E';
    file[30] = 'R';
    file[31] = 'L';
    WriteU32(file, 32, static_cast<uint32_t>(8 + data.size()));
    WriteI32(file, 36, static_cast<int32_t>(rerl.size()));
    std::memcpy(file.data() + start, data.data(), data.size());
    std::memcpy(file.data() + start + data.size(), rerl.data(), rerl.size());
    return file;
}

KvValue Strings(const std::vector<std::string>& values) {
    std::vector<KvValue> items;
    for (const std::string& value : values) {
        items.push_back(KvValue::String(value));
    }
    return KvValue::Array(std::move(items));
}

KvValue Ints(const std::vector<int>& values) {
    std::vector<KvValue> items;
    for (int value : values) {
        items.push_back(KvValue::Int32(value));
    }
    return KvValue::Array(std::move(items));
}

struct ModelSpec {
    std::vector<std::string> Names;
    std::vector<int> Parents;
    std::optional<std::vector<int>> Remap;
    float Pose = 0;
    std::string SkeletonRef;
    std::vector<std::string> AnimGroups;
    std::vector<std::string> Physics;
    std::vector<std::string> Graphs;
    std::vector<std::string> References;
};

std::vector<uint8_t> Model(const ModelSpec& spec) {
    std::vector<KvValue> positions;
    std::vector<KvValue> rotations;
    std::vector<KvValue> spheres;
    std::vector<KvValue> scales;
    for (size_t i = 0; i < spec.Names.size(); ++i) {
        positions.push_back(KvValue::Array({KvValue::Float(spec.Pose), KvValue::Float(0), KvValue::Float(0)}));
        rotations.push_back(KvValue::Array({KvValue::Float(0), KvValue::Float(0), KvValue::Float(0), KvValue::Float(1)}));
        spheres.push_back(KvValue::Float(1));
        scales.push_back(KvValue::Float(1));
    }
    KvValue skeleton = KvValue::Object({});
    skeleton.Set("m_boneName", Strings(spec.Names));
    skeleton.Set("m_nParent", Ints(spec.Parents));
    skeleton.Set("m_boneSphere", KvValue::Array(std::move(spheres)));
    skeleton.Set("m_nFlag", Ints(std::vector<int>(spec.Names.size(), 0)));
    skeleton.Set("m_bonePosParent", KvValue::Array(std::move(positions)));
    skeleton.Set("m_boneRotParent", KvValue::Array(std::move(rotations)));
    skeleton.Set("m_boneScaleParent", KvValue::Array(std::move(scales)));
    KvValue root = KvValue::Object({});
    root.Set("m_name", KvValue::String("models/heroes/test/test.vmdl"));
    root.Set("m_modelSkeleton", std::move(skeleton));
    if (!spec.SkeletonRef.empty()) {
        root.Set("m_vecNmSkeletonRefs", KvValue::Array({KvValue::String(spec.SkeletonRef)}));
    }
    if (!spec.AnimGroups.empty()) {
        root.Set("m_refAnimGroups", Strings(spec.AnimGroups));
    }
    if (!spec.Physics.empty()) {
        root.Set("m_refPhysicsData", Strings(spec.Physics));
    }
    if (!spec.Graphs.empty()) {
        std::vector<KvValue> graphs;
        for (const std::string& graph : spec.Graphs) {
            KvValue item = KvValue::Object({});
            item.Set("m_hGraph", KvValue::String(graph));
            graphs.push_back(std::move(item));
        }
        root.Set("m_animGraph2Refs", KvValue::Array(std::move(graphs)));
    }
    if (spec.Remap.has_value()) {
        root.Set("m_remappingTable", Ints(*spec.Remap));
    }
    skeleton_repair::KvDocument document;
    document.Root = std::move(root);
    return Wrap(skeleton_repair::WriteKv3(document), spec.References);
}

KvValue Pose(float x) {
    return KvValue::Array({
        KvValue::Float(x), KvValue::Float(0), KvValue::Float(0), KvValue::Float(1),
        KvValue::Float(0), KvValue::Float(0), KvValue::Float(0), KvValue::Float(1),
    });
}

struct AnimSpec {
    std::vector<std::string> Names;
    std::vector<int> Parents;
    int Lod = 1;
    float PoseX = 0;
    std::string MaskLabel = "custom-mask";
    std::string OtherMaskSkeleton;
};

std::vector<uint8_t> Animation(const AnimSpec& spec) {
    KvValue root = KvValue::Object({});
    root.Set("m_ID", KvValue::String(kSkeletonId));
    root.Set("m_boneIDs", Strings(spec.Names));
    root.Set("m_parentIndices", Ints(spec.Parents));
    std::vector<KvValue> poses;
    for (size_t i = 0; i < spec.Names.size(); ++i) {
        poses.push_back(Pose(spec.PoseX));
    }
    root.Set("m_parentSpaceReferencePose", KvValue::Array(poses));
    root.Set("m_modelSpaceReferencePose", KvValue::Array(poses));
    root.Set("m_numBonesToSampleAtLowLOD", KvValue::Int32(spec.Lod));
    KvValue weights = KvValue::Object({});
    weights.Set("m_skeletonName", KvValue::String(kSkeletonId));
    weights.Set("m_boneIDs", Strings(spec.Names));
    weights.Set("m_weights", KvValue::Array(std::vector<KvValue>(spec.Names.size(), KvValue::Float(0.5f))));
    KvValue mask = KvValue::Object({});
    mask.Set("m_ID", KvValue::String(spec.MaskLabel));
    mask.Set("m_authoredValue", KvValue::Float(0.75f));
    mask.Set("m_primaryWeightList", std::move(weights));
    if (!spec.OtherMaskSkeleton.empty()) {
        KvValue other = KvValue::Object({});
        other.Set("m_skeletonName", KvValue::String(spec.OtherMaskSkeleton));
        other.Set("m_boneIDs", Strings(spec.Names));
        other.Set("m_weights", KvValue::Array(std::vector<KvValue>(spec.Names.size(), KvValue::Float(0.25f))));
        mask.Set("m_secondaryWeightList", std::move(other));
    }
    root.Set("m_maskDefinitions", KvValue::Array({std::move(mask)}));
    root.Set("m_secondarySkeletons", KvValue::Array({}));
    root.Set("m_bIsPropSkeleton", KvValue::Bool(false));
    skeleton_repair::KvDocument document;
    document.Root = std::move(root);
    return Wrap(skeleton_repair::WriteKv3(document), {});
}

const ModelResult* ResultFor(const UpdateReport& report, std::string_view model) {
    for (const ModelResult& result : report.Results) {
        if (result.Model == model) {
            return &result;
        }
    }
    return nullptr;
}

KvDocument DataOf(const std::vector<uint8_t>& bytes) {
    return skeleton_repair::ReadKv3(skeleton_repair::FindDataBlock(bytes));
}

std::vector<uint8_t> EntryBytes(const VpkArchive& archive, std::string_view path) {
    for (const auto& entry : archive.Entries()) {
        if (entry.FullPath == path) {
            return archive.Read(entry);
        }
    }
    ADD_FAILURE() << path;
    return {};
}

struct CliOutput {
    int Code = 0;
    std::string Text;
};

CliOutput Execute(const std::vector<std::string>& args, skeleton_repair::CancellationToken token = {}) {
    CliOutput run;
    run.Code = CommandLine::Execute(args, [&](std::string_view line) {
        run.Text.append(line);
        run.Text.push_back('\n');
    }, token);
    return run;
}

const std::vector<uint8_t> kGameModel = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}});
const std::vector<uint8_t> kOldModel = Model({.Names = {"arm", "root"}, .Parents = {1, -1}, .Remap = std::vector<int>{0, 1}});

struct Pair {
    TempDir dir;
    fs::path game_dir;
    std::string game;
    fs::path mod_path;

    explicit Pair(const std::vector<VpkChange>& mod, const std::vector<VpkChange>& game_entries = {{kModel, kGameModel}})
        : game_dir(dir.Path() / "game"), game(Utf8(game_dir)), mod_path(dir.Path() / "mod.vpk") {
        Archive(game_dir / "pak01_dir.vpk", game_entries);
        Archive(mod_path, mod);
    }
};

template <typename Exception>
void ExpectError(const std::function<void()>& call, const char* message) {
    try {
        call();
        FAIL() << "expected an exception";
    } catch (const Exception& ex) {
        EXPECT_STREQ(ex.what(), message);
    } catch (const std::exception& ex) {
        FAIL() << ex.what();
    }
}

}  // namespace

TEST(Updater, AnalyzeCanCheckOneModel) {
    constexpr const char* kOther = "models/heroes/other/other.vmdl_c";
    constexpr const char* kOtherSkeleton = "models/heroes/other/other.vnmskel_c";
    const auto stock = Animation({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Lod = 1});
    Pair pair(
        {{kModel, kOldModel}, {kSkeleton, stock}, {kOther, kOldModel}, {kOtherSkeleton, stock},
            {"models/props/crate.vmdl_c", std::vector<uint8_t>{1, 2, 3}}},
        {{kModel, kGameModel}, {kSkeleton, stock}, {kOther, kGameModel}, {kOtherSkeleton, stock}});
    const std::string mod = Utf8(pair.mod_path);

    const UpdateReport all = HeroModelUpdater::Analyze(pair.game, mod);
    EXPECT_NE(ResultFor(all, kModel), nullptr);
    EXPECT_NE(ResultFor(all, kOther), nullptr);
    EXPECT_EQ(ResultFor(all, "models/props/crate.vmdl_c"), nullptr);

    const auto before = ReadBytes(pair.mod_path);
    const UpdateReport one = HeroModelUpdater::Analyze(pair.game, mod, {}, {}, kModel);
    EXPECT_FALSE(one.WroteFile);
    EXPECT_EQ(ReadBytes(pair.mod_path), before);
    ASSERT_NE(ResultFor(one, kModel), nullptr);
    EXPECT_EQ(ResultFor(one, kModel)->Outcome, ModelOutcome::Repairable);
    EXPECT_NE(ResultFor(one, kSkeleton), nullptr);
    EXPECT_EQ(ResultFor(one, kOther), nullptr);
    EXPECT_EQ(ResultFor(one, kOtherSkeleton), nullptr);
    EXPECT_EQ(one.Results.size(), 2u);

    const UpdateReport missing = HeroModelUpdater::Analyze(pair.game, mod, {}, {}, "models/heroes/test/absent.vmdl_c");
    ASSERT_EQ(missing.Results.size(), 1u);
    EXPECT_EQ(missing.Results[0].Model, "models/heroes/test/absent.vmdl_c");
    EXPECT_EQ(missing.Results[0].Outcome, ModelOutcome::Skipped);
    EXPECT_EQ(missing.Results[0].Detail, "That model is not in this archive.");

    const UpdateReport prop = HeroModelUpdater::Analyze(pair.game, mod, {}, {}, "models/props/crate.vmdl_c");
    ASSERT_EQ(prop.Results.size(), 1u);
    EXPECT_EQ(prop.Results[0].Model, "models/props/crate.vmdl_c");
    EXPECT_EQ(prop.Results[0].Outcome, ModelOutcome::Skipped);
    EXPECT_EQ(prop.Results[0].Detail, "That model is not a supported hero model in this archive.");

    ExpectError<std::invalid_argument>([&] { HeroModelUpdater::Analyze(pair.game, mod, {}, {}, "   "); }, "Model path is required.");
}

TEST(Updater, UpdateRepairsOneModelAndLeavesTheOther) {
    constexpr const char* kOther = "models/heroes/other/other.vmdl_c";
    Pair pair(
        {{kModel, kOldModel}, {kOther, kOldModel}, {"materials/custom.txt", std::vector<uint8_t>{9}}},
        {{kModel, kGameModel}, {kOther, kGameModel}});
    const UpdateReport report = HeroModelUpdater::Update(pair.game, Utf8(pair.mod_path), std::nullopt, true, true, {}, {}, false, nullptr, kModel);
    EXPECT_TRUE(report.WroteFile);
    ASSERT_NE(ResultFor(report, kModel), nullptr);
    EXPECT_EQ(ResultFor(report, kModel)->Outcome, ModelOutcome::Updated);
    EXPECT_EQ(ResultFor(report, kOther), nullptr);

    const VpkArchive archive = VpkArchive::Open(pair.mod_path);
    EXPECT_NE(EntryBytes(archive, kModel), kOldModel);
    EXPECT_EQ(EntryBytes(archive, kOther), kOldModel);
    EXPECT_EQ(EntryBytes(archive, "materials/custom.txt"), (std::vector<uint8_t>{9}));
    EXPECT_EQ(HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path), {}, {}, kModel).Results.at(0).Outcome, ModelOutcome::Current);
    EXPECT_EQ(HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path), {}, {}, kOther).Results.at(0).Outcome, ModelOutcome::Repairable);
}

TEST(Updater, AnalyzeDetectsReorderWithoutWriting) {
    Pair pair({{kModel, kOldModel}, {"materials/custom.txt", std::vector<uint8_t>{9, 8, 7}}});
    const auto original = ReadBytes(pair.mod_path);
    const UpdateReport report = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    EXPECT_TRUE(report.CanRepair());
    EXPECT_FALSE(report.WroteFile);
    EXPECT_EQ(ReadBytes(pair.mod_path), original);
    ASSERT_EQ(report.Results.size(), 1u);
    EXPECT_EQ(report.Results[0].Outcome, ModelOutcome::Repairable);
}

TEST(Updater, UpdateWritesACompatibleModelAndPreservesOtherAssets) {
    Pair pair({{kModel, kOldModel}, {"materials/custom.txt", std::vector<uint8_t>{9, 8, 7}}});
    const auto original = ReadBytes(pair.mod_path);
    const fs::path output = pair.dir.Path() / "repaired.vpk";
    const UpdateReport report = HeroModelUpdater::Update(pair.game, Utf8(pair.mod_path), Utf8(output), false, false);
    EXPECT_TRUE(report.WroteFile);
    ASSERT_EQ(report.Results.size(), 1u);
    EXPECT_EQ(report.Results[0].Outcome, ModelOutcome::Updated);
    EXPECT_EQ(ReadBytes(pair.mod_path), original);

    const VpkArchive archive = VpkArchive::Open(output);
    archive.Validate();
    EXPECT_EQ(EntryBytes(archive, "materials/custom.txt"), (std::vector<uint8_t>{9, 8, 7}));
    const KvDocument data = DataOf(EntryBytes(archive, kModel));
    const KvValue* remap = data.Root.Find("m_remappingTable");
    ASSERT_NE(remap, nullptr);
    EXPECT_EQ(remap->At(0).ToInt32(), 1);
    EXPECT_EQ(remap->At(1).ToInt32(), 0);
    EXPECT_EQ(HeroModelUpdater::Analyze(pair.game, Utf8(output)).Results.at(0).Outcome, ModelOutcome::Current);
}

TEST(Updater, ReplacementCreatesAnExactOriginalBackup) {
    Pair pair({{kModel, kOldModel}});
    const auto original = ReadBytes(pair.mod_path);
    const UpdateReport report = HeroModelUpdater::Update(pair.game, Utf8(pair.mod_path), std::nullopt, true, true);
    EXPECT_TRUE(report.WroteFile);
    ASSERT_TRUE(report.BackupPath.has_value());
    EXPECT_EQ(ReadBytes(FromUtf8(*report.BackupPath)), original);
    EXPECT_EQ(HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path)).Results.at(0).Outcome, ModelOutcome::Current);
}

TEST(Updater, OneInvalidModelBlocksThePackage) {
    const auto game_model = kGameModel;
    Pair pair(
        {{kModel, kOldModel}, {"models/heroes/test/broken.vmdl_c", std::vector<uint8_t>{1, 2, 3}}},
        {{kModel, game_model}, {"models/heroes/test/broken.vmdl_c", game_model}});
    const auto before = ReadBytes(pair.mod_path);
    const UpdateReport report = HeroModelUpdater::Update(pair.game, Utf8(pair.mod_path), std::nullopt, true, true);
    EXPECT_TRUE(report.HasFailures());
    EXPECT_FALSE(report.WroteFile);
    EXPECT_EQ(ReadBytes(pair.mod_path), before);
}

TEST(Updater, CyclicSkeletonsFail) {
    Pair pair({{kModel, Model({.Names = {"root", "arm"}, .Parents = {1, 0}, .Remap = std::vector<int>{0, 1}})}});
    const UpdateReport report = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    EXPECT_TRUE(report.HasFailures());
    ASSERT_EQ(report.Results.size(), 1u);
    EXPECT_NE(report.Results[0].Detail.find("cycle"), std::string::npos);
}

TEST(Updater, OutOfRangeRemapFailsWhenBoneOrderMatches) {
    Pair pair({{kModel, Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{2}})}});
    EXPECT_TRUE(HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path)).HasFailures());
}

TEST(Updater, MissingRemapBlocksAReorder) {
    Pair pair({{kModel, Model({.Names = {"arm", "root"}, .Parents = {1, -1}})}});
    const UpdateReport report = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    EXPECT_TRUE(report.HasBlockers());
    EXPECT_EQ(report.Results.at(0).Outcome, ModelOutcome::NeedsReview);
}

TEST(Updater, AdditiveSkeletonRepairPreservesSkinning) {
    Pair pair({{kModel, Model({.Names = {"root"}, .Parents = {-1}, .Remap = std::vector<int>{0}})}});
    const UpdateReport report = HeroModelUpdater::Update(pair.game, Utf8(pair.mod_path), std::nullopt, true, true);
    EXPECT_TRUE(report.WroteFile);
    EXPECT_EQ(HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path)).Results.at(0).Outcome, ModelOutcome::Current);
}

TEST(Updater, WorkingAg2RigsStayUnchanged) {
    const auto game_model = Model({
        .Names = {"root", "arm", "stock_cloth"}, .Parents = {-1, 0, 0}, .Remap = std::vector<int>{0, 1, 2}, .SkeletonRef = kSkeletonId});
    const auto custom = Model({
        .Names = {"arm", "root", "custom_helper"}, .Parents = {1, -1, 1}, .Remap = std::vector<int>{0, 1, 2},
        .Pose = 5, .SkeletonRef = kSkeletonId});
    const auto stock = Animation({.Names = {"root", "arm", "attachWorld"}, .Parents = {-1, 0, 0}, .Lod = 2});
    Pair pair({{kModel, custom}}, {{kModel, game_model}, {kSkeleton, stock}});
    const auto before = ReadBytes(pair.mod_path);
    const UpdateReport report = HeroModelUpdater::Update(pair.game, Utf8(pair.mod_path), std::nullopt, true, true);
    EXPECT_FALSE(report.HasBlockers());
    EXPECT_FALSE(report.WroteFile);
    ASSERT_EQ(report.Results.size(), 1u);
    EXPECT_EQ(report.Results[0].Outcome, ModelOutcome::Current);
    ASSERT_TRUE(report.Results[0].Warnings.has_value());
    EXPECT_FALSE(report.Results[0].Warnings->empty());
    EXPECT_EQ(ReadBytes(pair.mod_path), before);

    const CliOutput run = Execute({"--analyze", pair.game, Utf8(pair.mod_path)});
    EXPECT_EQ(run.Code, 0);
    EXPECT_NE(run.Text.find("\n  Note: "), std::string::npos);
}

TEST(Updater, MissingAg2SkeletonBlocksRepair) {
    const auto game_model = Model({
        .Names = {"root", "arm", "stock_cloth"}, .Parents = {-1, 0, 0}, .Remap = std::vector<int>{0, 1, 2}, .SkeletonRef = kSkeletonId});
    const auto stock = Animation({.Names = {"root", "arm", "attachWorld"}, .Parents = {-1, 0, 0}, .Lod = 2});
    const auto missing = Model({
        .Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1},
        .SkeletonRef = "models/heroes/test/missing.vnmskel"});
    Pair pair({{kModel, missing}}, {{kModel, game_model}, {kSkeleton, stock}});
    const UpdateReport report = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    EXPECT_TRUE(report.HasBlockers());
    EXPECT_NE(report.Results.at(0).Detail.find("models/heroes/test/missing.vnmskel"), std::string::npos);
    EXPECT_EQ(report.Results.at(0).Outcome, ModelOutcome::NeedsReview);
}

TEST(Updater, OutdatedAnimationIsRepairableWithoutRewritingTheModel) {
    const auto game_model = Model({
        .Names = {"root", "arm", "stock_cloth"}, .Parents = {-1, 0, 0}, .Remap = std::vector<int>{0, 1, 2}, .SkeletonRef = kSkeletonId});
    const auto custom = Model({
        .Names = {"arm", "root", "custom_helper"}, .Parents = {1, -1, 1}, .Remap = std::vector<int>{0, 1, 2},
        .Pose = 5, .SkeletonRef = kSkeletonId});
    const auto stock = Animation({.Names = {"root", "arm", "attachWorld"}, .Parents = {-1, 0, 0}, .Lod = 2});
    const auto old = Animation({.Names = {"arm", "root"}, .Parents = {1, -1}, .Lod = 2, .PoseX = 7});
    Pair pair({{kModel, custom}, {kSkeleton, old}}, {{kModel, game_model}, {kSkeleton, stock}});

    const UpdateReport analysis = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    EXPECT_TRUE(analysis.CanRepair());
    const ModelResult* model = ResultFor(analysis, kModel);
    const ModelResult* skeleton = ResultFor(analysis, kSkeleton);
    ASSERT_NE(model, nullptr);
    ASSERT_NE(skeleton, nullptr);
    EXPECT_EQ(model->Outcome, ModelOutcome::Current);
    EXPECT_EQ(skeleton->Outcome, ModelOutcome::Repairable);
    EXPECT_FALSE(model->BoneRenames.has_value());
    ASSERT_TRUE(skeleton->BoneRenames.has_value());
    EXPECT_TRUE(skeleton->BoneRenames->empty());

    const auto json = nlohmann::ordered_json::parse(ReportExporter::Serialize(pair.game, {ArchiveReport{Utf8(pair.mod_path), analysis}}));
    const auto& results = json["Archives"][0]["Report"]["Results"];
    const auto& model_json = results[0]["Model"] == kModel ? results[0] : results[1];
    const auto& skeleton_json = results[0]["Model"] == kSkeleton ? results[0] : results[1];
    EXPECT_TRUE(model_json["BoneRenames"].is_null());
    EXPECT_TRUE(skeleton_json["BoneRenames"].is_object());
    EXPECT_TRUE(skeleton_json["BoneRenames"].empty());

    const UpdateReport repaired = HeroModelUpdater::Update(pair.game, Utf8(pair.mod_path), std::nullopt, true, true);
    EXPECT_TRUE(repaired.WroteFile);
    const VpkArchive archive = VpkArchive::Open(pair.mod_path);
    EXPECT_EQ(EntryBytes(archive, kModel), custom);
    const KvDocument data = DataOf(EntryBytes(archive, kSkeleton));
    EXPECT_EQ(data.Root.Find("m_boneIDs")->At(0).String(), "root");
    EXPECT_EQ(data.Root.Find("m_boneIDs")->Count(), 3u);
    EXPECT_FLOAT_EQ(data.Root.Find("m_parentSpaceReferencePose")->At(0).At(0).Float(), 7.0f);
    EXPECT_FLOAT_EQ(data.Root.Find("m_parentSpaceReferencePose")->At(1).At(0).Float(), 7.0f);
    EXPECT_FLOAT_EQ(data.Root.Find("m_maskDefinitions")->At(0).Find("m_authoredValue")->Float(), 0.75f);
    EXPECT_FALSE(HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path)).CanRepair());
}

TEST(Updater, InvalidAnimationIndicesBlockTheVpk) {
    const auto custom = Model({
        .Names = {"arm", "root", "custom_helper"}, .Parents = {1, -1, 1}, .Remap = std::vector<int>{0, 1, 2}, .SkeletonRef = kSkeletonId});
    const auto stock = Animation({.Names = {"root", "arm", "attachWorld"}, .Parents = {-1, 0, 0}, .Lod = 2});
    const auto bad = Animation({.Names = {"root", "arm"}, .Parents = {-1, 8}, .Lod = 2});
    Pair pair({{kModel, custom}, {kSkeleton, bad}}, {{kModel, custom}, {kSkeleton, stock}});
    EXPECT_TRUE(HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path)).HasFailures());
}

TEST(Updater, CustomAnimationBonesNeedReview) {
    const auto custom = Model({
        .Names = {"arm", "root", "custom_helper"}, .Parents = {1, -1, 1}, .Remap = std::vector<int>{0, 1, 2}, .SkeletonRef = kSkeletonId});
    const auto stock = Animation({.Names = {"root", "arm", "attachWorld"}, .Parents = {-1, 0, 0}, .Lod = 2});
    const auto custom_animation = Animation({.Names = {"root", "custom_helper"}, .Parents = {-1, 0}, .Lod = 2});
    Pair pair({{kModel, custom}, {kSkeleton, custom_animation}}, {{kModel, custom}, {kSkeleton, stock}});
    const UpdateReport report = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    EXPECT_TRUE(report.HasBlockers());
    ASSERT_NE(ResultFor(report, kSkeleton), nullptr);
    EXPECT_EQ(ResultFor(report, kSkeleton)->Outcome, ModelOutcome::NeedsReview);
}

TEST(Updater, CaseOnlyAnimationNamesAreRepaired) {
    const auto casing_model = Model({
        .Names = {"root", "weapon_hand_L", "magazine_L"}, .Parents = {-1, 0, 1}, .Remap = std::vector<int>{0, 1, 2},
        .Pose = 3, .SkeletonRef = kSkeletonId});
    const auto game_animation = Animation({
        .Names = {"root", "weapon_hand_L", "magazine_L", "attachWorld"}, .Parents = {-1, 0, 1, 0}, .Lod = 2});
    const auto mod_animation = Animation({
        .Names = {"root", "weapon_hand_l", "magazine_l"}, .Parents = {-1, 0, 1}, .Lod = 3, .PoseX = 7,
        .MaskLabel = "weapon_hand_l", .OtherMaskSkeleton = "models/other.vnmskel"});
    Pair pair(
        {{kModel, casing_model}, {kSkeleton, mod_animation}},
        {{kModel, casing_model}, {kSkeleton, game_animation}});
    const UpdateReport report = HeroModelUpdater::Update(pair.game, Utf8(pair.mod_path), std::nullopt, true, true);
    EXPECT_TRUE(report.WroteFile);
    const VpkArchive archive = VpkArchive::Open(pair.mod_path);
    EXPECT_EQ(EntryBytes(archive, kModel), casing_model);
    const KvDocument data = DataOf(EntryBytes(archive, kSkeleton));
    EXPECT_EQ(data.Root.Find("m_boneIDs")->At(1).String(), "weapon_hand_L");
    EXPECT_EQ(data.Root.Find("m_boneIDs")->At(2).String(), "magazine_L");
    EXPECT_FLOAT_EQ(data.Root.Find("m_parentSpaceReferencePose")->At(1).At(0).Float(), 7.0f);
    const KvValue& mask = data.Root.Find("m_maskDefinitions")->At(0);
    EXPECT_EQ(mask.Find("m_primaryWeightList")->Find("m_boneIDs")->At(1).String(), "weapon_hand_L");
    EXPECT_FLOAT_EQ(mask.Find("m_primaryWeightList")->Find("m_weights")->At(1).Float(), 0.5f);
    EXPECT_EQ(mask.Find("m_ID")->String(), "weapon_hand_l");
    EXPECT_EQ(mask.Find("m_secondaryWeightList")->Find("m_boneIDs")->At(1).String(), "weapon_hand_l");
    EXPECT_FALSE(HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path)).CanRepair());
}

TEST(Updater, CasingWithChangedParentsNeedsReview) {
    const auto casing_model = Model({
        .Names = {"root", "weapon_hand_L", "magazine_L"}, .Parents = {-1, 0, 1}, .Remap = std::vector<int>{0, 1, 2}, .SkeletonRef = kSkeletonId});
    const auto game_animation = Animation({
        .Names = {"root", "weapon_hand_L", "magazine_L", "attachWorld"}, .Parents = {-1, 0, 1, 0}, .Lod = 2});
    const auto changed = Animation({.Names = {"root", "weapon_hand_l", "magazine_l"}, .Parents = {-1, 0, 0}, .Lod = 3});
    Pair pair({{kModel, casing_model}, {kSkeleton, changed}}, {{kModel, casing_model}, {kSkeleton, game_animation}});
    EXPECT_TRUE(HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path)).HasBlockers());
}

TEST(Updater, ObsoleteRenderSpellingBlocksASkeletonRename) {
    const auto old_render = Model({
        .Names = {"root", "weapon_hand_l", "magazine_l"}, .Parents = {-1, 0, 1}, .Remap = std::vector<int>{0, 1, 2}, .SkeletonRef = kSkeletonId});
    const auto game_model = Model({
        .Names = {"root", "weapon_hand_L", "magazine_L"}, .Parents = {-1, 0, 1}, .Remap = std::vector<int>{0, 1, 2}, .SkeletonRef = kSkeletonId});
    const auto game_animation = Animation({
        .Names = {"root", "weapon_hand_L", "magazine_L", "attachWorld"}, .Parents = {-1, 0, 1, 0}, .Lod = 2});
    const auto mod_animation = Animation({.Names = {"root", "weapon_hand_l", "magazine_l"}, .Parents = {-1, 0, 1}, .Lod = 3});
    Pair pair({{kModel, old_render}, {kSkeleton, mod_animation}}, {{kModel, game_model}, {kSkeleton, game_animation}});
    const UpdateReport report = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    EXPECT_TRUE(report.HasBlockers());
    ASSERT_NE(ResultFor(report, kSkeleton), nullptr);
    EXPECT_NE(ResultFor(report, kSkeleton)->Detail.find("Render-resource references"), std::string::npos);
}

TEST(Updater, AThirdRenderSpellingAlsoNeedsReview) {
    const auto variant = Model({
        .Names = {"root", "WEAPON_HAND_l", "magazine_L"}, .Parents = {-1, 0, 1}, .Remap = std::vector<int>{0, 1, 2}, .SkeletonRef = kSkeletonId});
    const auto game_model = Model({
        .Names = {"root", "weapon_hand_L", "magazine_L"}, .Parents = {-1, 0, 1}, .Remap = std::vector<int>{0, 1, 2}, .SkeletonRef = kSkeletonId});
    const auto game_animation = Animation({
        .Names = {"root", "weapon_hand_L", "magazine_L", "attachWorld"}, .Parents = {-1, 0, 1, 0}, .Lod = 2});
    const auto mod_animation = Animation({.Names = {"root", "weapon_hand_l", "magazine_l"}, .Parents = {-1, 0, 1}, .Lod = 3});
    Pair pair({{kModel, variant}, {kSkeleton, mod_animation}}, {{kModel, game_model}, {kSkeleton, game_animation}});
    EXPECT_TRUE(HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path)).HasBlockers());
}

TEST(Updater, MissingExternalReferenceNeedsReview) {
    const auto referenced = Model({
        .Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1},
        .References = {"models/heroes/test/missing.vnmskel"}});
    Pair pair({{kModel, referenced}});
    const UpdateReport report = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    EXPECT_TRUE(report.HasBlockers());
    EXPECT_EQ(report.Results.at(0).Outcome, ModelOutcome::NeedsReview);
    EXPECT_NE(report.Results.at(0).Detail.find("models/heroes/test/missing.vnmskel"), std::string::npos);
    EXPECT_FALSE(report.Results.at(0).BoneRenames.has_value());
}

TEST(Updater, PresentExternalReferenceStaysCurrent) {
    const auto referenced = Model({
        .Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1},
        .References = {"models/heroes/test/missing.vnmskel"}});
    Pair pair({{kModel, referenced}}, {{kModel, kGameModel}, {"models/heroes/test/missing.vnmskel_c", std::vector<uint8_t>{1}}});
    const UpdateReport report = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    EXPECT_FALSE(report.HasBlockers());
    EXPECT_EQ(report.Results.at(0).Outcome, ModelOutcome::Current);
}

TEST(Updater, AbsentLegacyGraphIsANoteWhenAg2IsDeclared) {
    const auto game_model = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}, .SkeletonRef = kSkeletonId});
    const auto mod_model = Model({
        .Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}, .SkeletonRef = kSkeletonId,
        .Graphs = {"models/heroes/test/old.vanmgrph"}});
    const auto stock = Animation({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Lod = 1});
    Pair pair({{kModel, mod_model}}, {{kModel, game_model}, {kSkeleton, stock}});
    const UpdateReport report = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    EXPECT_FALSE(report.HasBlockers());
    EXPECT_EQ(report.Results.at(0).Outcome, ModelOutcome::Current);
    ASSERT_TRUE(report.Results.at(0).Warnings.has_value());
    ASSERT_FALSE(report.Results.at(0).Warnings->empty());
    EXPECT_NE(report.Results.at(0).Warnings->at(0).find("Legacy AG1"), std::string::npos);
}

TEST(Updater, ALegacyGraphThatExistsIsNotAWarning) {
    const auto game_model = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}, .SkeletonRef = kSkeletonId});
    const auto mod_model = Model({
        .Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}, .SkeletonRef = kSkeletonId,
        .Graphs = {"models/heroes/test/old.vanmgrph"}});
    const auto stock = Animation({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Lod = 1});
    Pair pair(
        {{kModel, mod_model}, {"models/heroes/test/old.vanmgrph_c", std::vector<uint8_t>{4}}},
        {{kModel, game_model}, {kSkeleton, stock}});
    const UpdateReport report = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    EXPECT_EQ(report.Results.at(0).Outcome, ModelOutcome::Current);
    if (report.Results.at(0).Warnings) {
        for (const std::string& warning : *report.Results.at(0).Warnings) {
            EXPECT_EQ(warning.find("Legacy AG1"), std::string::npos) << warning;
        }
    }
}

TEST(Updater, MissingPhysicsBlocksAReorder) {
    const auto mod_model = Model({
        .Names = {"arm", "root"}, .Parents = {1, -1}, .Remap = std::vector<int>{0, 1},
        .Physics = {"models/heroes/test/test.vphys"}});
    Pair pair({{kModel, mod_model}});
    const auto before = ReadBytes(pair.mod_path);
    const UpdateReport report = HeroModelUpdater::Update(pair.game, Utf8(pair.mod_path), std::nullopt, true, true);
    EXPECT_TRUE(report.HasBlockers());
    EXPECT_FALSE(report.WroteFile);
    EXPECT_EQ(report.Results.at(0).Outcome, ModelOutcome::NeedsReview);
    EXPECT_NE(report.Results.at(0).Detail.find("models/heroes/test/test.vphys"), std::string::npos);
    EXPECT_EQ(ReadBytes(pair.mod_path), before);
}

TEST(Updater, AmbiguousSuffixAndMissingModelAreReported) {
    const std::vector<uint8_t> bytes{1};
    TempDir dir;
    const fs::path game_dir = dir.Path() / "game";
    Archive(game_dir / "pak01_dir.vpk", {
        {"models/heroes/a/test.vmdl_c", bytes},
        {"models/heroes/b/test.vmdl_c", bytes},
    });
    const fs::path ambiguous = dir.Path() / "ambiguous.vpk";
    Archive(ambiguous, {{"models/heroes/c/test.vmdl_c", bytes}});
    const UpdateReport ambiguous_report = HeroModelUpdater::Analyze(Utf8(game_dir), Utf8(ambiguous));
    EXPECT_EQ(ambiguous_report.Results.at(0).Detail, "Ambiguous: 2 game models end with test.vmdl_c.");

    const fs::path missing = dir.Path() / "missing-model.vpk";
    Archive(missing, {{"models/heroes/nope/other.vmdl_c", bytes}});
    const UpdateReport missing_report = HeroModelUpdater::Analyze(Utf8(game_dir), Utf8(missing));
    EXPECT_EQ(missing_report.Results.at(0).Detail, "No matching model in the current game.");
}

TEST(Updater, RejectsUnsafePathsAndBlankArguments) {
    TempDir dir;
    const fs::path game_dir = dir.Path() / "game";
    Archive(game_dir / "pak01_dir.vpk", {{kModel, kGameModel}});
    const fs::path mod = dir.Path() / "mod.vpk";
    Archive(mod, {{kModel, kGameModel}});
    const std::string game = Utf8(game_dir);
    const std::string mod_text = Utf8(mod);
    const std::string pak = Utf8(game_dir / "pak01_dir.vpk");

    ExpectError<std::invalid_argument>([&] { HeroModelUpdater::Analyze("   ", mod_text); }, "Game folder is required.");
    ExpectError<std::invalid_argument>([&] { HeroModelUpdater::Analyze(game, " \t"); }, "Mod VPK is required.");
    ExpectError<std::invalid_argument>([&] { HeroModelUpdater::Update(game, mod_text, std::nullopt, false, false); }, "Output VPK is required.");
    ExpectError<InvalidOperation>([&] { HeroModelUpdater::Update(game, mod_text, std::nullopt, true, false); },
        "Replacing a mod requires a backup. Enable backups or write to a new file.");
    ExpectError<InvalidOperation>([&] { HeroModelUpdater::Analyze(game, pak); },
        "That file is the game archive. Choose a mod VPK from the addons folder.");

    const fs::path beside = game_dir / "addon.vpk";
    Archive(beside, {{kModel, kGameModel}});
    ExpectError<InvalidOperation>([&] { HeroModelUpdater::Analyze(game, Utf8(beside)); },
        "Choose a mod outside the game's archive folder, such as a VPK in addons.");
    ExpectError<InvalidOperation>([&] { HeroModelUpdater::Update(game, mod_text, Utf8(game_dir / "out.vpk"), false, false); },
        "Output must not be placed beside the official game archives.");
    ExpectError<InvalidOperation>([&] { HeroModelUpdater::Update(game, mod_text, mod_text, false, false); },
        "The output VPK has to be a different file from the mod. The tool reads the mod while it writes.");
    ExpectError<IoError>([&] { HeroModelUpdater::Analyze(Utf8(dir.Path() / "empty"), mod_text); },
        "Could not find pak01_dir.vpk in the game folder.");
    ExpectError<IoError>([&] { HeroModelUpdater::Analyze(game, Utf8(dir.Path() / "missing.vpk")); },
        "Could not find the mod VPK.");
}

TEST(Updater, CurrentAnimationLeavesRenameFieldsUnset) {
    const auto animation = Animation({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Lod = 1});
    Pair pair({{kSkeleton, animation}}, {{kModel, kGameModel}, {kSkeleton, animation}});
    const UpdateReport report = HeroModelUpdater::Analyze(pair.game, Utf8(pair.mod_path));
    const ModelResult* skeleton = ResultFor(report, kSkeleton);
    ASSERT_NE(skeleton, nullptr);
    EXPECT_EQ(skeleton->Outcome, ModelOutcome::Current);
    EXPECT_FALSE(skeleton->Warnings.has_value());
    EXPECT_FALSE(skeleton->BoneRenames.has_value());
}

TEST(Updater, CliAnalyzeAgreesWithTheJsonReport) {
    Pair pair({{kModel, kGameModel}});
    const fs::path report_path = pair.dir.Path() / "nested" / "report.json";
    const std::string mod = Utf8(pair.mod_path);
    const CliOutput run = Execute({"--analyze", pair.game, mod, "--report", Utf8(report_path)});
    EXPECT_EQ(run.Code, 0);
    EXPECT_NE(run.Text.find("Current: "), std::string::npos);
    EXPECT_NE(run.Text.find("No VPK was written."), std::string::npos);
    EXPECT_NE(run.Text.find("Saved report: "), std::string::npos);
    EXPECT_EQ(run.Text.find("Note:"), std::string::npos);

    const std::vector<uint8_t> report_bytes = ReadBytes(report_path);
    const std::string text(report_bytes.begin(), report_bytes.end());
    EXPECT_EQ(text.find('\r'), std::string::npos);
    const auto json = nlohmann::ordered_json::parse(text);
    std::vector<std::string> keys;
    for (auto it = json.begin(); it != json.end(); ++it) {
        keys.push_back(it.key());
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"SchemaVersion", "GeneratedUtc", "GameDirectory", "Scope", "Archives"}));
    EXPECT_EQ(json["SchemaVersion"], 2);
    EXPECT_EQ(json["GameDirectory"], pair.game);
    EXPECT_EQ(json["Scope"], ReportExporter::Scope);
    const std::string generated = json["GeneratedUtc"].get<std::string>();
    EXPECT_TRUE(generated.size() >= 25 && generated.ends_with("+00:00") && generated[10] == 'T');
    EXPECT_EQ(json["Archives"][0]["ModPath"], mod);
    const auto& body = json["Archives"][0]["Report"];
    EXPECT_FALSE(body["WroteFile"].get<bool>());
    EXPECT_TRUE(body["OutputPath"].is_null());
    EXPECT_TRUE(body["BackupPath"].is_null());
    EXPECT_FALSE(body["HasFailures"].get<bool>());
    EXPECT_FALSE(body["HasBlockers"].get<bool>());
    EXPECT_FALSE(body["CanRepair"].get<bool>());
    EXPECT_EQ(body["Results"][0]["Outcome"], "Current");
    EXPECT_TRUE(body["Results"][0]["Warnings"].is_array());
    EXPECT_TRUE(body["Results"][0]["Warnings"].empty());
    EXPECT_TRUE(body["Results"][0]["BoneRenames"].is_null());
}

TEST(Updater, CliCommandsMatchTheContract) {
    Pair pair({{kModel, kOldModel}, {"materials/custom.txt", std::vector<uint8_t>{9, 8, 7}}});
    const std::string mod = Utf8(pair.mod_path);
    const fs::path output = pair.dir.Path() / "out.vpk";
    const CliOutput updated = Execute({"--update", pair.game, mod, Utf8(output)});
    EXPECT_EQ(updated.Code, 0);
    EXPECT_NE(updated.Text.find("Updated: "), std::string::npos);
    EXPECT_NE(updated.Text.find("Wrote "), std::string::npos);

    Pair repair({{kModel, kOldModel}});
    const CliOutput repaired = Execute({"--repair", repair.game, Utf8(repair.mod_path)});
    EXPECT_EQ(repaired.Code, 0);
    EXPECT_NE(repaired.Text.find("Updated: "), std::string::npos);
    EXPECT_NE(repaired.Text.find("Original backup: "), std::string::npos);

    const fs::path plain = pair.dir.Path() / "plain.vpk";
    Archive(plain, {{"a.txt", std::vector<uint8_t>{1}}});
    const CliOutput skipped = Execute({"--analyze", pair.game, Utf8(plain)});
    EXPECT_EQ(skipped.Code, 1);
    EXPECT_NE(skipped.Text.find("Skipped: "), std::string::npos);

    const fs::path error_report = pair.dir.Path() / "error.json";
    const CliOutput missing = Execute({"--analyze", pair.game, Utf8(pair.dir.Path() / "missing.vpk"), "--report", Utf8(error_report)});
    EXPECT_EQ(missing.Code, 2);
    EXPECT_NE(missing.Text.find("Failed: "), std::string::npos);
    const std::vector<uint8_t> saved_bytes = ReadBytes(error_report);
    const std::string saved(saved_bytes.begin(), saved_bytes.end());
    EXPECT_NE(saved.find("Failed"), std::string::npos);

    const CliOutput bad_report = Execute({"--analyze", pair.game, mod, "--report", mod});
    EXPECT_EQ(bad_report.Code, 2);
    EXPECT_NE(bad_report.Text.find("Report output must be a .json file."), std::string::npos);
    EXPECT_EQ(bad_report.Text.find("Reading the game archive"), std::string::npos);

    const CliOutput misplaced = Execute({"--analyze", pair.game, mod, "--report", Utf8(pair.dir.Path() / "late.json"), "extra"});
    EXPECT_EQ(misplaced.Code, 2);
    EXPECT_NE(misplaced.Text.find("Exit codes:"), std::string::npos);

    const CliOutput help = Execute({"--help"});
    EXPECT_EQ(help.Code, 0);
    EXPECT_NE(help.Text.find(std::string(SR_PRODUCT_TITLE) + " — hero models and animation skeleton repairs"), std::string::npos);
    EXPECT_EQ(Execute({"-h"}).Code, 0);
    const CliOutput version = Execute({"--version"});
    EXPECT_EQ(version.Code, 0);
    EXPECT_NE(version.Text.find(std::string(SR_PRODUCT_TITLE) + " (" + SR_GIT_COMMIT + ")"), std::string::npos);
    EXPECT_EQ(version.Text.find("Exit codes:"), std::string::npos);
    const CliOutput none = Execute({});
    EXPECT_EQ(none.Code, 2);
    EXPECT_NE(none.Text.find("Exit codes:"), std::string::npos);

    CancellationSource source;
    source.Cancel();
    const fs::path cancelled_report = pair.dir.Path() / "cancelled.json";
    const CliOutput cancelled = Execute({"--analyze", pair.game, mod, "--report", Utf8(cancelled_report)}, source.Token());
    EXPECT_EQ(cancelled.Code, 2);
    EXPECT_NE(cancelled.Text.find("Cancelled. No pending repair was installed."), std::string::npos);
    EXPECT_EQ(cancelled.Text.find("Saved report:"), std::string::npos);
    EXPECT_FALSE(fs::exists(cancelled_report));

    EXPECT_THROW(ReportExporter::Write(Utf8(pair.dir.Path() / "notes.txt"), pair.game, {}), std::invalid_argument);
}

TEST(Updater, OutputAtTheGameArchiveIsRejected) {
    TempDir dir;
    const fs::path game_dir = dir.Path() / "game";
    Archive(game_dir / "pak01_dir.vpk", {{kModel, kGameModel}});
    const fs::path mod = dir.Path() / "mod.vpk";
    Archive(mod, {{kModel, kOldModel}});
    ExpectError<InvalidOperation>(
        [&] { HeroModelUpdater::Update(Utf8(game_dir), Utf8(mod), Utf8(game_dir / "pak01_dir.vpk"), false, false); },
        "Output must not be placed beside the official game archives.");
}

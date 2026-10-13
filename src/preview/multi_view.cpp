#include "profile.hpp"
#include "multi_view.hpp"

#include "error.hpp"
#include "model_geometry.hpp"
#include "model_versions.hpp"
#include "strings.hpp"
#include "ui.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace skeleton_repair {
namespace {

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool SameModel(const LibraryModel& left, const LibraryModel& right) {
    return EqualsIgnoreCase(left.Model, right.Model)
        && EqualsIgnoreCase(Narrow(left.Archive.wstring()), Narrow(right.Archive.wstring()));
}

std::string FitText(std::string text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    const std::string ellipsis = "...";
    while (!text.empty() && ImGui::CalcTextSize((text + ellipsis).c_str()).x > width) text.pop_back();
    return text + ellipsis;
}

bool ContainsIgnoreCase(std::string_view text, std::string_view query) {
    if (query.empty()) return true;
    if (query.size() > text.size()) return false;
    for (size_t index = 0; index + query.size() <= text.size(); ++index) {
        if (EqualsIgnoreCase(text.substr(index, query.size()), query)) return true;
    }
    return false;
}

std::vector<std::string> ClipsFor(const std::string& model_path, const IndexedArchive* mod, const IndexedArchive* game) {
    const size_t slash = model_path.rfind('/');
    const std::string folder = slash == std::string::npos ? std::string() : model_path.substr(0, slash + 1);
    std::vector<std::string> clips;
    for (const IndexedArchive* archive : {mod, game}) {
        if (archive == nullptr) continue;
        for (const VpkEntry& entry : archive->Archive().Entries()) {
            if (entry.FullPath.size() > folder.size() && entry.FullPath.compare(0, folder.size(), folder) == 0
                && EndsWith(entry.FullPath, ".vnmclip_c")
                && std::find(clips.begin(), clips.end(), entry.FullPath) == clips.end()) {
                clips.push_back(entry.FullPath);
            }
        }
    }
    return clips;
}

ResourceReader ModThenGame(std::shared_ptr<const IndexedArchive> mod, std::shared_ptr<const IndexedArchive> game) {
    return [mod = std::move(mod), game = std::move(game)](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        if (mod) {
            if (auto bytes = mod->Read(path)) return bytes;
        }
        return game ? game->Read(path) : std::nullopt;
    };
}

}  // namespace

MultiView::MultiView(const VulkanDevice& device) {
    for (auto& view : views_) view = std::make_unique<ModelViewport>(device);
}

void MultiView::SetArchives(std::shared_ptr<ArchiveCache> archives) {
    archives_ = std::move(archives);
    Reload();
}

void MultiView::SetGame(std::shared_ptr<const IndexedArchive> game) {
    if (game_ == game) return;
    game_ = std::move(game);
    Reload();
}

void MultiView::Open(const LibraryModel& model, const std::vector<LibraryModel>& library, bool keep_layout) {
    const std::optional<std::string> character = CharacterOfModel(model.Model);
    character_ = character ? *character : std::string{};
    const auto found = std::find_if(library.begin(), library.end(), [&](const LibraryModel& other) { return SameModel(other, model); });
    const std::vector<size_t> indexes = found == library.end()
        ? std::vector<size_t>{}
        : CharacterSlots(library, static_cast<size_t>(found - library.begin()), kMaxSlots);

    for (int slot = 0; slot < kMaxSlots; ++slot) {
        loads_[slot].Cancel();
        slots_[slot] = {};
        views_[slot]->SetModel(static_cast<const PreparedModel*>(nullptr), 0);
        if (slot < static_cast<int>(indexes.size())) slots_[slot].Model = library[indexes[static_cast<size_t>(slot)]];
    }
    if (indexes.empty()) slots_[0].Model = model;
    framed_ = false;
    last_moved_ = 0;
    player_.SetClips({});
    clip_paths_.clear();
    if (!keep_layout) {
        int filled = 0;
        for (const Slot& slot : slots_) filled += slot.Model ? 1 : 0;
        slot_count_ = std::clamp(std::max(filled, 1), 1, kMaxSlots);
    }
    for (int slot = 0; slot < kMaxSlots; ++slot) {
        if (slots_[slot].Model) StartLoad(slot);
    }
}

void MultiView::OpenCharacter(const std::string& name, const std::vector<LibraryModel>& library) {
    if (EqualsIgnoreCase(character_, name) && std::any_of(slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.Model.has_value(); })) {
        return;
    }
    for (const LibraryModel& model : library) {
        const std::optional<std::string> character = CharacterOfModel(model.Model);
        if (character && EqualsIgnoreCase(*character, name)) {
            Open(model, library, true);
            return;
        }
    }
}

bool MultiView::Busy() const {
    return std::any_of(loads_.begin(), loads_.end(), [](const LatestTask<LoadResult>& load) { return load.Busy(); });
}

void MultiView::Release() {
    for (auto& load : loads_) load.Cancel(true);
    for (int slot = 0; slot < kMaxSlots; ++slot) {
        views_[slot]->SetModel(static_cast<const PreparedModel*>(nullptr), 0);
        slots_[slot].Prepared.reset();
        slots_[slot].Animation.reset();
        slots_[slot].Clips.clear();
        slots_[slot].Error.clear();
        slots_[slot].Warning.clear();
    }
    player_.SetClips({});
    clip_paths_.clear();
}

void MultiView::Reload() {
    if (!archives_) return;
    for (int slot = 0; slot < kMaxSlots; ++slot) {
        if (slots_[slot].Model) StartLoad(slot);
    }
}

MultiView::LoadResult MultiView::LoadSlot(std::shared_ptr<ArchiveCache> archives, std::shared_ptr<const IndexedArchive> game,
    LibraryModel model, CancellationToken token) {
    LoadResult result;
    result.Model = model;
    try {
        token.ThrowIfCancelled();
        if (!archives) {
            result.Error = "The model library is still opening.";
            return result;
        }
        const auto mod = archives->Get(model.Archive);
        const VpkEntry* entry = mod->Find(model.Model);
        if (entry == nullptr) {
            result.Error = "This model is no longer in " + Narrow(model.Archive.filename().wstring()) + ".";
            return result;
        }
        const auto bytes = mod->Read(entry->FullPath, token);
        if (!bytes) {
            result.Error = "This model is no longer in " + Narrow(model.Archive.filename().wstring()) + ".";
            return result;
        }
        const ModelGeometry geometry = DecodeModelGeometry(*bytes);
        const ResourceReader read = ModThenGame(mod, game);
        if (const std::optional<std::string> skeleton = AnimationSkeletonPath(geometry)) {
            if (const auto skeleton_bytes = read(*skeleton)) {
                try {
                    result.Animation = DecodeAnimationSkeleton(*skeleton_bytes);
                } catch (const std::exception&) {
                    // Playback stays off for this viewport. The mesh is still shown.
                }
            }
        }
        result.Clips = ClipsFor(model.Model, mod.get(), game.get());
        token.ThrowIfCancelled();
        result.Prepared = PrepareModel(geometry, &read);
    } catch (const OperationCancelled&) {
        throw;
    } catch (const std::exception& error) {
        result.Error = error.what();
    }
    return result;
}

void MultiView::StartLoad(int slot) {
    if (!slots_[slot].Model || !archives_) {
        loads_[slot].Cancel();
        views_[slot]->SetModel(static_cast<const PreparedModel*>(nullptr), 0);
        slots_[slot].Prepared.reset();
        slots_[slot].Animation.reset();
        slots_[slot].Clips.clear();
        slots_[slot].Error.clear();
        slots_[slot].Warning.clear();
        SyncClips();
        return;
    }
    // Leave the current mesh on screen until the replacement upload replaces it. Dropping it here frees GPU
    // buffers the previous frame is still drawing.
    slots_[slot].Error.clear();
    slots_[slot].Warning.clear();
    const LibraryModel model = *slots_[slot].Model;
    loads_[slot].Start([archives = archives_, game = game_, model](CancellationToken token) {
        return LoadSlot(archives, game, model, token);
    });
}

void MultiView::Apply(int slot, LoadResult result) {
    if (!slots_[slot].Model || !SameModel(*slots_[slot].Model, result.Model)) return;
    slots_[slot].Clips = std::move(result.Clips);
    slots_[slot].Warning.clear();
    if (!result.Prepared) {
        views_[slot]->SetModel(static_cast<const PreparedModel*>(nullptr), 0);
        slots_[slot].Prepared.reset();
        slots_[slot].Animation.reset();
        slots_[slot].Error = result.Error.empty() ? "This model could not be loaded." : std::move(result.Error);
        SyncClips();
        return;
    }
    slots_[slot].Error.clear();
    slots_[slot].Prepared = std::move(result.Prepared);
    slots_[slot].Animation = std::move(result.Animation);
    views_[slot]->SetModel(&*slots_[slot].Prepared, settings_.Lod);
    views_[slot]->SetAnimationSkeleton(slots_[slot].Animation ? &*slots_[slot].Animation : nullptr);
    // Frame the clicked model when it arrives. Another slot frames the view only after that load has finished.
    const bool waiting_for_first = slots_[0].Model && loads_[0].Busy() && views_[0]->Empty();
    if (!framed_ && !views_[slot]->Empty() && (slot == 0 || !waiting_for_first)) {
        shared_ = OrbitCamera::Framing(views_[slot]->Scene().Min, views_[slot]->Scene().Max);
        for (Slot& other : slots_) other.Camera = shared_;
        framed_ = true;
    }
    SyncClips();
}

void MultiView::Assign(int slot, std::optional<LibraryModel> model) {
    if (slot < 0 || slot >= kMaxSlots) return;
    if (!slots_[slot].Model && !model) return;
    if (slots_[slot].Model && model && SameModel(*slots_[slot].Model, *model)) return;
    slots_[slot].Model = std::move(model);
    StartLoad(slot);
}

void MultiView::SyncClips() {
    std::vector<std::string> paths;
    for (const Slot& slot : slots_) {
        paths.insert(paths.end(), slot.Clips.begin(), slot.Clips.end());
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (paths == clip_paths_) return;
    clip_paths_ = std::move(paths);
    const bool animating = std::any_of(views_.begin(), views_.end(), [](const std::unique_ptr<ModelViewport>& view) {
        return view && view->CanAnimate();
    });
    player_.SetClips(animating ? clip_paths_ : std::vector<std::string>{});
}

void MultiView::SetLinked(bool linked) {
    if (linked == link_cameras_) return;
    if (linked) {
        const int source = std::clamp(last_moved_, 0, kMaxSlots - 1);
        shared_ = slots_[static_cast<size_t>(source)].Camera;
        for (Slot& slot : slots_) slot.Camera = shared_;
    } else {
        for (Slot& slot : slots_) slot.Camera = shared_;
    }
    link_cameras_ = linked;
}

ResourceReader MultiView::ClipReader() const {
    std::vector<std::filesystem::path> sources;
    for (const Slot& slot : slots_) {
        if (slot.Model) sources.push_back(slot.Model->Archive);
    }
    auto archives = archives_;
    auto game = game_;
    return [archives = std::move(archives), game = std::move(game), sources = std::move(sources)](const std::string& path)
        -> std::optional<std::vector<uint8_t>> {
        if (archives) {
            for (const std::filesystem::path& source : sources) {
                try {
                    if (const auto bytes = archives->Get(source)->Read(path)) return bytes;
                } catch (const OperationCancelled&) {
                    throw;
                } catch (const std::exception&) {
                }
            }
        }
        return game ? game->Read(path) : std::nullopt;
    };
}

void MultiView::Poll() {
    ZoneScopedN("Multi view poll");
    for (int slot = 0; slot < kMaxSlots; ++slot) {
        try {
            if (std::optional<LoadResult> result = loads_[slot].Take()) Apply(slot, std::move(*result));
        } catch (const OperationCancelled&) {
        } catch (const std::exception& error) {
            slots_[slot].Error = error.what();
            views_[slot]->SetModel(static_cast<const PreparedModel*>(nullptr), 0);
        }
    }
}

void MultiView::DrawSlot(int slot, ImVec2 size, const std::vector<LibraryModel>& choices, bool& frame, bool& lod_changed) {
    ImGui::BeginGroup();
    const float combo_height = ImGui::GetFrameHeightWithSpacing();
    const ImVec2 image{size.x, std::max(32.0f, size.y - combo_height)};
    const char* preview = "Choose a model";
    std::string preview_storage;
    if (slots_[slot].Model) {
        preview_storage = SlotCaption(*slots_[slot].Model);
        preview = preview_storage.c_str();
    }
    ImGui::SetNextItemWidth(size.x);
    if (ImGui::BeginCombo("##model", preview, ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) {
            picker_filter_[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filter", "Filter models", picker_filter_, sizeof(picker_filter_));
        if (ImGui::Selectable("(empty)", !slots_[slot].Model)) {
            Assign(slot, std::nullopt);
            ImGui::CloseCurrentPopup();
        }
        for (size_t index = 0; index < choices.size(); ++index) {
            const LibraryModel& choice = choices[index];
            const std::string caption = SlotCaption(choice);
            if (!ContainsIgnoreCase(caption, picker_filter_)) continue;
            const bool current = slots_[slot].Model && SameModel(*slots_[slot].Model, choice);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(caption.c_str(), current)) {
                Assign(slot, choice);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s\n%s", choice.Model.c_str(), Narrow(choice.Archive.wstring()).c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Model shown in this viewport");

    ModelViewport& view = *views_[slot];
    OrbitCamera& camera = link_cameras_ ? shared_ : slots_[slot].Camera;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const bool loading = loads_[slot].Busy();
    if (view.Empty()) {
        ImGui::Dummy(image);
        draw->AddRectFilled(origin, {origin.x + image.x, origin.y + image.y}, ImGui::GetColorU32(ImGuiCol_FrameBg));
        const char* message = loading ? "Loading..." : !slots_[slot].Error.empty() ? slots_[slot].Error.c_str() : "Choose a model";
        const ImVec4 color = slots_[slot].Error.empty() || loading ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : ui::kError;
        draw->AddText({origin.x + 6, origin.y + ImGui::GetTextLineHeight() + 8}, ImGui::GetColorU32(color), FitText(message, image.x - 12).c_str());
    } else if (view.Draw("viewport", image, camera)) {
        last_moved_ = slot;
        if (link_cameras_) {
            for (Slot& other : slots_) other.Camera = shared_;
        }
    }
    if (loading && !view.Empty()) {
        draw->AddText({origin.x + 6, origin.y + ImGui::GetTextLineHeight() + 8}, ImGui::GetColorU32(ImGuiCol_TextDisabled), "Loading...");
    }
    if (view.TakeContextMenu()) ImGui::OpenPopup("slot_menu");

    const std::string caption = slots_[slot].Model ? FitText(SlotCaption(*slots_[slot].Model), image.x - 16) : std::string("Empty");
    const ImVec2 text = ImGui::CalcTextSize(caption.c_str());
    draw->AddRectFilled({origin.x, origin.y}, {origin.x + text.x + 12, origin.y + text.y + 6}, IM_COL32(0, 0, 0, 140));
    draw->AddText({origin.x + 6, origin.y + 3}, ImGui::GetColorU32(ImGuiCol_Text), caption.c_str());
    if (!slots_[slot].Warning.empty()) {
        draw->AddText({origin.x + text.x + 18, origin.y + 3}, ImGui::GetColorU32(ui::kWarning), "clip mismatch");
    }

    if (ImGui::BeginPopup("slot_menu")) {
        bool menu_frame = false;
        int lod_count = 1;
        for (int index = 0; index < slot_count_; ++index) lod_count = std::max(lod_count, views_[index]->Scene().LodCount);
        lod_changed = DrawViewMenu(settings_, lod_count, menu_frame) || lod_changed;
        frame = frame || menu_frame;
        ImGui::Separator();
        if (ImGui::MenuItem("Link cameras", nullptr, link_cameras_)) SetLinked(!link_cameras_);
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
}

void MultiView::Draw(const std::vector<LibraryModel>& library) {
    ZoneScopedN("Multi view");
    const float scale = ImGui::GetFontSize() / 15.0f;
    const std::vector<CharacterOption> characters = CharactersIn(library);
    ImGui::SetNextItemWidth(180 * scale);
    const char* character_preview = character_.empty() ? "Character" : character_.c_str();
    if (ImGui::BeginCombo("##character", character_preview, ImGuiComboFlags_HeightLarge)) {
        if (characters.empty()) ImGui::TextDisabled("No hero models in the library yet.");
        for (const CharacterOption& option : characters) {
            char label[160];
            std::snprintf(label, sizeof(label), "%s  (%zu)", option.Name.c_str(), option.Count);
            if (ImGui::Selectable(label, EqualsIgnoreCase(option.Name, character_))) {
                OpenCharacter(option.Name, library);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Character whose models fill the viewports");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(52 * scale);
    const char* counts[] = {"1", "2", "3", "4"};
    int count_index = std::clamp(slot_count_, 1, kMaxSlots) - 1;
    if (ImGui::Combo("##count", &count_index, counts, kMaxSlots)) slot_count_ = count_index + 1;
    ImGui::SetItemTooltip("How many viewports to show");
    ImGui::SameLine();
    bool linked = link_cameras_;
    if (ImGui::Checkbox("Link", &linked)) SetLinked(linked);
    ImGui::SetItemTooltip("Orbit, pan and zoom move every viewport together");

    if (character_.empty() && std::none_of(slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.Model.has_value(); })) {
        ImGui::TextDisabled("Right-click a skin in the library and choose Multi view, or pick a character above.");
        return;
    }

    std::vector<LibraryModel> choices;
    if (!character_.empty()) {
        for (const LibraryModel& model : library) {
            const std::optional<std::string> name = CharacterOfModel(model.Model);
            if (name && EqualsIgnoreCase(*name, character_)) choices.push_back(model);
        }
        std::sort(choices.begin(), choices.end(), [](const LibraryModel& left, const LibraryModel& right) {
            return CompareIgnoreCase(SlotCaption(left), SlotCaption(right)) < 0;
        });
    }

    int lod_count = 1;
    bool can_animate = false;
    for (int slot = 0; slot < slot_count_; ++slot) {
        lod_count = std::max(lod_count, views_[slot]->Scene().LodCount);
        can_animate = can_animate || views_[slot]->CanAnimate();
    }
    bool frame = false;
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < 380 * scale) ImGui::NewLine();
    else ui::ToolSeparator();
    bool lod_changed = DrawViewSettings(settings_, lod_count, frame);

    if (can_animate && archives_) {
        player_.Draw(ClipReader());
        player_.Advance(ImGui::GetIO().DeltaTime);
        for (int slot = 0; slot < slot_count_; ++slot) {
            slots_[slot].Warning = views_[slot]->Animate(player_.Clip(), player_.Time());
        }
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const ViewportGrid grid = GridForViewportCount(slot_count_);
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float cell_width = std::max(32.0f, (available.x - style.ItemSpacing.x * static_cast<float>(grid.Columns - 1)) / static_cast<float>(grid.Columns));
    const float cell_height = std::max(48.0f, (available.y - style.ItemSpacing.y * static_cast<float>(grid.Rows - 1)) / static_cast<float>(grid.Rows));
    for (int row = 0; row < grid.Rows; ++row) {
        for (int column = 0; column < grid.Columns; ++column) {
            if (column > 0) ImGui::SameLine(0, style.ItemSpacing.x);
            const int slot = row * grid.Columns + column;
            ImGui::PushID(slot);
            DrawSlot(slot, {cell_width, cell_height}, choices, frame, lod_changed);
            ImGui::PopID();
        }
    }

    if (lod_changed) {
        for (int slot = 0; slot < kMaxSlots; ++slot) views_[slot]->SetLod(settings_.Lod);
    }
    if (frame) {
        if (link_cameras_) {
            for (int slot = 0; slot < slot_count_; ++slot) {
                if (views_[slot]->Empty()) continue;
                shared_ = OrbitCamera::Framing(views_[slot]->Scene().Min, views_[slot]->Scene().Max);
                for (Slot& other : slots_) other.Camera = shared_;
                framed_ = true;
                break;
            }
        } else {
            for (int slot = 0; slot < slot_count_; ++slot) {
                if (views_[slot]->Empty()) continue;
                slots_[slot].Camera = OrbitCamera::Framing(views_[slot]->Scene().Min, views_[slot]->Scene().Max);
            }
        }
    }
}

void MultiView::Record(VkCommandBuffer command_buffer) {
    ZoneScopedN("Record multi view");
    for (int slot = 0; slot < slot_count_; ++slot) {
        const OrbitCamera& camera = link_cameras_ ? shared_ : slots_[slot].Camera;
        views_[slot]->Record(command_buffer, camera, settings_);
    }
}

}  // namespace skeleton_repair

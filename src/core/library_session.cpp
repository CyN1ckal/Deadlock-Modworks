#include "library_session.hpp"

#include "archive_extract.hpp"
#include "error.hpp"
#include "model_library.hpp"
#include "strings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <utility>
#include <string_view>

namespace skeleton_repair {
namespace {

namespace fs = std::filesystem;

StoreProgress Counting(LibraryJobProgress& progress) {
    return [&progress](size_t done, size_t total) {
        progress.Done = done;
        progress.Total = total;
    };
}

// A fresh folder under the store's downloads, removed with everything in it when the job ends.
class ScratchFolder {
public:
    ScratchFolder(const fs::path& root, const std::wstring& prefix)
        : path_(root / L"downloads" / (prefix + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()))) {
        fs::create_directories(path_);
    }
    ~ScratchFolder() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;
    const fs::path& Path() const { return path_; }

private:
    fs::path path_;
};

// A downloaded file's name, safe to create inside a folder.
std::wstring SafeFileName(const std::string& name) {
    std::wstring file_name = fs::path(Widen(name)).filename().wstring();
    for (auto& c : file_name) {
        if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) c = L'_';
    }
    if (file_name.empty() || file_name == L"." || file_name == L"..") file_name = L"download";
    return file_name;
}

bool SameText(const fs::path& a, const fs::path& b) {
    return EqualsIgnoreCase(Narrow(a.wstring()), Narrow(b.wstring()));
}

// Drops selections and packages whose source is not in the library.
void DropUnlisted(ModelLibrary& library) {
    const auto unlisted = [&](const fs::path& path) {
        return std::none_of(library.Sources.begin(), library.Sources.end(), [&](const fs::path& other) { return SameText(other, path); });
    };
    std::erase_if(library.Selected, [&](const LibraryModel& model) { return unlisted(model.Archive); });
    std::erase_if(library.Packages, unlisted);
}

// Puts `cached`, the stored copy of `source`, in the source's place.
void ReplaceSource(ModelLibrary& library, const fs::path& source, const fs::path& cached) {
    auto& sources = library.Sources;
    const bool stored = std::any_of(sources.begin(), sources.end(), [&](const fs::path& other) { return SameLibraryPath(other, cached); });
    const auto previous = std::find_if(sources.begin(), sources.end(), [&](const fs::path& other) { return SameLibraryPath(other, source); });
    // A changed copy, such as a repaired one, is stored as a new version that takes its place.
    if (previous != sources.end() && !SameLibraryPath(source, cached)) {
        if (stored) sources.erase(previous);
        else *previous = cached;
    } else if (!stored) {
        sources.push_back(cached);
    }
    for (auto& model : library.Selected) {
        if (SameLibraryPath(model.Archive, source)) model.Archive = cached;
    }
    for (auto& package : library.Packages) {
        if (SameLibraryPath(package, source)) package = cached;
    }
}

bool HasContent(const LocalModStore& store, const std::vector<fs::path>& sources, const std::optional<std::string>& id) {
    return std::any_of(sources.begin(), sources.end(), [&](const fs::path& source) { return store.ContentId(source) == id; });
}

}  // namespace

bool SameLibraryPath(const fs::path& a, const fs::path& b) {
    return EqualsIgnoreCase(Narrow(fs::absolute(a).lexically_normal().wstring()), Narrow(fs::absolute(b).lexically_normal().wstring()));
}

LibraryJobResult RestoreLibrary(const fs::path& root, CancellationToken cancellation, LibraryJobProgress& progress) {
    LibraryJobResult result;
    result.Library = LocalModStore(root).Load(cancellation, Counting(progress), &result.Errors);
    result.Restored = true;
    return result;
}

LibraryJobResult MergeLibrary(const fs::path& root, const fs::path& other, CancellationToken cancellation, LibraryJobProgress& progress) {
    const LocalModStore store(root);
    store.Merge(other, cancellation);
    LibraryJobResult result;
    result.Library = store.Load(cancellation, Counting(progress), &result.Errors);
    result.Restored = true;
    return result;
}

LibraryJobResult RenameStoredMod(const fs::path& root, const fs::path& archive, const std::string& name,
    CancellationToken cancellation, LibraryJobProgress& progress) {
    const LocalModStore store(root);
    store.Rename(archive, name);
    LibraryJobResult result;
    result.Library = store.Load(cancellation, Counting(progress), &result.Errors);
    result.Restored = true;
    result.Done = name.empty() ? "The mod's name is derived again." : "Renamed to " + name + ".";
    return result;
}

LibraryJobResult ImportSources(const fs::path& root, const std::vector<fs::path>& sources, ModelLibrary starting,
    bool checkpoints, CancellationToken cancellation, LibraryJobProgress& progress) {
    LibraryJobResult result;
    result.Library = std::move(starting);
    progress.Total = sources.size();
    const LocalModStore store(root);
    for (const auto& source : sources) {
        try {
            const auto cached = store.Import(source, cancellation);
            result.Imported.emplace_back(source, cached);
            ReplaceSource(result.Library, source, cached);
            if (checkpoints) {
                auto checkpoint = result.Library;
                DropUnlisted(checkpoint);
                store.Save(checkpoint);
            }
        } catch (const OperationCancelled&) {
            result.Errors += "Import cancelled. Completed imports were kept.\n";
            break;
        } catch (const std::exception& error) {
            result.Errors += Narrow(source.filename().wstring()) + ": " + error.what() + "\n";
        }
        ++progress.Done;
    }
    DropUnlisted(result.Library);
    return result;
}

LibraryJobResult DownloadMod(const fs::path& root, const GbMod& mod, const GbFile& file, bool enable,
    CancellationToken cancellation, LibraryJobProgress& progress) {
    LibraryJobResult result;
    const LocalModStore store(root);
    const ScratchFolder folder(root, L"");
    const auto archive = folder.Path() / SafeFileName(file.Name);
    gamebanana::Download(file, archive, cancellation, [&progress](uint64_t done, std::optional<uint64_t> total) {
        progress.Bytes = done;
        if (total) progress.ByteTotal = *total;
    });

    std::vector<fs::path> vpks;
    if (IsModArchive(archive)) vpks = ExtractModArchive(archive, folder.Path() / L"extracted", cancellation).Archives;
    else if (EqualsIgnoreCase(Narrow(archive.extension().wstring()), ".vpk")) vpks.push_back(archive);
    else throw InvalidData(file.Name + " is not a zip, 7z, rar or VPK file. Follow the mod page to install it by hand.");
    progress.Total = vpks.size();
    std::vector<fs::path> imported;
    for (const auto& vpk : vpks) {
        try {
            const auto cached = store.Import(vpk, cancellation);
            store.Rename(cached, vpks.size() == 1 ? mod.Name : mod.Name + " (" + Narrow(vpk.stem().wstring()) + ")");
            if (const auto id = store.ContentId(cached)) result.AddedIds.push_back(*id);
            imported.push_back(cached);
        } catch (const OperationCancelled&) {
            throw;
        } catch (const std::exception& error) {
            result.Errors += Narrow(vpk.filename().wstring()) + ": " + error.what() + "\n";
        }
        ++progress.Done;
    }
    // Add each new mod to the saved collection, then read it back so the copies carry the new names.
    std::string ignored;
    auto library = store.Load(cancellation, {}, &ignored);
    for (const auto& cached : imported) {
        if (!HasContent(store, library.Sources, store.ContentId(cached))) library.Sources.push_back(cached);
    }
    store.Save(library);
    // Load replaces its notice, so the import errors above are kept apart.
    std::string notice;
    result.Library = store.Load(cancellation, {}, &notice);
    result.Errors += notice;
    result.Restored = true;
    result.Origin = ModOrigin{mod.Id, file.Id, file.Name, mod.ProfileUrl, mod.DateModified};
    result.Enable = enable;
    if (!result.AddedIds.empty()) {
        result.Done = "Downloaded " + mod.Name + (enable ? ". It is on; choose Apply to game on My mods." : ". Turn it on in My mods.");
    }
    return result;
}

LibraryJobResult BuildPackAsMod(const fs::path& root, const std::string& name, bool enable, const ModelLibrary& selection,
    const fs::path& game_archive, const std::optional<std::string>& replaced, CancellationToken cancellation) {
    LibraryJobResult result;
    const LocalModStore store(root);
    const ScratchFolder folder(root, L"build-");
    const auto vpk = folder.Path() / L"pack.vpk";
    ExportModelLibrary(selection.Selected, game_archive, vpk, cancellation, selection.Packages);
    const auto cached = store.Import(vpk, cancellation);
    const auto id = store.ContentId(cached);
    if (!id) throw InvalidData("The built pack was not stored in the library.");

    std::string ignored;
    auto library = store.Load(cancellation, {}, &ignored);
    // The library stores identical files once, so a pack made of exactly one mod's files is that mod.
    // Naming it would rename that mod, and a later rebuild would take it out of the collection.
    if (id != replaced) {
        for (const auto& source : library.Sources) {
            if (store.ContentId(source) != id) continue;
            const auto other = store.Name(source).value_or(Narrow(source.stem().wstring()));
            throw InvalidOperation("The pack has exactly the same files as " + other
                + ", which is already in your library. Turn that mod on in My mods instead.");
        }
    }
    store.Rename(cached, name);
    const bool replacing = replaced && *replaced != *id;
    if (replacing) {
        const auto old = [&](const fs::path& path) { return store.ContentId(path) == replaced; };
        std::erase_if(library.Sources, old);
        std::erase_if(library.Packages, old);
        std::erase_if(library.Selected, [&](const LibraryModel& model) { return old(model.Archive); });
    }
    if (!HasContent(store, library.Sources, id)) library.Sources.push_back(cached);
    store.Save(library);
    std::string notice;
    result.Library = store.Load(cancellation, {}, &notice);
    result.Errors += notice;
    result.Restored = true;
    result.AddedIds = {*id};
    ModOrigin origin;
    origin.Built = true;
    result.Origin = origin;
    result.Enable = enable;
    if (replacing) result.ReplacedId = *replaced;
    result.Done = "Built " + name + (replacing ? ", replacing the earlier build" : "")
        + (enable ? ". It is on; choose Apply to game on My mods." : ". Turn it on in My mods.");
    return result;
}

LibrarySession::LibrarySession(const std::filesystem::path& data_root) {
    try {
        store_.emplace(data_root);
    } catch (const std::exception& error) {
        save_error_ = error.what();
        return;
    }
    restoring_ = true;
    Launch(Job::Opening, {}, 0, 0, [root = store_->Root()](CancellationToken token, LibraryJobProgress& progress) {
        return RestoreLibrary(root, token, progress);
    });
}

LibrarySession::~LibrarySession() {
    cancellation_.Cancel();
    if (worker_.valid()) worker_.wait();
}

void LibrarySession::Wait() const {
    if (worker_.valid()) worker_.wait();
}

template <typename Work>
void LibrarySession::Launch(Job job, std::string name, size_t total, uint64_t byte_total, Work work) {
    job_ = job;
    job_name_ = std::move(name);
    cancellation_ = CancellationSource{};
    progress_.Reset(total, byte_total);
    worker_ = std::async(std::launch::async, [work = std::move(work), token = cancellation_.Token(), progress = &progress_]() mutable {
        return work(token, *progress);
    });
}

LibrarySession::SaveOutcome LibrarySession::Save(const ModelLibrary& current, bool force) {
    if (!store_ || Busy() || !can_persist_) return SaveOutcome::Skipped;
    if (saved_ && *saved_ == current) return SaveOutcome::Unchanged;
    const auto now = std::chrono::steady_clock::now();
    if (!force && !save_error_.empty() && last_failed_save_ && now - *last_failed_save_ < std::chrono::seconds(5)) {
        return SaveOutcome::Skipped;
    }
    try {
        store_->Save(current);
    } catch (const std::exception& error) {
        save_error_ = error.what();
        last_failed_save_ = now;
        return SaveOutcome::Failed;
    }
    saved_ = current;
    save_error_.clear();
    last_failed_save_.reset();
    return SaveOutcome::Saved;
}

void LibrarySession::StartMerge(const std::filesystem::path& other) {
    if (!store_ || Busy()) return;
    Launch(Job::Merging, {}, 0, 0, [root = store_->Root(), other](CancellationToken token, LibraryJobProgress& progress) {
        return MergeLibrary(root, other, token, progress);
    });
}

void LibrarySession::StartRename(const std::filesystem::path& archive, const std::string& name) {
    if (!store_ || Busy()) return;
    Launch(Job::Renaming, {}, 0, 0, [root = store_->Root(), archive, name](CancellationToken token, LibraryJobProgress& progress) {
        return RenameStoredMod(root, archive, name, token, progress);
    });
}

void LibrarySession::StartImport(std::vector<std::filesystem::path> sources, ModelLibrary starting) {
    if (!store_ || Busy()) return;
    const size_t total = sources.size();
    Launch(Job::Importing, {}, total, 0, [root = store_->Root(), sources = std::move(sources), starting = std::move(starting),
        checkpoints = can_persist_](CancellationToken token, LibraryJobProgress& progress) mutable {
        return ImportSources(root, sources, std::move(starting), checkpoints, token, progress);
    });
}

void LibrarySession::StartDownload(const GbMod& mod, const GbFile& file, bool enable) {
    if (!store_ || Busy()) return;
    Launch(Job::Downloading, mod.Name, 0, file.Size, [root = store_->Root(), mod, file, enable](CancellationToken token,
        LibraryJobProgress& progress) {
        return DownloadMod(root, mod, file, enable, token, progress);
    });
}

void LibrarySession::StartBuild(const std::string& name, bool enable, ModelLibrary selection, std::filesystem::path game_archive,
    std::optional<std::string> replaced) {
    if (!store_ || Busy()) return;
    Launch(Job::Building, name, 0, 0, [root = store_->Root(), name, enable, selection = std::move(selection),
        game = std::move(game_archive), replaced = std::move(replaced)](CancellationToken token, LibraryJobProgress&) {
        return BuildPackAsMod(root, name, enable, selection, game, replaced, token);
    });
}

std::optional<LibrarySession::Finished> LibrarySession::Poll() {
    if (!worker_.valid() || worker_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return std::nullopt;
    Finished finished;
    finished.Kind = job_;
    finished.Name = job_name_;
    finished.WasRestoring = std::exchange(restoring_, false);
    try {
        finished.Result = worker_.get();
        if (finished.Result->Restored) {
            saved_ = finished.Result->Library;
            can_persist_ = true;
        }
    } catch (const std::exception& error) {
        finished.Error = error.what();
    }
    job_ = Job::None;
    return finished;
}

}  // namespace skeleton_repair

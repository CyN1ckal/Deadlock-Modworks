#pragma once

#include "cancellation.hpp"
#include "gamebanana.hpp"
#include "mod_state.hpp"
#include "mod_store.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skeleton_repair {

// Library workflows the model tools run on a worker thread. Each opens its own LocalModStore under `root`, so they
// share nothing with the UI thread except the progress counters.

// Counters a job updates while it runs and the UI reads every frame.
struct LibraryJobProgress {
    std::atomic<size_t> Done = 0;
    std::atomic<size_t> Total = 0;
    // Bytes of a download, when the job downloads.
    std::atomic<uint64_t> Bytes = 0;
    std::atomic<uint64_t> ByteTotal = 0;

    void Reset(size_t total = 0, uint64_t byte_total = 0) {
        Done = 0;
        Total = total;
        Bytes = 0;
        ByteTotal = byte_total;
    }
};

// What a finished job hands back to the UI.
struct LibraryJobResult {
    ModelLibrary Library;
    std::string Errors;
    // Each source with its stored copy.
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Imported;
    // The library was read back from the database, which now matches it.
    bool Restored = false;
    // Shown when the work finished without errors, instead of the import message.
    std::string Done;
    // A GameBanana download or a pack built as a mod: the content ids of its stored mods, where they came from, and
    // whether to enable them. A rebuilt mod replaces ReplacedId on the Mods page.
    std::vector<std::string> AddedIds;
    std::optional<ModOrigin> Origin;
    bool Enable = false;
    std::string ReplacedId;
};

// Reads the saved library, merging the folder-based store of earlier releases on first use.
LibraryJobResult RestoreLibrary(const std::filesystem::path& root, CancellationToken cancellation, LibraryJobProgress& progress);

// Adds a shared library database's mods and selections, then reads the library back.
LibraryJobResult MergeLibrary(const std::filesystem::path& root, const std::filesystem::path& other,
    CancellationToken cancellation, LibraryJobProgress& progress);

// Names a stored mod, then reads the library back so its copy carries the new name. An empty name derives it again.
LibraryJobResult RenameStoredMod(const std::filesystem::path& root, const std::filesystem::path& archive,
    const std::string& name, CancellationToken cancellation, LibraryJobProgress& progress);

// Stores each source and puts its stored copy in its place in `starting`. A changed copy, such as a repaired one,
// becomes a new version that takes the old one's place. With `checkpoints`, the collection is saved after every
// source, so a cancelled import keeps what it finished. Selections and packages of sources that are not in the
// result are dropped. Errors of single sources are collected; cancelling stops the loop.
LibraryJobResult ImportSources(const std::filesystem::path& root, const std::vector<std::filesystem::path>& sources,
    ModelLibrary starting, bool checkpoints, CancellationToken cancellation, LibraryJobProgress& progress);

// Downloads one file of a GameBanana mod, stores every mod VPK in it under the mod's name, and adds them to the
// saved collection. The result names where they came from so the Mods page can record it.
LibraryJobResult DownloadMod(const std::filesystem::path& root, const GbMod& mod, const GbFile& file, bool enable,
    CancellationToken cancellation, LibraryJobProgress& progress);

// Builds the selection like Build VPK, stores the pack as a mod called `name`, and adds it to the saved collection.
// `replaced` is the content id of an earlier build of that name, which leaves the collection.
LibraryJobResult BuildPackAsMod(const std::filesystem::path& root, const std::string& name, bool enable,
    const ModelLibrary& selection, const std::filesystem::path& game_archive, const std::optional<std::string>& replaced,
    CancellationToken cancellation);

// Two paths name the same file: compared absolute, normalized and ignoring case.
bool SameLibraryPath(const std::filesystem::path& a, const std::filesystem::path& b);

// The model tools' local mod library: the store under the data root, the one background job that reads or replaces
// it, and whether the collection on screen has been saved. Used from the UI thread only; jobs run on a worker and
// their results are applied by whoever calls Poll.
class LibrarySession {
public:
    enum class Job { None, Opening, Merging, Renaming, Importing, Downloading, Building };

    // Opens the store under `data_root` and starts reading the saved library. A store that cannot be opened leaves
    // Store() empty and the reason in SaveError().
    explicit LibrarySession(const std::filesystem::path& data_root);
    // Cancels the running job and waits for it, dropping its result.
    ~LibrarySession();
    LibrarySession(const LibrarySession&) = delete;
    LibrarySession& operator=(const LibrarySession&) = delete;

    const LocalModStore* Store() const { return store_ ? &*store_ : nullptr; }
    bool Busy() const { return worker_.valid(); }
    Job CurrentJob() const { return Busy() ? job_ : Job::None; }
    // True until the first read of the saved library has finished, successfully or not.
    bool Restoring() const { return restoring_; }
    // The saved library has been read, so a save cannot replace it with less.
    bool CanPersist() const { return can_persist_; }
    // The mod being downloaded or the pack being built.
    const std::string& JobName() const { return job_name_; }
    const LibraryJobProgress& Progress() const { return progress_; }
    // The last failure to open or save the library. The UI may clear it to dismiss it.
    std::string& SaveError() { return save_error_; }
    const std::string& SaveError() const { return save_error_; }
    void Cancel() { cancellation_.Cancel(); }
    // Blocks until the running job has finished. Poll then returns its result.
    void Wait() const;

    enum class SaveOutcome { Saved, Unchanged, Skipped, Failed };
    // Saves `current` unless it is already saved. Skipped while a job runs or before the library was read, and,
    // unless `force`, within five seconds of a failed save. A failure is kept in SaveError().
    SaveOutcome Save(const ModelLibrary& current, bool force);

    // Each starts a job unless one is running or there is no store. See library_jobs.hpp for what they do.
    void StartMerge(const std::filesystem::path& other);
    void StartRename(const std::filesystem::path& archive, const std::string& name);
    void StartImport(std::vector<std::filesystem::path> sources, ModelLibrary starting);
    void StartDownload(const GbMod& mod, const GbFile& file, bool enable);
    void StartBuild(const std::string& name, bool enable, ModelLibrary selection, std::filesystem::path game_archive,
        std::optional<std::string> replaced);

    struct Finished {
        Job Kind = Job::None;
        // JobName() while it ran.
        std::string Name;
        // The job was the first read of the saved library.
        bool WasRestoring = false;
        // Empty when the job threw; Error then says why.
        std::optional<LibraryJobResult> Result;
        std::string Error;
    };
    // The finished job, once. A result that read the library back marks it saved.
    std::optional<Finished> Poll();

private:
    template <typename Work>
    void Launch(Job job, std::string name, size_t total, uint64_t byte_total, Work work);

    std::optional<LocalModStore> store_;
    std::optional<ModelLibrary> saved_;
    std::string save_error_;
    std::optional<std::chrono::steady_clock::time_point> last_failed_save_;
    bool restoring_ = false;
    bool can_persist_ = false;
    Job job_ = Job::None;
    std::string job_name_;
    CancellationSource cancellation_;
    LibraryJobProgress progress_;
    // Declared last, so it is destroyed first: the job refers to the progress counters above.
    std::future<LibraryJobResult> worker_;
};

}  // namespace skeleton_repair

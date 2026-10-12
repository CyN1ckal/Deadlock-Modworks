#pragma once

#include "cancellation.hpp"
#include "vpk.hpp"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skeleton_repair {

// An opened VPK with a case-insensitive path index. Immutable, so any thread may read it.
class IndexedArchive {
public:
    explicit IndexedArchive(VpkArchive archive);
    IndexedArchive(const IndexedArchive&) = delete;
    IndexedArchive& operator=(const IndexedArchive&) = delete;

    const VpkArchive& Archive() const { return archive_; }
    const VpkEntry* Find(std::string_view path) const;
    std::optional<std::vector<uint8_t>> Read(std::string_view path, CancellationToken cancellation = {}) const;

private:
    VpkArchive archive_;
    std::unordered_map<std::string, const VpkEntry*> index_;
};

// Opens each archive once and shares it between threads. Concurrent requests for the same path wait for one open.
class ArchiveCache {
public:
    // Throws when the archive cannot be opened; a failed open is retried by the next request.
    std::shared_ptr<const IndexedArchive> Get(const std::filesystem::path& path);
    // Forgets every archive, for example after files were replaced. Holders keep their copies.
    void Clear();
    void Remove(const std::vector<std::filesystem::path>& paths);

private:
    static std::wstring Key(const std::filesystem::path& path);
    using Entry = std::shared_future<std::shared_ptr<const IndexedArchive>>;
    std::mutex mutex_;
    std::map<std::wstring, Entry> archives_;
};

// Runs one job at a time on its own worker thread. Starting a job cancels the unfinished one, and only the newest
// job's result is returned, so the UI never applies a stale load.
template <typename T>
class LatestTask {
public:
    using Work = std::function<T(CancellationToken)>;

    LatestTask() = default;
    LatestTask(const LatestTask&) = delete;
    LatestTask& operator=(const LatestTask&) = delete;
    ~LatestTask() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
            pending_ = nullptr;
            cancellation_.Cancel();
        }
        changed_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void Start(Work work) {
        {
            std::lock_guard lock(mutex_);
            cancellation_.Cancel();
            cancellation_ = CancellationSource{};
            pending_ = std::move(work);
            ++requested_;
            outstanding_ = true;
            ready_ = false;
            result_.reset();
            error_ = nullptr;
            if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
        }
        changed_.notify_all();
    }

    // Drops the outstanding job. With `wait`, also returns only once the worker is idle, so it holds no files open.
    void Cancel(bool wait = false) {
        std::unique_lock lock(mutex_);
        cancellation_.Cancel();
        pending_ = nullptr;
        ++requested_;
        outstanding_ = false;
        ready_ = false;
        result_.reset();
        error_ = nullptr;
        if (wait) idle_.wait(lock, [this] { return !running_; });
    }

    // True from Start until the result has been taken or cancelled.
    bool Busy() const {
        std::lock_guard lock(mutex_);
        return outstanding_;
    }

    // The newest job's result once it is ready. Rethrows what the job threw.
    std::optional<T> Take() {
        std::lock_guard lock(mutex_);
        if (!ready_) return std::nullopt;
        ready_ = false;
        outstanding_ = false;
        if (error_) {
            auto error = std::exchange(error_, nullptr);
            std::rethrow_exception(error);
        }
        return std::exchange(result_, std::nullopt);
    }

private:
    void Run() {
        std::unique_lock lock(mutex_);
        while (true) {
            changed_.wait(lock, [this] { return stop_ || pending_; });
            if (stop_) return;
            Work work = std::move(pending_);
            pending_ = nullptr;
            const uint64_t generation = requested_;
            const CancellationToken token = cancellation_.Token();
            running_ = true;
            lock.unlock();
            std::optional<T> value;
            std::exception_ptr error;
            try {
                value.emplace(work(token));
            } catch (...) {
                error = std::current_exception();
            }
            work = nullptr;
            lock.lock();
            running_ = false;
            if (generation == requested_) {
                result_ = std::move(value);
                error_ = error;
                ready_ = true;
            }
            idle_.notify_all();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::condition_variable idle_;
    std::thread worker_;
    Work pending_;
    CancellationSource cancellation_;
    uint64_t requested_ = 0;
    bool outstanding_ = false;
    bool running_ = false;
    bool ready_ = false;
    bool stop_ = false;
    std::optional<T> result_;
    std::exception_ptr error_;
};

}  // namespace skeleton_repair

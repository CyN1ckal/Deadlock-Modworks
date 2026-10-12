#pragma once

#include "error.hpp"

#include <atomic>
#include <memory>

namespace skeleton_repair {

class CancellationToken {
public:
    CancellationToken() = default;

    bool IsCancelled() const { return flag_ && flag_->load(std::memory_order_relaxed); }

    void ThrowIfCancelled() const {
        if (IsCancelled()) {
            throw OperationCancelled();
        }
    }

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {}

    std::shared_ptr<std::atomic<bool>> flag_;
};

class CancellationSource {
public:
    CancellationSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void Cancel() noexcept { flag_->store(true, std::memory_order_relaxed); }

    CancellationToken Token() const { return CancellationToken(flag_); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace skeleton_repair

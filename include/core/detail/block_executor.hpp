#ifndef WOLVRIX_CORE_DETAIL_BLOCK_EXECUTOR_HPP
#define WOLVRIX_CORE_DETAIL_BLOCK_EXECUTOR_HPP

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace wolvrix::lib::grh::detail {

// A short-lived executor for one flatten/import transaction. Workers remain
// alive between batches submitted by the owner, but the executor itself owns
// their lifetime and joins them before destruction. Tasks continue draining
// after a failure so shared graph mutation scopes can unwind cleanly.
class BlockExecutor {
public:
    explicit BlockExecutor(std::size_t threads = 1) {
        threads = threads ? threads : 1;
        workers_.reserve(threads);
        try {
            for (std::size_t i = 0; i < threads; ++i)
                workers_.emplace_back([this] { run(); });
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            ready_.notify_all();
            for (auto& worker : workers_) if (worker.joinable()) worker.join();
            throw;
        }
    }
    BlockExecutor(const BlockExecutor&) = delete;
    BlockExecutor& operator=(const BlockExecutor&) = delete;
    ~BlockExecutor() noexcept {
        try { wait(); } catch (...) {}
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }

    template <typename Function>
    void submit(Function&& function) {
        std::function<void()> task(std::forward<Function>(function));
        if (!task) throw std::invalid_argument("BlockExecutor task is empty");
        {
            std::lock_guard lock(mutex_);
            if (stopping_) throw std::logic_error("cannot submit to stopped BlockExecutor");
            if (nextSequence_ == std::numeric_limits<std::size_t>::max())
                throw std::length_error("BlockExecutor sequence capacity");
            queue_.push_back({nextSequence_, std::move(task)});
            ++nextSequence_;
            ++pending_;
        }
        ready_.notify_one();
    }

    void wait() {
        std::exception_ptr error;
        {
            std::unique_lock lock(mutex_);
            idle_.wait(lock, [&] { return pending_ == 0; });
            error = std::exchange(failure_, {});
            failureSequence_ = std::numeric_limits<std::size_t>::max();
        }
        if (error) std::rethrow_exception(error);
    }

    std::size_t threadCount() const noexcept { return workers_.size(); }

private:
    struct Task {
        std::size_t sequence;
        std::function<void()> function;
    };
    void run() noexcept {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) return;
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try { task.function(); }
            catch (...) {
                std::lock_guard lock(mutex_);
                // Recording a worker failure must not allocate or throw.
                if (task.sequence < failureSequence_) {
                    failureSequence_ = task.sequence;
                    failure_ = std::current_exception();
                }
            }
            {
                std::lock_guard lock(mutex_);
                if (--pending_ == 0) idle_.notify_all();
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable idle_;
    std::deque<Task> queue_;
    std::exception_ptr failure_;
    std::size_t failureSequence_ = std::numeric_limits<std::size_t>::max();
    std::vector<std::thread> workers_;
    std::size_t nextSequence_ = 0;
    std::size_t pending_ = 0;
    bool stopping_ = false;
};

} // namespace wolvrix::lib::grh::detail

#endif

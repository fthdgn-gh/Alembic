// SPDX-License-Identifier: GPL-3.0-or-later
// A fixed set of worker threads taking jobs from one queue, for the writers
// that spread work over the CPU cores (LZ4 packing, package encryption).
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace alembic {

// Jobs get the index of the worker that runs them (for per-worker state).
// The destructor runs the jobs still queued, then joins the workers.
class Pool {
public:
    explicit Pool(unsigned threads) {
        if (threads == 0) threads = 1;
        for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this, i] { run(i); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        work_.notify_all();
        for (std::thread& t : threads_) t.join();
    }
    unsigned size() const { return static_cast<unsigned>(threads_.size()); }

    void submit(std::function<void(unsigned)> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(job));
        }
        work_.notify_one();
    }

private:
    void run(unsigned index) {
        for (;;) {
            std::function<void(unsigned)> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job(index);
        }
    }

    std::vector<std::thread> threads_;
    std::deque<std::function<void(unsigned)>> queue_;
    std::mutex mutex_;
    std::condition_variable work_;
    bool stop_ = false;
};

// 0 means every core.
inline unsigned worker_count(unsigned requested) {
    if (requested) return requested;
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
}

} // namespace alembic

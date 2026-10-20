#pragma once
#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

inline size_t num_worker_threads() {
    static const size_t n = std::max(1u, std::thread::hardware_concurrency());
    return n;
}

// A fixed set of worker threads, created once and reused for every parallel phase.
// run(f) calls f(t) for every t in [0, size()) at the same time and returns when
// all calls have finished: the same fork-join pattern as spawning and joining
// size() threads, without creating any threads.
class ThreadPool {
public:
    explicit ThreadPool(size_t n) : n_(std::max<size_t>(1, n)) {
        for (size_t id = 1; id < n_; ++id)
            workers_.emplace_back([this, id] { worker_loop(id); });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        start_cv_.notify_all();
        for (auto& w : workers_) w.join();
    }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    size_t size() const { return n_; }

    // The calling thread runs t = 0, the workers run t = 1 .. size()-1.
    // Must not be called from inside a task (that would deadlock).
    template <class F>
    void run(F&& f) {
        assert(!in_task() && "ThreadPool::run called from inside a pool task");
        if (n_ == 1) { f(size_t{0}); return; }

        std::function<void(size_t)> job = [&f](size_t t) { f(t); };
        {
            std::lock_guard<std::mutex> lk(m_);
            job_       = &job;
            remaining_ = n_ - 1;
            ++generation_;
        }
        start_cv_.notify_all();

        in_task() = true;
        f(size_t{0});
        in_task() = false;

        std::unique_lock<std::mutex> lk(m_);
        done_cv_.wait(lk, [this] { return remaining_ == 0; });
        job_ = nullptr;
    }

private:
    static bool& in_task() {
        thread_local bool flag = false;
        return flag;
    }

    void worker_loop(size_t id) {
        size_t seen = 0;
        for (;;) {
            std::function<void(size_t)>* job;
            {
                std::unique_lock<std::mutex> lk(m_);
                start_cv_.wait(lk, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
                job  = job_;
            }
            in_task() = true;
            (*job)(id);
            in_task() = false;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (--remaining_ == 0) done_cv_.notify_one();
            }
        }
    }

    size_t                       n_;
    std::vector<std::thread>     workers_;
    std::mutex                   m_;
    std::condition_variable      start_cv_, done_cv_;
    std::function<void(size_t)>* job_        = nullptr;
    size_t                       remaining_  = 0;
    size_t                       generation_ = 0;
    bool                         stop_       = false;
};

// The one pool used by the whole program. Created on first use.
inline ThreadPool& pool() {
    static ThreadPool p(num_worker_threads());
    return p;
}
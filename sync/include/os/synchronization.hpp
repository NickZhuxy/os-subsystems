#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

namespace os::sync {
class FutexMutex {
    alignas(4) std::atomic<std::uint32_t> state_{0}; // 0 free, 1 held, 2 held with possible waiters.
public:
    FutexMutex() = default;
    FutexMutex(const FutexMutex&) = delete;
    FutexMutex& operator=(const FutexMutex&) = delete;
    void lock();
    bool try_lock();
    void unlock() noexcept;
};
class Semaphore {
    alignas(4) std::atomic<std::uint32_t> permits_;
public:
    explicit Semaphore(std::uint32_t permits = 0);
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
    void acquire();
    bool try_acquire();
    void release(std::uint32_t permits = 1);
};
}

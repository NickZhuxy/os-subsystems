#include "os/synchronization.hpp"
#include "os/fd.hpp"
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <climits>
#include <stdexcept>

namespace os::sync {
namespace {
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
void wait(std::atomic<std::uint32_t>& word, std::uint32_t expected) {
    auto result = syscall(SYS_futex,reinterpret_cast<std::uint32_t*>(&word),FUTEX_WAIT_PRIVATE,expected,nullptr,nullptr,0);
    if (result < 0 && errno != EAGAIN && errno != EINTR) os::system_error("futex wait");
}
void wake(std::atomic<std::uint32_t>& word, int count) {
    if (syscall(SYS_futex,reinterpret_cast<std::uint32_t*>(&word),FUTEX_WAKE_PRIVATE,count,nullptr,nullptr,0) < 0)
        os::system_error("futex wake");
}
}
void FutexMutex::lock() {
    std::uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected,1,std::memory_order_acquire,std::memory_order_relaxed)) return;
    while (state_.exchange(2,std::memory_order_acquire) != 0) wait(state_,2);
}
bool FutexMutex::try_lock() {
    std::uint32_t expected = 0;
    return state_.compare_exchange_strong(expected,1,std::memory_order_acquire,std::memory_order_relaxed);
}
void FutexMutex::unlock() noexcept {
    try { if (state_.exchange(0,std::memory_order_release) == 2) wake(state_,1); }
    catch (...) { std::terminate(); }
}
Semaphore::Semaphore(std::uint32_t permits) : permits_(permits) {
    if (permits > INT_MAX) throw std::invalid_argument("semaphore limit is INT_MAX");
}
bool Semaphore::try_acquire() {
    auto value = permits_.load(std::memory_order_relaxed);
    while (value) if (permits_.compare_exchange_weak(value,value - 1,std::memory_order_acquire,std::memory_order_relaxed)) return true;
    return false;
}
void Semaphore::acquire() { while (!try_acquire()) wait(permits_,0); }
void Semaphore::release(std::uint32_t count) {
    if (!count) return;
    auto value = permits_.load(std::memory_order_relaxed);
    do {
        if (count > INT_MAX || value > INT_MAX - count) throw std::overflow_error("semaphore permit overflow");
    } while (!permits_.compare_exchange_weak(value,value + count,std::memory_order_release,std::memory_order_relaxed));
    wake(permits_,static_cast<int>(count));
}
}

#include "os/synchronization.hpp"
#include "os/stack.hpp"
#include "os/queue.hpp"
#include "os/cli.hpp"
#include <algorithm>
#include <barrier>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t capacity = 8192, warmup = 2000;
struct Outcome { double seconds; std::uint64_t operations, checksum, retries; };
template<class Mutex> Outcome counter(std::size_t count, std::size_t iterations) {
    Mutex mutex; std::uint64_t value = 0;
    std::barrier warmed(count + 1), start(count + 1), finish(count + 1); std::vector<std::thread> threads;
    for (std::size_t t = 0; t < count; ++t) threads.emplace_back([&] {
        for (std::size_t i = 0; i < warmup; ++i) { std::lock_guard lock(mutex); ++value; }
        warmed.arrive_and_wait(); start.arrive_and_wait();
        for (std::size_t i = 0; i < iterations; ++i) { std::lock_guard lock(mutex); ++value; }
        finish.arrive_and_wait();
    });
    warmed.arrive_and_wait(); value = 0; auto before = Clock::now(); start.arrive_and_wait(); finish.arrive_and_wait();
    auto seconds = std::chrono::duration<double>(Clock::now() - before).count();
    for (auto& thread : threads) thread.join();
    if (value != count * iterations) throw std::runtime_error("lost protected increments");
    return {seconds,count * iterations,value,0};
}
template<bool Fifo> class MutexContainer {
    os::sync::FutexMutex mutex_;
    std::vector<std::uint64_t> values_ = std::vector<std::uint64_t>(capacity);
    std::size_t first_ = 0, size_ = 0;
public:
    std::monostate participate() { return {}; }
    bool push(std::monostate&, std::uint64_t value) {
        std::lock_guard lock(mutex_); if (size_ == capacity) return false;
        values_[(first_ + size_) % capacity] = value; ++size_; return true;
    }
    std::optional<std::uint64_t> pop(std::monostate&) {
        std::lock_guard lock(mutex_); if (!size_) return std::nullopt;
        auto value = values_[(first_ + (Fifo ? 0 : size_ - 1)) % capacity]; --size_;
        if constexpr (Fifo) first_ = (first_ + 1) % capacity;
        return value;
    }
};
struct Stack {
    os::sync::TaggedStack stack{capacity};
    std::monostate participate() { return {}; }
    bool push(std::monostate&, std::uint64_t value) { return stack.push(value); }
    std::optional<std::uint64_t> pop(std::monostate&) { return stack.pop(); }
};
struct Queue {
    os::sync::HazardQueue queue{capacity};
    auto participate() { return queue.participate(); }
    bool push(os::sync::HazardQueue::Participant& thread, std::uint64_t value) { return queue.enqueue(thread,value); }
    std::optional<std::uint64_t> pop(os::sync::HazardQueue::Participant& thread) { return queue.dequeue(thread); }
};
template<class Container> Outcome transfer(std::size_t count, std::size_t iterations) {
    Container container; std::vector<std::vector<std::uint64_t>> received(count,std::vector<std::uint64_t>(iterations));
    std::vector<std::uint64_t> retries(count); std::barrier warmed(count + 1), start(count + 1), finish(count + 1);
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < count; ++t) threads.emplace_back([&,t] {
        auto participant = container.participate();
        for (std::size_t i = 0; i < warmup; ++i) {
            while (!container.push(participant,0)) std::this_thread::yield();
            while (!container.pop(participant)) std::this_thread::yield();
        }
        warmed.arrive_and_wait(); start.arrive_and_wait();
        for (std::size_t i = 0; i < iterations; ++i) {
            while (!container.push(participant,t * iterations + i + 1)) { ++retries[t]; std::this_thread::yield(); }
            std::optional<std::uint64_t> value;
            while (!(value = container.pop(participant))) { ++retries[t]; std::this_thread::yield(); }
            received[t][i] = *value;
        }
        finish.arrive_and_wait();
    });
    warmed.arrive_and_wait(); auto before = Clock::now(); start.arrive_and_wait(); finish.arrive_and_wait();
    auto seconds = std::chrono::duration<double>(Clock::now() - before).count();
    for (auto& thread : threads) thread.join();
    std::vector<std::uint64_t> all; all.reserve(count * iterations); std::uint64_t retry_count = 0;
    for (const auto& values : received) all.insert(all.end(),values.begin(),values.end());
    for (auto value : retries) retry_count += value;
    std::sort(all.begin(),all.end()); std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i] != i + 1) throw std::runtime_error("lost, duplicate, or unexpected item"); checksum += all[i];
    }
    auto participant = container.participate(); if (container.pop(participant)) throw std::runtime_error("container did not drain");
    return {seconds,2 * count * iterations,checksum,retry_count};
}
}
int main(int argc, char** argv) {
    try {
        os::Arguments args(argc,argv); auto kind = args.take("--kind","hazard-queue");
        auto threads = args.number("--threads",4,1,64), iterations = args.number("--iterations",100000,1,32000000); args.finish();
        if (threads * iterations > 32000000) throw std::invalid_argument("maximum 32 million values per run");
        Outcome result{};
        if (kind == "futex-mutex") result = counter<os::sync::FutexMutex>(threads,iterations);
        else if (kind == "std-mutex") result = counter<std::mutex>(threads,iterations);
        else if (kind == "mutex-stack") result = transfer<MutexContainer<false>>(threads,iterations);
        else if (kind == "tagged-stack") result = transfer<Stack>(threads,iterations);
        else if (kind == "mutex-queue") result = transfer<MutexContainer<true>>(threads,iterations);
        else if (kind == "hazard-queue") result = transfer<Queue>(threads,iterations);
        else throw std::invalid_argument("unknown --kind");
        std::cout.precision(12);
        std::cout << "{\"kind\":\"" << kind << "\",\"threads\":" << threads << ",\"iterations_per_thread\":" << iterations
            << ",\"warmup_per_thread\":" << warmup << ",\"capacity\":" << capacity << ",\"seconds\":" << result.seconds
            << ",\"operations\":" << result.operations << ",\"operations_per_second\":" << result.operations / result.seconds
            << ",\"checksum\":" << result.checksum << ",\"retries\":" << result.retries << ",\"validated\":true}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

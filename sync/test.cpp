#include "os/synchronization.hpp"
#include "os/stack.hpp"
#include "os/queue.hpp"
#include "os/test.hpp"
#include <algorithm>
#include <barrier>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include <climits>
using namespace os::sync;
namespace {
void locks_and_semaphore() {
    FutexMutex mutex; CHECK(mutex.try_lock()); CHECK(!mutex.try_lock()); mutex.unlock();
    std::uint64_t counter = 0; std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) threads.emplace_back([&] {
        for (int j = 0; j < 10000; ++j) { std::lock_guard lock(mutex); ++counter; }
    });
    for (auto& thread : threads) thread.join(); CHECK(counter == 80000);
    Semaphore empty; CHECK(!empty.try_acquire()); empty.release(3);
    CHECK(empty.try_acquire()); CHECK(empty.try_acquire()); CHECK(empty.try_acquire()); CHECK(!empty.try_acquire());
    Semaphore limit(INT_MAX); check_throws<std::overflow_error>([&] { limit.release(); });
    check_throws<std::invalid_argument>([] { Semaphore invalid(UINT32_MAX); });
    // Non-atomic payload handoff must be ordered by release/acquire permits.
    Semaphore ready, consumed(1); int payload = 0;
    std::thread producer([&] { for (int i = 1; i <= 10000; ++i) { consumed.acquire(); payload = i; ready.release(); } });
    for (int i = 1; i <= 10000; ++i) { ready.acquire(); CHECK(payload == i); consumed.release(); }
    producer.join();
    Semaphore permits(3); std::atomic<int> inside{0}, peak{0}; threads.clear();
    for (int i = 0; i < 12; ++i) threads.emplace_back([&] {
        for (int j = 0; j < 500; ++j) {
            permits.acquire(); auto value = inside.fetch_add(1) + 1; CHECK(value <= 3);
            auto observed = peak.load(); while (observed < value && !peak.compare_exchange_weak(observed,value)) {}
            std::this_thread::yield(); inside.fetch_sub(1); permits.release();
        }
    });
    for (auto& thread : threads) thread.join(); CHECK(inside == 0); CHECK(peak > 0 && peak <= 3);
}
void sequential_and_aba() {
    TaggedStack stack(2); CHECK(!stack.pop()); CHECK(stack.push(1)); CHECK(stack.push(2)); CHECK(!stack.push(3));
    CHECK(stack.pop() == 2); CHECK(stack.pop() == 1); CHECK(!stack.pop());
    // A relative pointer returns to A, but its version differs. A stale CAS fails.
    std::atomic<std::uint64_t> head{7}; auto stale = head.load();
    head.store(detail::advance(head.load(),8)); head.store(detail::advance(head.load(),7));
    CHECK(detail::index(head.load()) == detail::index(stale));
    CHECK(!head.compare_exchange_strong(stale,detail::advance(stale,9)));
    CHECK(head.load() >> 16 == 2);
    HazardQueue queue(2); auto token = queue.participate();
    CHECK(!queue.dequeue(token)); CHECK(queue.enqueue(token,1)); CHECK(queue.enqueue(token,2)); CHECK(!queue.enqueue(token,3));
    CHECK(queue.dequeue(token) == 1); CHECK(queue.dequeue(token) == 2); CHECK(!queue.dequeue(token));
    for (int i = 0; i < 5000; ++i) { CHECK(queue.enqueue(token,i)); CHECK(queue.dequeue(token) == static_cast<unsigned>(i)); }
    queue.reclaim(); CHECK(queue.retired_count() == 5002); CHECK(queue.reclaimed_count() == 5002);
    HazardQueue one(1); auto single = one.participate();
    for (int i = 0; i < 100; ++i) { CHECK(one.enqueue(single,i)); CHECK(!one.enqueue(single,i)); CHECK(one.dequeue(single) == static_cast<unsigned>(i)); }
    check_throws<std::invalid_argument>([&] { one.dequeue(token); });
}
template<class Push, class Pop> void stress(Push push, Pop pop) {
    constexpr int producers = 4, per_producer = 10000, total = producers * per_producer;
    auto seen = std::make_unique<std::atomic<unsigned>[]>(total);
    std::atomic<int> consumed{0}; std::barrier start(8); std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) threads.emplace_back([&,p] {
        auto send = push(); start.arrive_and_wait();
        for (int i = 0; i < per_producer; ++i) while (!send(p * per_producer + i)) std::this_thread::yield();
    });
    for (int c = 0; c < 4; ++c) threads.emplace_back([&] {
        auto receive = pop(); start.arrive_and_wait();
        while (consumed.load() < total) {
            auto value = receive();
            if (value) { CHECK(*value < total); CHECK(seen[*value].fetch_add(1) == 0); consumed.fetch_add(1); }
            else std::this_thread::yield();
        }
    });
    for (auto& thread : threads) thread.join();
    for (int i = 0; i < total; ++i) CHECK(seen[i] == 1); CHECK(consumed == total);
}
void contention() {
    TaggedStack stack(127);
    stress([&] { return [&](auto value) { return stack.push(value); }; },[&] { return [&] { return stack.pop(); }; });
    HazardQueue queue(127);
    stress([&] { return [&,token = queue.participate()](auto value) mutable { return queue.enqueue(token,value); }; },
           [&] { return [&,token = queue.participate()]() mutable { return queue.dequeue(token); }; });
    queue.reclaim(); CHECK(queue.retired_count() == 40000); CHECK(queue.reclaimed_count() == 40000);
}
struct Operation {
    bool push = false;
    std::uint64_t argument = 0, begin = 0, end = 0;
    std::optional<std::uint64_t> result;
};
// Search all sequential histories allowed by real-time precedence, comparing
// observed results with an independent std::deque model. No implementation hooks.
bool linearizable(const std::vector<Operation>& history, bool fifo) {
    auto all = (1U << history.size()) - 1;
    std::vector<unsigned> predecessors(history.size());
    for (std::size_t i = 0; i < history.size(); ++i) for (std::size_t j = 0; j < history.size(); ++j)
        if (history[j].end < history[i].begin) predecessors[i] |= 1U << j;
    std::function<bool(unsigned,const std::deque<std::uint64_t>&)> search;
    search = [&](unsigned used, const std::deque<std::uint64_t>& state) {
        if (used == all) return true;
        for (std::size_t i = 0; i < history.size(); ++i) {
            if ((used & (1U << i)) || (predecessors[i] & ~used)) continue;
            auto next = state; const auto& op = history[i];
            if (op.push) next.push_back(op.argument);
            else if (!op.result) { if (!next.empty()) continue; }
            else {
                if (next.empty() || *op.result != (fifo ? next.front() : next.back())) continue;
                if (fifo) next.pop_front(); else next.pop_back();
            }
            if (search(used | (1U << i),next)) return true;
        }
        return false;
    };
    return search(0,{});
}
void histories() {
    for (bool fifo : {false,true}) for (unsigned repeat = 0; repeat < 40; ++repeat) {
        TaggedStack stack(64); HazardQueue queue(64); std::vector<Operation> history(12);
        std::atomic<std::uint64_t> clock{0}; std::barrier start(4); std::vector<std::thread> threads;
        for (unsigned worker = 0; worker < 4; ++worker) threads.emplace_back([&,worker] {
            auto token = queue.participate(); std::mt19937 random(repeat * 41 + worker); start.arrive_and_wait();
            for (unsigned n = 0; n < 3; ++n) {
                auto& op = history[worker * 3 + n]; op.push = random() % 2; op.argument = worker * 3 + n;
                op.begin = clock.fetch_add(1);
                if (op.push) CHECK(fifo ? queue.enqueue(token,op.argument) : stack.push(op.argument));
                else op.result = fifo ? queue.dequeue(token) : stack.pop();
                op.end = clock.fetch_add(1);
            }
        });
        for (auto& thread : threads) thread.join(); CHECK(linearizable(history,fifo));
    }
    std::vector<Operation> impossible{{true,1,0,1,{}},{false,0,2,3,2}};
    CHECK(!linearizable(impossible,true)); CHECK(!linearizable(impossible,false));
}
}
int main() {
    try { locks_and_semaphore(); sequential_and_aba(); contention(); histories();
        std::cout << "sync: 80000 protected increments, 10000 permit handoffs, 80000 exact-once items, 80 linearizable histories passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#pragma once
#include "os/tagged_pool.hpp"
#include <array>
#include <optional>
#include <utility>

namespace os::sync {
// Bounded Michael-Scott queue. Nodes live in a fixed arena; hazard pointers
// defer node reuse, which is the relevant reclamation operation for this arena.
class HazardQueue {
    static constexpr std::size_t max_threads = 64;
    struct Node : detail::PoolNode {
        std::atomic<std::uint16_t> next{detail::null_index};
        std::uint64_t value = 0;
    };
    struct alignas(64) Hazards {
        std::atomic<bool> active{false};
        std::atomic<std::uint16_t> first{detail::null_index}, second{detail::null_index};
    };
    static_assert(std::atomic<std::uint16_t>::is_always_lock_free);
    std::unique_ptr<Node[]> nodes_;
    std::array<Hazards,max_threads> hazards_;
    alignas(64) std::atomic<std::uint16_t> head_{0};
    alignas(64) std::atomic<std::uint16_t> tail_{0};
    detail::TaggedList free_, retired_;
    std::atomic<std::uint64_t> retired_count_{0}, reclaimed_count_{0};

    std::uint16_t protect(std::atomic<std::uint16_t>& source, std::atomic<std::uint16_t>& hazard) {
        std::uint16_t candidate;
        do { candidate = source.load(); hazard.store(candidate); } while (source.load() != candidate);
        return candidate;
    }
    void clear(std::size_t slot) {
        hazards_[slot].first.store(detail::null_index); hazards_[slot].second.store(detail::null_index);
    }
public:
    class Participant {
        friend class HazardQueue;
        HazardQueue* owner_;
        std::size_t slot_, since_scan_ = 0;
        Participant(HazardQueue& owner, std::size_t slot) : owner_(&owner), slot_(slot) {}
    public:
        Participant(const Participant&) = delete;
        Participant& operator=(const Participant&) = delete;
        Participant(Participant&& other) noexcept : owner_(std::exchange(other.owner_,nullptr)), slot_(other.slot_), since_scan_(other.since_scan_) {}
        Participant& operator=(Participant&&) = delete;
        ~Participant() {
            if (owner_) { owner_->clear(slot_); owner_->hazards_[slot_].active.store(false); }
        }
    };
    explicit HazardQueue(std::size_t capacity) {
        if (!capacity || capacity >= detail::null_index - 1) throw std::invalid_argument("queue capacity must be 1..65533");
        nodes_ = std::make_unique<Node[]>(capacity + 1);
        for (std::size_t i = 1; i <= capacity; ++i) free_.push(nodes_,i);
    }
    // A Participant belongs to one thread at a time and cannot outlive this queue.
    Participant participate() {
        for (std::size_t i = 0; i < max_threads; ++i) {
            bool expected = false;
            if (hazards_[i].active.compare_exchange_strong(expected,true)) return Participant(*this,i);
        }
        throw std::runtime_error("queue supports at most 64 registered participants");
    }
    void reclaim() {
        // Detach first, then read hazards. A reader that publishes later must
        // revalidate against head/tail before dereferencing the retired node.
        auto first = retired_.detach(); if (first == detail::null_index) return;
        std::array<std::uint16_t,2 * max_threads> protected_nodes{};
        for (std::size_t i = 0; i < max_threads; ++i) {
            protected_nodes[2 * i] = hazards_[i].first.load(); protected_nodes[2 * i + 1] = hazards_[i].second.load();
        }
        while (first != detail::null_index) {
            auto id = first; first = nodes_[id].link.load(std::memory_order_relaxed);
            bool protected_now = false;
            for (auto other : protected_nodes) protected_now = protected_now || id == other;
            if (protected_now) retired_.push(nodes_,id);
            else { free_.push(nodes_,id); reclaimed_count_.fetch_add(1,std::memory_order_relaxed); }
        }
    }
    bool enqueue(Participant& thread, std::uint64_t value) {
        if (thread.owner_ != this) throw std::invalid_argument("participant belongs to another queue");
        auto id = free_.pop(nodes_);
        if (id == detail::null_index) { reclaim(); id = free_.pop(nodes_); }
        if (id == detail::null_index) return false;
        nodes_[id].value = value; nodes_[id].next.store(detail::null_index);
        auto& hazard = hazards_[thread.slot_].first;
        while (true) {
            auto tail = protect(tail_,hazard); auto next = nodes_[tail].next.load();
            if (tail_.load() != tail) continue;
            if (next != detail::null_index) { tail_.compare_exchange_weak(tail,next); continue; }
            if (nodes_[tail].next.compare_exchange_weak(next,id)) {
                tail_.compare_exchange_strong(tail,id); clear(thread.slot_); return true;
            }
        }
    }
    std::optional<std::uint64_t> dequeue(Participant& thread) {
        if (thread.owner_ != this) throw std::invalid_argument("participant belongs to another queue");
        auto& hazard = hazards_[thread.slot_];
        while (true) {
            auto head = protect(head_,hazard.first); auto tail = tail_.load(); auto next = nodes_[head].next.load();
            hazard.second.store(next);
            if (head_.load() != head) continue;
            if (head == tail) {
                if (next == detail::null_index) { clear(thread.slot_); return std::nullopt; }
                tail_.compare_exchange_weak(tail,next); continue;
            }
            if (next == detail::null_index) continue;
            auto value = nodes_[next].value;
            if (head_.compare_exchange_weak(head,next)) {
                clear(thread.slot_); retired_.push(nodes_,head); retired_count_.fetch_add(1,std::memory_order_relaxed);
                if (++thread.since_scan_ >= 32) { reclaim(); thread.since_scan_ = 0; }
                return value;
            }
        }
    }
    std::uint64_t retired_count() const { return retired_count_.load(std::memory_order_relaxed); }
    std::uint64_t reclaimed_count() const { return reclaimed_count_.load(std::memory_order_relaxed); }
};
}

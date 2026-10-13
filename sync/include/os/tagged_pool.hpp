#pragma once
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>

namespace os::sync::detail {
// Relative pointer: low 16 bits are an index, high 48 bits are a generation.
// Nodes never move or disappear during an operation. Never wrap the generation.
constexpr std::uint16_t null_index = 65535;
constexpr std::uint64_t tag_limit = (1ULL << 48) - 1;
inline std::uint16_t index(std::uint64_t tagged) { return tagged & 65535; }
inline std::uint64_t advance(std::uint64_t previous, std::uint16_t next) {
    auto tag = previous >> 16;
    if (tag == tag_limit) std::terminate();
    return ((tag + 1) << 16) | next;
}
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
struct PoolNode {
    std::atomic<std::uint16_t> link{null_index};
};
class TaggedList {
    std::atomic<std::uint64_t> head_{null_index};
public:
    template<class Nodes> void push(Nodes& nodes, std::uint16_t id) {
        auto before = head_.load(std::memory_order_acquire);
        do { nodes[id].link.store(index(before),std::memory_order_relaxed); }
        while (!head_.compare_exchange_weak(before,advance(before,id),std::memory_order_acq_rel,std::memory_order_acquire));
    }
    template<class Nodes> std::uint16_t pop(Nodes& nodes) {
        auto before = head_.load(std::memory_order_acquire);
        while (index(before) != null_index) {
            auto id = index(before); auto next = nodes[id].link.load(std::memory_order_relaxed);
            if (head_.compare_exchange_weak(before,advance(before,next),std::memory_order_acq_rel,std::memory_order_acquire)) return id;
        }
        return null_index;
    }
    std::uint16_t detach() {
        auto before = head_.load(std::memory_order_acquire);
        do { if (index(before) == null_index) return null_index; }
        while (!head_.compare_exchange_weak(before,advance(before,null_index),std::memory_order_acq_rel,std::memory_order_acquire));
        return index(before);
    }
};
}

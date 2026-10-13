#pragma once
#include "os/tagged_pool.hpp"
#include <optional>

namespace os::sync {
class TaggedStack {
    struct Node : detail::PoolNode { std::uint64_t value = 0; };
    std::unique_ptr<Node[]> nodes_;
    detail::TaggedList free_, values_;
public:
    explicit TaggedStack(std::size_t capacity) {
        if (!capacity || capacity >= detail::null_index) throw std::invalid_argument("stack capacity must be 1..65534");
        nodes_ = std::make_unique<Node[]>(capacity);
        for (std::size_t i = 0; i < capacity; ++i) free_.push(nodes_,i);
    }
    bool push(std::uint64_t value) {
        auto id = free_.pop(nodes_); if (id == detail::null_index) return false;
        nodes_[id].value = value; values_.push(nodes_,id); return true;
    }
    std::optional<std::uint64_t> pop() {
        auto id = values_.pop(nodes_); if (id == detail::null_index) return std::nullopt;
        auto value = nodes_[id].value; free_.push(nodes_,id); return value;
    }
};
}

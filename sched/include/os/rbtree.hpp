#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace os::sched {
// An intrusive, index-based red-black tree. A task has at most one active node.
// The scheduler only needs insertion and removal of the least-vruntime task.
class RunTree {
    using Index = std::size_t;
    using Key = std::pair<long double, Index>;
    struct Node { Index left, right, parent; Key key{}; bool red = false, active = false; };
    std::vector<Node> nodes_;
    Index nil_, root_;
    std::size_t size_ = 0;
    void left_rotate(Index x) {
        Index y = nodes_[x].right;
        nodes_[x].right = nodes_[y].left;
        if (nodes_[y].left != nil_) nodes_[nodes_[y].left].parent = x;
        nodes_[y].parent = nodes_[x].parent;
        if (nodes_[x].parent == nil_) root_ = y;
        else if (x == nodes_[nodes_[x].parent].left) nodes_[nodes_[x].parent].left = y;
        else nodes_[nodes_[x].parent].right = y;
        nodes_[y].left = x; nodes_[x].parent = y;
    }
    void right_rotate(Index x) {
        Index y = nodes_[x].left;
        nodes_[x].left = nodes_[y].right;
        if (nodes_[y].right != nil_) nodes_[nodes_[y].right].parent = x;
        nodes_[y].parent = nodes_[x].parent;
        if (nodes_[x].parent == nil_) root_ = y;
        else if (x == nodes_[nodes_[x].parent].right) nodes_[nodes_[x].parent].right = y;
        else nodes_[nodes_[x].parent].left = y;
        nodes_[y].right = x; nodes_[x].parent = y;
    }
    void repair_insert(Index x) {
        while (nodes_[nodes_[x].parent].red) {
            auto parent = nodes_[x].parent;
            auto grandparent = nodes_[parent].parent;
            if (parent == nodes_[grandparent].left) {
                auto uncle = nodes_[grandparent].right;
                if (nodes_[uncle].red) {
                    nodes_[parent].red = false; nodes_[uncle].red = false;
                    nodes_[grandparent].red = true; x = grandparent;
                } else {
                    if (x == nodes_[parent].right) { x = parent; left_rotate(x); }
                    parent = nodes_[x].parent; grandparent = nodes_[parent].parent;
                    nodes_[parent].red = false; nodes_[grandparent].red = true;
                    right_rotate(grandparent);
                }
            } else {
                auto uncle = nodes_[grandparent].left;
                if (nodes_[uncle].red) {
                    nodes_[parent].red = false; nodes_[uncle].red = false;
                    nodes_[grandparent].red = true; x = grandparent;
                } else {
                    if (x == nodes_[parent].left) { x = parent; right_rotate(x); }
                    parent = nodes_[x].parent; grandparent = nodes_[parent].parent;
                    nodes_[parent].red = false; nodes_[grandparent].red = true;
                    left_rotate(grandparent);
                }
            }
        }
        nodes_[root_].red = false;
    }
    void repair_remove(Index x) {
        while (x != root_ && !nodes_[x].red) {
            auto parent = nodes_[x].parent;
            if (x == nodes_[parent].left) {
                auto sibling = nodes_[parent].right;
                if (nodes_[sibling].red) {
                    nodes_[sibling].red = false; nodes_[parent].red = true;
                    left_rotate(parent); sibling = nodes_[parent].right;
                }
                if (!nodes_[nodes_[sibling].left].red && !nodes_[nodes_[sibling].right].red) {
                    nodes_[sibling].red = true; x = parent;
                } else {
                    if (!nodes_[nodes_[sibling].right].red) {
                        nodes_[nodes_[sibling].left].red = false; nodes_[sibling].red = true;
                        right_rotate(sibling); sibling = nodes_[parent].right;
                    }
                    nodes_[sibling].red = nodes_[parent].red; nodes_[parent].red = false;
                    nodes_[nodes_[sibling].right].red = false; left_rotate(parent); x = root_;
                }
            } else {
                auto sibling = nodes_[parent].left;
                if (nodes_[sibling].red) {
                    nodes_[sibling].red = false; nodes_[parent].red = true;
                    right_rotate(parent); sibling = nodes_[parent].left;
                }
                if (!nodes_[nodes_[sibling].right].red && !nodes_[nodes_[sibling].left].red) {
                    nodes_[sibling].red = true; x = parent;
                } else {
                    if (!nodes_[nodes_[sibling].left].red) {
                        nodes_[nodes_[sibling].right].red = false; nodes_[sibling].red = true;
                        left_rotate(sibling); sibling = nodes_[parent].left;
                    }
                    nodes_[sibling].red = nodes_[parent].red; nodes_[parent].red = false;
                    nodes_[nodes_[sibling].left].red = false; right_rotate(parent); x = root_;
                }
            }
        }
        nodes_[x].red = false;
    }
    int validate_node(Index x, Index parent, std::optional<Key> low, std::optional<Key> high,
                      std::vector<bool>& seen) const {
        if (x == nil_) return 1;
        if (x >= nil_ || seen[x]) throw std::logic_error("tree cycle or invalid index");
        seen[x] = true;
        const auto& node = nodes_[x];
        if (!node.active || node.parent != parent || (low && node.key <= *low) || (high && node.key >= *high))
            throw std::logic_error("tree order or parent invariant");
        if (node.red && (nodes_[node.left].red || nodes_[node.right].red))
            throw std::logic_error("red parent with red child");
        int left = validate_node(node.left, x, low, node.key, seen);
        int right = validate_node(node.right, x, node.key, high, seen);
        if (left != right) throw std::logic_error("unequal black height");
        return left + (node.red ? 0 : 1);
    }
public:
    explicit RunTree(std::size_t capacity) : nodes_(capacity + 1), nil_(capacity), root_(nil_) {
        for (auto& node : nodes_) node.left = node.right = node.parent = nil_;
    }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    Key minimum() const {
        if (empty()) throw std::logic_error("empty tree");
        Index x = root_;
        while (nodes_[x].left != nil_) x = nodes_[x].left;
        return nodes_[x].key;
    }
    void insert(Index id, long double vruntime) {
        if (id >= nil_ || nodes_[id].active || !std::isfinite(vruntime))
            throw std::invalid_argument("invalid tree insertion");
        auto& node = nodes_[id];
        node.key = {vruntime, id}; node.left = node.right = nil_;
        Index parent = nil_, x = root_;
        while (x != nil_) { parent = x; x = node.key < nodes_[x].key ? nodes_[x].left : nodes_[x].right; }
        node.parent = parent; node.red = true; node.active = true;
        if (parent == nil_) root_ = id;
        else if (node.key < nodes_[parent].key) nodes_[parent].left = id;
        else nodes_[parent].right = id;
        ++size_; repair_insert(id);
    }
    Index pop_minimum() {
        Index id = minimum().second;
        auto& node = nodes_[id];
        Index replacement = node.right; // The minimum has no left child.
        nodes_[replacement].parent = node.parent;
        if (node.parent == nil_) root_ = replacement;
        else nodes_[node.parent].left = replacement;
        if (!node.red) repair_remove(replacement);
        node.active = false; --size_;
        return id;
    }
    void validate() const {
        if (nodes_[nil_].red || nodes_[root_].red) throw std::logic_error("black root and sentinel required");
        std::vector<bool> seen(nil_, false);
        validate_node(root_, nil_, std::nullopt, std::nullopt, seen);
        std::size_t count = 0;
        for (Index i = 0; i < nil_; ++i) { if (seen[i] != nodes_[i].active) throw std::logic_error("orphan tree node"); count += seen[i]; }
        if (count != size_) throw std::logic_error("tree size invariant");
    }
};
}

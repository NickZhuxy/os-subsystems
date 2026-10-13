#include "os/scheduler.hpp"
#include "os/rbtree.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <queue>
#include <set>
#include <stdexcept>

namespace os::sched {
namespace {
constexpr Time infinity = std::numeric_limits<Time>::max() / 4;
constexpr Time maximum_time = 1000000000000ULL;
enum class Status { future, ready, running, blocked, done };
enum class Reason { initial, wake, preempt, quantum };
struct State {
    Status status = Status::future;
    std::size_t burst = 0, level = 0;
    Time remaining = 0, ready_since = 0, slice_left = 0, dispatch_service = 0;
    long double vruntime = 0, weight = 1024;
};
class Policy {
protected:
    std::vector<State>& states;
    const Config& config;
    std::vector<bool> queued;
    std::size_t queued_count = 0;
    void add(std::size_t id) {
        if (queued.at(id)) throw std::logic_error("task queued twice");
        queued[id] = true; ++queued_count;
    }
    std::size_t remove(std::size_t id) {
        if (!queued.at(id)) throw std::logic_error("task absent from queue");
        queued[id] = false; --queued_count; return id;
    }
public:
    Policy(std::vector<State>& state, const Config& cfg) : states(state), config(cfg), queued(state.size()) {}
    virtual ~Policy() = default;
    virtual void enqueue(std::size_t, Reason) = 0;
    virtual std::optional<std::size_t> pick() = 0;
    virtual Time budget(std::size_t id) const { return states[id].slice_left; }
    virtual void account(std::size_t id, Time duration) { states[id].slice_left -= duration; }
    virtual bool preempt(std::size_t) const { return false; }
    virtual void leave(std::size_t) {}
    virtual bool boost() { return false; }
    virtual Time boost_period() const { return infinity; }
    virtual void validate() const {
        std::size_t count = 0;
        for (std::size_t i = 0; i < states.size(); ++i) {
            if (queued[i] != (states[i].status == Status::ready)) throw std::logic_error("queue/state mismatch");
            count += queued[i];
        }
        if (count != queued_count) throw std::logic_error("queue count mismatch");
    }
};
class Baseline final : public Policy {
    std::deque<std::size_t> queue;
    bool round_robin;
public:
    Baseline(std::vector<State>& state, const Config& cfg, bool rr) : Policy(state, cfg), round_robin(rr) {}
    void enqueue(std::size_t id, Reason) override { add(id); queue.push_back(id); }
    std::optional<std::size_t> pick() override {
        if (queue.empty()) return std::nullopt;
        auto id = queue.front(); queue.pop_front(); remove(id);
        states[id].slice_left = round_robin ? config.rr_quantum : infinity;
        return id;
    }
};
class Mlfq final : public Policy {
    std::vector<std::deque<std::size_t>> queues;
    void demote(std::size_t id) {
        auto& s = states[id];
        s.level = std::min(s.level + 1, queues.size() - 1);
        s.slice_left = config.mlfq_quantums[s.level];
    }
public:
    Mlfq(std::vector<State>& state, const Config& cfg) : Policy(state, cfg), queues(cfg.mlfq_quantums.size()) {
        for (auto& s : states) s.slice_left = cfg.mlfq_quantums[0];
    }
    void enqueue(std::size_t id, Reason reason) override {
        if (reason == Reason::quantum) demote(id);
        add(id); queues[states[id].level].push_back(id);
    }
    std::optional<std::size_t> pick() override {
        for (auto& queue : queues) if (!queue.empty()) {
            auto id = queue.front(); queue.pop_front(); return remove(id);
        }
        return std::nullopt;
    }
    bool preempt(std::size_t id) const override {
        for (std::size_t level = 0; level < states[id].level; ++level)
            if (!queues[level].empty()) return true;
        return false;
    }
    void leave(std::size_t id) override { if (states[id].slice_left == 0) demote(id); }
    Time boost_period() const override { return config.boost_period ? config.boost_period : infinity; }
    bool boost() override {
        for (std::size_t level = 1; level < queues.size(); ++level) {
            queues[0].insert(queues[0].end(), queues[level].begin(), queues[level].end());
            queues[level].clear();
        }
        for (auto& s : states) { s.level = 0; s.slice_left = config.mlfq_quantums[0]; }
        return true;
    }
};
class Cfs final : public Policy {
    RunTree tree;
    long double ready_weight = 0, floor = 0;
public:
    Cfs(std::vector<State>& state, const Config& cfg) : Policy(state, cfg), tree(state.size()) {}
    void enqueue(std::size_t id, Reason reason) override {
        auto& s = states[id];
        if (reason == Reason::initial || reason == Reason::wake) s.vruntime = std::max(s.vruntime, floor);
        add(id); tree.insert(id, s.vruntime); ready_weight += s.weight;
    }
    std::optional<std::size_t> pick() override {
        if (tree.empty()) return std::nullopt;
        auto id = tree.pop_minimum(); remove(id);
        auto& s = states[id];
        auto period = std::max(config.target_latency, (tree.size() + 1) * config.min_granularity);
        auto slice = static_cast<Time>(std::ceil(period * s.weight / ready_weight));
        s.slice_left = std::max(config.min_granularity, slice);
        s.dispatch_service = 0; ready_weight -= s.weight;
        if (tree.empty()) ready_weight = 0; // Avoid accumulating floating-point residue.
        return id;
    }
    void account(std::size_t id, Time duration) override {
        auto& s = states[id]; s.slice_left -= duration; s.dispatch_service += duration;
        s.vruntime += duration * 1024.0L / s.weight;
        auto minimum = tree.empty() ? s.vruntime : std::min(s.vruntime, tree.minimum().first);
        floor = std::max(floor, minimum);
    }
    bool preempt(std::size_t id) const override {
        return !tree.empty() && states[id].dispatch_service >= config.min_granularity &&
            tree.minimum().first < states[id].vruntime;
    }
    Time budget(std::size_t id) const override {
        const auto& s = states[id];
        if (!tree.empty() && tree.minimum().first < s.vruntime && s.dispatch_service < config.min_granularity)
            return std::min(s.slice_left, config.min_granularity - s.dispatch_service);
        return s.slice_left;
    }
    void leave(std::size_t) override { if (!tree.empty()) floor = std::max(floor, tree.minimum().first); }
    void validate() const override { Policy::validate(); tree.validate(); if (tree.size() != queued_count) throw std::logic_error("CFS size mismatch"); }
};
}
long double nice_weight(int nice) {
    if (nice < -20 || nice > 19) throw std::invalid_argument("nice must be in [-20, 19]");
    return 1024.0L * std::pow(1.25L, -nice);
}
Result simulate(const std::vector<Process>& workload, const std::string& name, const Config& config) {
    if (!config.horizon || config.horizon > maximum_time || !config.rr_quantum ||
        config.rr_quantum > maximum_time || config.mlfq_quantums.empty() ||
        config.boost_period > maximum_time || !config.target_latency || config.target_latency > maximum_time ||
        !config.min_granularity || config.min_granularity > maximum_time || !config.starvation_threshold ||
        workload.size() > 100000)
        throw std::invalid_argument("invalid scheduler configuration");
    for (auto quantum : config.mlfq_quantums)
        if (!quantum || quantum > maximum_time) throw std::invalid_argument("invalid MLFQ quantum");
    std::set<std::string> names;
    std::vector<State> states(workload.size());
    Result result; result.policy = name;
    using Event = std::pair<Time, std::size_t>;
    std::priority_queue<Event, std::vector<Event>, std::greater<>> events;
    for (std::size_t i = 0; i < workload.size(); ++i) {
        const auto& p = workload[i];
        if (p.name.empty() || !names.insert(p.name).second || p.arrival > maximum_time ||
            p.bursts.empty() || p.bursts.size() % 2 == 0)
            throw std::invalid_argument("invalid process description");
        Time total = 0;
        for (auto burst : p.bursts) {
            if (!burst || burst > maximum_time || total > maximum_time - burst)
                throw std::invalid_argument("invalid burst length");
            total += burst;
        }
        states[i].remaining = p.bursts.front(); states[i].weight = nice_weight(p.nice);
        ProcessResult output; output.name = p.name; output.arrival = p.arrival; output.weight = states[i].weight;
        result.processes.push_back(output); events.emplace(p.arrival, i);
    }
    std::unique_ptr<Policy> policy;
    if (name == "fifo" || name == "rr") policy = std::make_unique<Baseline>(states, config, name == "rr");
    else if (name == "mlfq") policy = std::make_unique<Mlfq>(states, config);
    else if (name == "cfs") policy = std::make_unique<Cfs>(states, config);
    else throw std::invalid_argument("unknown scheduler");
    Time now = 0, next_boost = policy->boost_period();
    std::optional<std::size_t> running;
    auto requeue = [&](Reason reason) {
        auto id = *running; auto& state = states[id];
        state.status = Status::ready; state.ready_since = now;
        policy->enqueue(id, reason); running.reset();
    };
    // Resolve the CPU event, then arrivals/wakeups, then boost and preemption.
    // This deterministic tie order is part of the simulator's contract.
    while (now < config.horizon) {
        if (running) {
            auto id = *running; auto& s = states[id];
            if (s.remaining == 0) {
                policy->leave(id);
                if (s.burst + 1 == workload[id].bursts.size()) {
                    s.status = Status::done; result.processes[id].completion = now; ++result.completed;
                } else {
                    events.emplace(now + workload[id].bursts[s.burst + 1], id);
                    s.burst += 2; s.remaining = workload[id].bursts[s.burst]; s.status = Status::blocked;
                }
                running.reset();
            } else if (s.slice_left == 0) requeue(Reason::quantum);
        }
        while (!events.empty() && events.top().first == now) {
            auto id = events.top().second; events.pop(); auto& s = states[id];
            auto reason = s.status == Status::future ? Reason::initial : Reason::wake;
            s.status = Status::ready; s.ready_since = now; policy->enqueue(id, reason);
        }
        bool boosted = false;
        if (now == next_boost) { boosted = policy->boost(); next_boost += policy->boost_period(); }
        if (running && (boosted || policy->preempt(*running))) requeue(Reason::preempt);
        if (!running) {
            running = policy->pick();
            if (running) {
                auto id = *running; auto& s = states[id]; auto& p = result.processes[id];
                s.status = Status::running;
                auto wait = now - s.ready_since; p.ready_wait += wait; p.max_ready_wait = std::max(p.max_ready_wait, wait);
                if (!p.first_run) p.first_run = now;
                ++result.dispatches;
            }
        }
        if (config.verify) policy->validate();
        if (!running) {
            if (events.empty()) break;
            auto next = std::min(config.horizon, events.top().first);
            if (next_boost < next) {
                policy->boost();
                next_boost = ((next - 1) / policy->boost_period() + 1) * policy->boost_period();
            }
            result.idle += next - now; now = next; continue;
        }
        auto id = *running; auto& s = states[id];
        Time end = std::min({config.horizon, now + s.remaining, now + policy->budget(id), next_boost,
            events.empty() ? infinity : events.top().first});
        if (end <= now) throw std::logic_error("simulation made no progress");
        auto duration = end - now;
        policy->account(id, duration); s.remaining -= duration;
        result.processes[id].service += duration; result.busy += duration;
        if (config.trace) {
            if (!result.trace.empty() && result.trace.back().process == id && result.trace.back().end == now)
                result.trace.back().end = end;
            else result.trace.push_back({id, now, end});
        }
        now = end;
    }
    // A completion exactly on the observation boundary still counts.
    if (running && states[*running].remaining == 0 && states[*running].burst + 1 == workload[*running].bursts.size()) {
        states[*running].status = Status::done; result.processes[*running].completion = now; ++result.completed;
    }
    result.elapsed = now;
    std::size_t started = 0, eligible = 0;
    long double sum = 0, squares = 0;
    for (std::size_t i = 0; i < states.size(); ++i) {
        auto& p = result.processes[i];
        if (states[i].status == Status::ready) {
            auto wait = now - states[i].ready_since; p.ready_wait += wait; p.max_ready_wait = std::max(p.max_ready_wait, wait);
        }
        if (p.first_run) { result.mean_response += *p.first_run - p.arrival; ++started; }
        if (p.completion) result.mean_turnaround += *p.completion - p.arrival;
        if (p.max_ready_wait >= config.starvation_threshold) ++result.starved;
        if (p.arrival < now) { auto service = p.service / p.weight; sum += service; squares += service * service; ++eligible; }
    }
    if (started) result.mean_response /= started;
    if (result.completed) result.mean_turnaround /= result.completed;
    if (squares > 0) result.jain_weighted_service = static_cast<double>(sum * sum / (eligible * squares));
    if (result.busy + result.idle != result.elapsed) throw std::logic_error("CPU-time conservation failed");
    return result;
}
std::vector<Process> standard_workload(const std::string& name) {
    std::vector<Process> result;
    if (name == "mixed") {
        for (int i = 0; i < 4; ++i) result.push_back({"cpu" + std::to_string(i), 0, 0, {80000}});
        for (int i = 0; i < 8; ++i) {
            Process p{"interactive" + std::to_string(i), static_cast<Time>(i * 500), 0, {}};
            for (int burst = 0; burst < 20; ++burst) {
                if (burst) p.bursts.push_back(4500);
                p.bursts.push_back(500);
            }
            result.push_back(std::move(p));
        }
    } else if (name == "equal") {
        for (int i = 0; i < 8; ++i) result.push_back({"cpu" + std::to_string(i), 0, 0, {100000000}});
    } else if (name == "weighted") {
        for (int nice : {-5, 0, 5, 10}) result.push_back({"nice" + std::to_string(nice), 0, nice, {100000000}});
    } else throw std::invalid_argument("unknown workload");
    return result;
}
}

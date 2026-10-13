#include "os/scheduler.hpp"
#include "os/rbtree.hpp"
#include "os/test.hpp"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
using namespace os::sched;
namespace {
void tree_invariants() {
    for (bool reverse : {false, true}) {
        RunTree tree(257);
        for (std::size_t i = 0; i < 257; ++i) { auto id = reverse ? 256 - i : i; tree.insert(id, id); tree.validate(); }
        for (std::size_t i = 0; i < 257; ++i) { CHECK(tree.pop_minimum() == i); tree.validate(); }
        CHECK(tree.empty());
    }
    RunTree tree(128);
    std::set<std::pair<long double, std::size_t>> reference;
    std::vector<bool> active(128, false);
    std::mt19937 generator(4021);
    for (int operation = 0; operation < 20000; ++operation) {
        auto id = static_cast<std::size_t>(generator() % active.size());
        if (!active[id] && (generator() % 3 || reference.empty())) {
            long double key = generator() % 100;
            tree.insert(id, key); reference.emplace(key, id); active[id] = true;
            check_throws<std::invalid_argument>([&] { tree.insert(id, key); });
        } else if (!reference.empty()) {
            CHECK(tree.minimum() == *reference.begin());
            auto removed = tree.pop_minimum(); CHECK(removed == reference.begin()->second);
            active[removed] = false; reference.erase(reference.begin());
        }
        tree.validate(); CHECK(tree.size() == reference.size());
    }
}
void exact_schedules() {
    Config config; config.rr_quantum = 2; config.trace = config.verify = true;
    auto fifo = simulate({{"a",0,0,{5}}, {"b",0,0,{3}}}, "fifo", config);
    CHECK(fifo.elapsed == 8 && fifo.busy == 8 && fifo.idle == 0);
    CHECK(fifo.processes[0].completion == 5 && fifo.processes[1].first_run == 5);
    CHECK(fifo.mean_response == 2.5 && fifo.mean_turnaround == 6.5);
    auto rr = simulate({{"a",0,0,{5}}, {"b",0,0,{3}}}, "rr", config);
    CHECK(rr.processes[0].completion == 8 && rr.processes[1].completion == 7);
    CHECK(rr.processes[0].ready_wait == 3 && rr.processes[1].ready_wait == 4);
    CHECK(rr.trace.size() == 5);
    auto io = simulate({{"io",3,0,{2,7,3}}}, "fifo", config);
    CHECK(io.elapsed == 15 && io.busy == 5 && io.idle == 10);
    CHECK(io.processes[0].ready_wait == 0 && io.processes[0].first_run == 3);
    config.horizon = 8;
    CHECK(simulate({{"a",0,0,{8}}}, "fifo", config).completed == 1);
    CHECK(simulate({{"a",8,0,{1}}}, "fifo", config).processes[0].service == 0);
    CHECK(simulate({}, "cfs", config).elapsed == 0);
}
void mlfq_rules() {
    Config config; config.mlfq_quantums = {2,4,8}; config.boost_period = 0;
    config.trace = config.verify = true;
    auto result = simulate({{"a",0,0,{10}}, {"b",0,0,{1}}}, "mlfq", config);
    CHECK(result.processes[1].first_run == 2 && result.processes[0].completion == 11);
    auto preempt = simulate({{"cpu",0,0,{30}}, {"arrival",3,0,{1}}}, "mlfq", config);
    CHECK(preempt.processes[1].first_run == 3);
    auto gamer = simulate({{"io",0,0,{1,1,1,1,1,1,1}}, {"cpu",0,0,{30}}}, "mlfq", config);
    std::vector<Time> starts;
    for (auto segment : gamer.trace) if (segment.process == 0) starts.push_back(segment.begin);
    CHECK(starts.size() == 4 && starts[0] == 0 && starts[1] == 3 && starts[2] == 8);
    std::vector<Process> busy{{"long",0,0,{1000}}};
    for (Time t = 2; t < 100; t += 2) busy.push_back({"short" + std::to_string(t),t,0,{2}});
    config.horizon = 100; config.starvation_threshold = 20;
    auto without = simulate(busy, "mlfq", config);
    config.boost_period = 10;
    auto with = simulate(busy, "mlfq", config);
    CHECK(with.processes[0].service > without.processes[0].service);
    CHECK(with.processes[0].max_ready_wait < without.processes[0].max_ready_wait);
}
void cfs_fairness() {
    Config config; config.horizon = 50000; config.min_granularity = 5;
    config.target_latency = 80; config.verify = true;
    auto equal = simulate(standard_workload("equal"), "cfs", config);
    CHECK(equal.jain_weighted_service > .9999 && equal.completed == 0);
    auto weighted = simulate(standard_workload("weighted"), "cfs", config);
    CHECK(weighted.jain_weighted_service > .9999);
    std::vector<long double> normalized;
    for (const auto& p : weighted.processes) normalized.push_back(p.service / p.weight);
    auto [low, high] = std::minmax_element(normalized.begin(), normalized.end());
    CHECK(*high / *low < 1.02L);
    CHECK(weighted.processes[0].service > weighted.processes[1].service);
    auto arrival = simulate({{"cpu",0,0,{10000}}, {"new",500,0,{5}}}, "cfs", config);
    CHECK(*arrival.processes[1].first_run - 500 <= config.target_latency);
}
void randomized_conservation() {
    std::mt19937 random(29);
    Config config; config.rr_quantum = 3; config.mlfq_quantums = {2,4,8}; config.boost_period = 20;
    config.target_latency = 8; config.min_granularity = 1; config.verify = config.trace = true;
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<Process> workload;
        for (int id = 0; id < 8; ++id) {
            Process p{"p" + std::to_string(id), random() % 15, static_cast<int>(random() % 40) - 20, {}};
            unsigned count = 1 + random() % 4;
            for (unsigned i = 0; i < count * 2 - 1; ++i) p.bursts.push_back(1 + random() % 10);
            workload.push_back(p);
        }
        for (auto policy : {"fifo", "rr", "mlfq", "cfs"}) {
            auto result = simulate(workload, policy, config);
            CHECK(result.completed == workload.size());
            Time total_cpu = 0;
            for (std::size_t i = 0; i < workload.size(); ++i) {
                Time cpu = 0, io = 0;
                for (std::size_t b = 0; b < workload[i].bursts.size(); ++b)
                    (b % 2 ? io : cpu) += workload[i].bursts[b];
                const auto& p = result.processes[i];
                CHECK(p.service == cpu && *p.first_run >= p.arrival);
                CHECK(*p.completion - p.arrival == p.ready_wait + cpu + io);
                total_cpu += cpu;
            }
            CHECK(total_cpu == result.busy && result.busy + result.idle == result.elapsed);
            Time previous = 0, traced = 0;
            for (auto segment : result.trace) {
                CHECK(segment.begin >= previous && segment.end > segment.begin);
                traced += segment.end - segment.begin; previous = segment.end;
            }
            CHECK(traced == result.busy);
        }
    }
}
void invalid_inputs() {
    check_throws<std::invalid_argument>([] { simulate({{"p",0,0,{0}}}, "fifo"); });
    check_throws<std::invalid_argument>([] { simulate({{"p",0,20,{1}}}, "fifo"); });
    check_throws<std::invalid_argument>([] { simulate({{"p",0,0,{1,1}}}, "fifo"); });
    check_throws<std::invalid_argument>([] { simulate({{"p",0,0,{1}}, {"p",1,0,{1}}}, "fifo"); });
    check_throws<std::invalid_argument>([] { simulate({}, "unknown"); });
    Config cfg; cfg.mlfq_quantums = {};
    check_throws<std::invalid_argument>([&] { simulate({}, "mlfq", cfg); });
}
}
int main() {
    for (auto [name, test] : std::vector<std::pair<const char*, void(*)()>>{
        {"red-black invariants", tree_invariants}, {"exact schedules", exact_schedules},
        {"MLFQ rules", mlfq_rules}, {"CFS fairness", cfs_fairness},
        {"randomized conservation", randomized_conservation}, {"invalid input", invalid_inputs}}) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; return 1; }
    }
}

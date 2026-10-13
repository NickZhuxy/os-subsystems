#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace os::sched {
using Time = std::uint64_t; // Simulated microseconds, not elapsed wall time.
struct Process {
    std::string name;
    Time arrival = 0;
    int nice = 0;
    std::vector<Time> bursts; // CPU, I/O, CPU, ..., CPU.
};
struct Config {
    Time rr_quantum = 2000;
    std::vector<Time> mlfq_quantums{1000, 4000, 8000};
    Time boost_period = 20000;
    Time target_latency = 8000;
    Time min_granularity = 500;
    Time horizon = 10000000;
    Time starvation_threshold = 50000;
    bool verify = false;
    bool trace = false;
};
struct ProcessResult {
    std::string name;
    Time arrival = 0, service = 0, ready_wait = 0, max_ready_wait = 0;
    std::optional<Time> first_run, completion;
    long double weight = 1024;
};
struct Segment { std::size_t process; Time begin, end; };
struct Result {
    std::string policy;
    Time elapsed = 0, busy = 0, idle = 0;
    std::size_t dispatches = 0, completed = 0, starved = 0;
    double mean_response = 0, mean_turnaround = 0, jain_weighted_service = 0;
    std::vector<ProcessResult> processes;
    std::vector<Segment> trace;
};
long double nice_weight(int nice);
Result simulate(const std::vector<Process>& workload, const std::string& policy, const Config& config = {});
std::vector<Process> standard_workload(const std::string& name);
}

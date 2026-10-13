#include "os/scheduler.hpp"
#include "os/cli.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
int main(int argc, char** argv) {
    try {
        os::Arguments arguments(argc, argv);
        auto workload_name = arguments.take("--workload", "mixed");
        auto policy = arguments.take("--policy", "mlfq");
        auto output_prefix = arguments.take("--output");
        os::sched::Config config;
        config.horizon = arguments.number("--horizon", workload_name == "mixed" ? 10000000 : 500000, 1, 1000000000000ULL);
        config.trace = !output_prefix.empty(); config.verify = true;
        arguments.finish();
        auto workload = os::sched::standard_workload(workload_name);
        auto result = os::sched::simulate(workload, policy, config);
        if (!output_prefix.empty()) {
            std::ofstream processes(output_prefix + "-processes.csv"), trace(output_prefix + "-trace.csv"), input(output_prefix + "-input.csv");
            processes.exceptions(std::ios::badbit | std::ios::failbit);
            trace.exceptions(std::ios::badbit | std::ios::failbit);
            input.exceptions(std::ios::badbit | std::ios::failbit);
            processes << "name,arrival_us,weight,service_us,first_run_us,completion_us,ready_wait_us,max_ready_wait_us\n" << std::setprecision(15);
            for (const auto& p : result.processes) {
                processes << p.name << ',' << p.arrival << ',' << p.weight << ',' << p.service << ',';
                if (p.first_run) processes << *p.first_run;
                processes << ',';
                if (p.completion) processes << *p.completion;
                processes << ',' << p.ready_wait << ',' << p.max_ready_wait << '\n';
            }
            trace << "process,begin_us,end_us\n";
            for (auto segment : result.trace) trace << segment.process << ',' << segment.begin << ',' << segment.end << '\n';
            input << "name,arrival_us,nice,burst_index,kind,duration_us\n";
            for (const auto& p : workload) for (std::size_t i = 0; i < p.bursts.size(); ++i)
                input << p.name << ',' << p.arrival << ',' << p.nice << ',' << i << ',' << (i % 2 ? "io" : "cpu") << ',' << p.bursts[i] << '\n';
        }
        std::cout << std::setprecision(12) << "{\"policy\":\"" << policy << "\",\"workload\":\"" << workload_name
            << "\",\"processes\":" << workload.size() << ",\"horizon_us\":" << config.horizon
            << ",\"elapsed_us\":" << result.elapsed << ",\"busy_us\":" << result.busy << ",\"idle_us\":" << result.idle
            << ",\"completed\":" << result.completed << ",\"dispatches\":" << result.dispatches
            << ",\"mean_response_us\":" << result.mean_response << ",\"mean_turnaround_us\":";
        if (result.completed) std::cout << result.mean_turnaround; else std::cout << "null";
        std::cout << ",\"jain_weighted_service\":" << result.jain_weighted_service
            << ",\"starvation_threshold_us\":" << config.starvation_threshold << ",\"starved_processes\":" << result.starved << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

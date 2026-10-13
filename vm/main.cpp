#include "os/vm.hpp"
#include "os/cli.hpp"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <unordered_map>
int main(int argc, char** argv) {
    try {
        os::Arguments args(argc, argv); os::vm::Config config;
        auto name = args.take("--policy", "clock"); config.policy = os::vm::replacement(name);
        auto workload = args.take("--trace", "looping");
        config.frames = args.number("--frames", 32, 1, 65536);
        config.tlb_entries = args.number("--tlb", 16, 0, 4096);
        config.aging_period = args.number("--aging-period", 8, 1, 1000000);
        auto count = args.number("--accesses", 65536, 1, 10000000);
        auto seed = static_cast<std::uint32_t>(args.number("--seed", 42, 0, 0xffffffff));
        auto dump = args.take("--dump-trace"), input_path = args.take("--trace-file"); args.finish();
        std::vector<os::vm::Access> trace;
        if (!input_path.empty()) {
            std::ifstream input(input_path);
            if (!input) throw std::invalid_argument("cannot open trace");
            trace = os::vm::read_trace(input); workload = "file";
        } else trace = os::vm::trace(workload, count, seed);
        if (!dump.empty()) {
            std::ofstream file(dump); file.exceptions(std::ios::badbit | std::ios::failbit);
            file << "address,operation,value\n";
            for (auto access : trace) {
                file << access.address << ',' << (access.value ? "write" : "read") << ',';
                if (access.value) file << static_cast<unsigned>(*access.value);
                file << '\n';
            }
        }
        os::vm::Memory memory(config);
        std::unordered_map<std::uint32_t, std::uint8_t> oracle;
        std::uint64_t checksum = 0;
        auto begin = std::chrono::steady_clock::now();
        for (auto access : trace) {
            if (access.value) { memory.write(access.address, *access.value); oracle[access.address] = *access.value; }
            else {
                auto actual = memory.read(access.address);
                if (actual != oracle[access.address]) throw std::runtime_error("VM byte mismatch");
                checksum += actual;
            }
        }
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        memory.validate(); const auto& s = memory.statistics();
        std::cout << std::setprecision(12) << "{\"policy\":\"" << name << "\",\"trace\":\"" << workload
            << "\",\"frames\":" << config.frames << ",\"tlb_entries\":" << config.tlb_entries
            << ",\"aging_period\":" << config.aging_period << ",\"seed\":" << seed
            << ",\"accesses\":" << s.accesses << ",\"page_faults\":" << s.page_faults
            << ",\"page_fault_rate\":" << static_cast<double>(s.page_faults) / s.accesses
            << ",\"tlb_hits\":" << s.tlb_hits << ",\"tlb_hit_rate\":" << static_cast<double>(s.tlb_hits) / s.accesses
            << ",\"first_touches\":" << s.first_touches << ",\"page_tables\":" << s.page_tables
            << ",\"resident_pages\":" << s.resident_pages << ",\"evictions\":" << s.evictions
            << ",\"swap_reads\":" << s.swap_reads << ",\"swap_writes\":" << s.swap_writes
            << ",\"swap_bytes_written\":" << s.swap_bytes_written << ",\"checksum\":" << checksum
            << ",\"elapsed_seconds_including_oracle\":" << elapsed << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

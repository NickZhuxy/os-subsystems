#include "os/vm.hpp"
#include "os/fs.hpp"
#include "os/cli.hpp"
#include <valgrind/cachegrind.h>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <iostream>

namespace {
using Clock = std::chrono::steady_clock;
struct Result { double seconds; std::uint64_t checksum, accesses, block_reads; };
Result vm(std::uint64_t iterations) {
    os::vm::Memory memory({512,16,8,os::vm::Replacement::aging});
    for (std::uint32_t page = 0; page < 512; ++page) memory.write(page * os::vm::page_size,page % 251);
    std::uint64_t checksum = 0; auto start = Clock::now();
    CACHEGRIND_START_INSTRUMENTATION;
    for (std::uint64_t i = 0; i < iterations; ++i)
        checksum += memory.read(static_cast<std::uint32_t>((i * 257 % 512) * os::vm::page_size));
    CACHEGRIND_STOP_INSTRUMENTATION;
    auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::uint64_t expected = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) expected += (i * 257 % 512) % 251;
    memory.validate();
    if (checksum != expected || memory.statistics().page_faults != 512 || memory.statistics().accesses != iterations + 512)
        throw std::runtime_error("VM workload validation failed");
    return {seconds,checksum,iterations,0};
}
Result fs(std::uint64_t iterations) {
    char name[] = "/tmp/os-hotspot-XXXXXX"; auto created = mkdtemp(name);
    if (!created) os::system_error("hotspot temporary directory");
    struct Cleanup { std::string path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{created};
    auto path = cleanup.path + "/image"; os::fs::format(path,8192,256);
    os::fs::Image image(path);
    for (int i = 0; i < 64; ++i) {
        auto handle = image.create_open("/f" + std::to_string(i),0644,O_WRONLY);
        image.write(handle,0,os::fs::Bytes{static_cast<std::uint8_t>(i)}); image.release(handle);
    }
    auto expected = image.space(); auto before = image.io_statistics();
    if (expected.free_blocks != 8115 || expected.free_inodes != 190) throw std::runtime_error("unexpected formatted allocation");
    std::uint64_t checksum = 0; auto start = Clock::now();
    CACHEGRIND_START_INSTRUMENTATION;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        auto space = image.space(); checksum += space.free_blocks + space.free_inodes;
    }
    CACHEGRIND_STOP_INSTRUMENTATION;
    auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    if (checksum != iterations * (expected.free_blocks + expected.free_inodes)) throw std::runtime_error("filesystem workload validation failed");
    return {seconds,checksum,iterations,image.io_statistics().block_reads - before.block_reads};
}
}
int main(int argc, char** argv) {
    try {
        os::Arguments args(argc,argv); auto component = args.take("--component","vm");
        auto iterations = args.number("--iterations",200000,1,10000000); args.finish();
        auto result = component == "vm" ? vm(iterations) : component == "fs" ? fs(iterations)
            : throw std::invalid_argument("component must be vm or fs");
        std::cout.precision(12);
        std::cout << "{\"component\":\"" << component << "\",\"iterations\":" << iterations << ",\"seconds\":" << result.seconds
            << ",\"checksum\":" << result.checksum << ",\"accesses\":" << result.accesses
            << ",\"block_reads\":" << result.block_reads << ",\"validated\":true}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

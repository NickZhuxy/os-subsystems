#pragma once
#include "os/fd.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace os::vm {
constexpr std::size_t page_size = 4096;
enum class Replacement { fifo, clock, aging };
struct Config {
    std::size_t frames = 32;
    std::size_t tlb_entries = 16;
    std::uint64_t aging_period = 8;
    Replacement policy = Replacement::clock;
};
struct Statistics {
    std::uint64_t accesses = 0, page_faults = 0, first_touches = 0;
    std::uint64_t tlb_hits = 0, tlb_misses = 0, evictions = 0;
    std::uint64_t swap_reads = 0, swap_writes = 0, swap_bytes_written = 0;
    std::size_t resident_pages = 0, page_tables = 0;
};
class Memory {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Memory(Config config = {});
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    std::uint8_t read(std::uint32_t address);
    void write(std::uint32_t address, std::uint8_t value);
    const Statistics& statistics() const;
    bool resident(std::uint32_t page) const;
    void validate() const;
};
struct Access { std::uint32_t address; std::optional<std::uint8_t> value; };
std::vector<Access> trace(const std::string& name, std::size_t count, std::uint32_t seed);
std::vector<Access> read_trace(std::istream& stream);
Replacement replacement(const std::string& name);
}

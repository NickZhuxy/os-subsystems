#include "os/vm.hpp"
#include "os/test.hpp"
#include <iostream>
#include <random>
#include <sstream>
#include <unordered_map>
using namespace os::vm;
namespace {
void demand_and_tlb() {
    Config config; config.frames = 2; config.tlb_entries = 2;
    Memory memory(config);
    CHECK(memory.statistics().resident_pages == 0 && memory.statistics().page_tables == 0);
    CHECK(memory.read(0) == 0 && memory.statistics().page_faults == 1);
    CHECK(memory.read(4095) == 0 && memory.statistics().tlb_hits == 1);
    memory.write(4095, 42);
    CHECK(memory.read(4095) == 42 && memory.statistics().page_faults == 1);
    CHECK(memory.read(4096 * 1024) == 0 && memory.statistics().page_tables == 2);
    CHECK(memory.read(0xffffffff) == 0 && memory.statistics().page_tables == 3);
    CHECK(memory.statistics().resident_pages == 2 && memory.statistics().evictions == 1);
    CHECK(memory.read(4095) == 42);
    CHECK(memory.statistics().swap_writes == 1 && memory.statistics().swap_reads == 1);
    memory.validate();
    config.tlb_entries = 0; Memory uncached(config);
    for (int i = 0; i < 10; ++i) CHECK(uncached.read(0) == 0);
    CHECK(uncached.statistics().tlb_hits == 0 && uncached.statistics().tlb_misses == 10);
    CHECK(uncached.statistics().page_faults == 1);
}
void belady_anomaly() {
    std::vector<std::uint64_t> faults;
    for (auto count : {3U,4U}) {
        Config config; config.policy = Replacement::fifo; config.frames = count;
        Memory memory(config);
        for (auto access : trace("belady", 12, 0)) { CHECK(memory.read(access.address) == 0); memory.validate(); }
        faults.push_back(memory.statistics().page_faults);
        CHECK(memory.statistics().first_touches == 5 && memory.statistics().swap_writes == 0);
    }
    CHECK(faults == (std::vector<std::uint64_t>{9,10}));
}
void eviction_and_dirty_data() {
    for (auto policy : {Replacement::fifo, Replacement::clock, Replacement::aging}) {
        Config config; config.frames = 1; config.tlb_entries = 16; config.policy = policy; config.aging_period = 2;
        Memory memory(config);
        memory.write(0, 37); memory.write(4095, 254);
        memory.write(4096, 68); // Eviction must invalidate the still-cached first page.
        CHECK(memory.read(0) == 37 && memory.read(4095) == 254 && memory.read(1) == 0);
        auto writes = memory.statistics().swap_writes;
        CHECK(memory.read(4096) == 68);
        CHECK(memory.statistics().swap_writes == writes); // Reloaded clean pages need no second write.
        CHECK(memory.read(0) == 37);
        memory.write(0, 13); CHECK(memory.read(4096) == 68); CHECK(memory.read(0) == 13);
        CHECK(memory.statistics().swap_bytes_written == memory.statistics().swap_writes * page_size);
        memory.validate();
    }
}
void clock_second_chance() {
    Config config; config.frames = 3; config.tlb_entries = 3; config.policy = Replacement::clock;
    Memory memory(config);
    for (std::uint32_t page : {0U,1U,2U,3U}) memory.read(page * page_size);
    CHECK(!memory.resident(0) && memory.resident(1));
    memory.read(page_size); // TLB hit must also set the reference bit.
    memory.read(4 * page_size);
    CHECK(memory.resident(1) && !memory.resident(2)); memory.validate();
}
void randomized_oracle() {
    for (auto policy : {Replacement::fifo, Replacement::clock, Replacement::aging}) {
        Config config; config.frames = 3; config.tlb_entries = 8; config.policy = policy; config.aging_period = 3;
        Memory memory(config); std::mt19937 random(192);
        std::unordered_map<std::uint32_t, std::uint8_t> oracle;
        for (int i = 0; i < 5000; ++i) {
            std::uint32_t page = random() % 16;
            if (i % 23 == 0) page += 1024;
            auto address = static_cast<std::uint32_t>(page * page_size + random() % 32);
            if (random() % 3 == 0) {
                auto value = static_cast<std::uint8_t>(random()); memory.write(address, value); oracle[address] = value;
            } else CHECK(memory.read(address) == oracle[address]);
            if (i % 17 == 0) memory.validate();
        }
        for (const auto& [address, value] : oracle) CHECK(memory.read(address) == value);
        memory.validate();
        CHECK(memory.statistics().page_faults > memory.statistics().first_touches);
        CHECK(memory.statistics().swap_reads > 0 && memory.statistics().swap_writes > 0);
    }
}
void invalid_configuration() {
    Config config; config.frames = 0;
    check_throws<std::invalid_argument>([&] { Memory memory(config); });
    config.frames = 1; config.aging_period = 0;
    check_throws<std::invalid_argument>([&] { Memory memory(config); });
    check_throws<std::invalid_argument>([] { replacement("lru"); });
    std::istringstream input("address,operation,value\n0,write,7\n0,read,\n4294967295,write,255\n");
    auto parsed = read_trace(input); CHECK(parsed.size() == 3 && parsed[0].value == 7 && !parsed[1].value);
    CHECK(parsed[2].address == 0xffffffff && parsed[2].value == 255);
    for (auto text : {"bad\n", "address,operation,value\n-1,read,\n", "address,operation,value\n0,write,256\n"}) {
        std::istringstream bad(text); check_throws<std::invalid_argument>([&] { read_trace(bad); });
    }
}
}
int main() {
    for (auto [name, test] : std::vector<std::pair<const char*, void(*)()>>{
        {"demand paging and TLB", demand_and_tlb}, {"FIFO Belady anomaly", belady_anomaly},
        {"dirty swap and stale TLB", eviction_and_dirty_data}, {"Clock second chance", clock_second_chance},
        {"randomized byte oracle", randomized_oracle}, {"invalid configuration", invalid_configuration}}) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; return 1; }
    }
}

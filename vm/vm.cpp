#include "os/vm.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <random>
#include <set>
#include <span>
#include <unistd.h>

namespace os::vm {
namespace {
constexpr std::size_t table_width = 1024;
constexpr std::uint32_t virtual_pages = 1024 * 1024;
Fd swap_file() {
    std::string name = "/tmp/os-subsystems-swap-XXXXXX";
    int fd = mkostemp(name.data(), O_CLOEXEC);
    if (fd < 0) os::system_error("mkstemp swap");
    Fd result(fd);
    if (unlink(name.c_str()) < 0) os::system_error("unlink temporary swap");
    return result; // Real disk-backed file, removed automatically when closed.
}
void disk_io(int fd, std::span<std::uint8_t> bytes, std::uint32_t page, bool write) {
    off_t offset = static_cast<off_t>(page) * page_size;
    while (!bytes.empty()) {
        auto count = write ? pwrite(fd, bytes.data(), bytes.size(), offset) : pread(fd, bytes.data(), bytes.size(), offset);
        if (count < 0) { if (errno == EINTR) continue; os::system_error(write ? "pwrite swap" : "pread swap"); }
        if (count == 0) throw std::runtime_error("short swap I/O");
        bytes = bytes.subspan(static_cast<std::size_t>(count)); offset += count;
    }
}
}
struct Memory::Impl {
    struct Entry { std::size_t frame = 0; bool touched = false, present = false, swapped = false, dirty = false; };
    using Table = std::array<Entry, table_width>;
    struct Frame {
        std::uint32_t page = 0;
        std::uint8_t age = 0;
        bool used = false, referenced = false;
    };
    struct TlbEntry { std::uint32_t page = 0; std::size_t frame = 0; std::uint64_t last = 0; bool valid = false; };
    Config config;
    Statistics stats;
    std::array<std::unique_ptr<Table>, table_width> directory;
    std::vector<Frame> frames;
    // Replacement scans touch compact metadata, not one cache line per 4 KiB page.
    std::vector<std::array<std::uint8_t, page_size>> frame_bytes;
    std::vector<TlbEntry> tlb;
    std::deque<std::size_t> fifo;
    std::size_t next_free = 0, hand = 0;
    Fd swap;
    explicit Impl(Config options) : config(options), swap(swap_file()) {
        if (!config.frames || config.frames > 65536 || config.tlb_entries > 4096 || !config.aging_period)
            throw std::invalid_argument("invalid VM configuration");
        if (config.policy != Replacement::fifo && config.policy != Replacement::clock && config.policy != Replacement::aging)
            throw std::invalid_argument("invalid replacement policy");
        frames.resize(config.frames); frame_bytes.resize(config.frames); tlb.resize(config.tlb_entries);
    }
    Entry& entry(std::uint32_t page) {
        auto& table = directory.at(page >> 10);
        if (!table) { table = std::make_unique<Table>(); ++stats.page_tables; }
        return table->at(page & 1023);
    }
    const Entry* lookup(std::uint32_t page) const {
        if (page >= virtual_pages || !directory[page >> 10]) return nullptr;
        return &(*directory[page >> 10])[page & 1023];
    }
    void age() {
        if (config.policy != Replacement::aging || stats.accesses % config.aging_period) return;
        for (auto& frame : frames) if (frame.used) {
            frame.age = static_cast<std::uint8_t>((frame.age >> 1) | (frame.referenced ? 128 : 0));
            frame.referenced = false;
        }
    }
    std::size_t victim() {
        if (config.policy == Replacement::fifo) { auto frame = fifo.front(); fifo.pop_front(); return frame; }
        if (config.policy == Replacement::clock) {
            for (;;) {
                auto selected = hand; hand = (hand + 1) % frames.size();
                if (!frames[selected].referenced) return selected;
                frames[selected].referenced = false;
            }
        }
        std::size_t selected = 0;
        for (std::size_t i = 1; i < frames.size(); ++i)
            if (frames[i].age < frames[selected].age) selected = i;
        return selected;
    }
    std::size_t fault(std::uint32_t page, Entry& pte) {
        ++stats.page_faults;
        if (!pte.touched) { pte.touched = true; ++stats.first_touches; }
        std::size_t selected;
        if (next_free < frames.size()) { selected = next_free++; ++stats.resident_pages; }
        else {
            selected = victim(); auto& old = frames[selected]; auto& old_pte = entry(old.page);
            if (old_pte.dirty) {
                disk_io(swap.get(), frame_bytes[selected], old.page, true);
                old_pte.swapped = true; old_pte.dirty = false;
                ++stats.swap_writes; stats.swap_bytes_written += page_size;
            }
            old_pte.present = false;
            for (auto& cached : tlb) if (cached.valid && cached.page == old.page) cached.valid = false;
            ++stats.evictions;
        }
        auto& frame = frames[selected];
        if (pte.swapped) { disk_io(swap.get(), frame_bytes[selected], page, false); ++stats.swap_reads; }
        else frame_bytes[selected].fill(0);
        frame.page = page; frame.used = frame.referenced = true; frame.age = 128;
        pte.frame = selected; pte.present = true; pte.dirty = false;
        if (config.policy == Replacement::fifo) fifo.push_back(selected);
        return selected;
    }
    std::uint8_t access(std::uint32_t address, std::optional<std::uint8_t> value) {
        ++stats.accesses; age();
        auto page = address >> 12; auto offset = address & 4095;
        std::optional<std::size_t> selected;
        for (auto& cached : tlb) if (cached.valid && cached.page == page) {
            selected = cached.frame; cached.last = stats.accesses; ++stats.tlb_hits; break;
        }
        if (!selected) {
            ++stats.tlb_misses;
            auto& pte = entry(page);
            selected = pte.present ? pte.frame : fault(page, pte);
            if (!tlb.empty()) {
                auto target = std::find_if(tlb.begin(), tlb.end(), [](const auto& e) { return !e.valid; });
                if (target == tlb.end()) target = std::min_element(tlb.begin(), tlb.end(),
                    [](const auto& a, const auto& b) { return a.last < b.last; });
                *target = TlbEntry{page, *selected, stats.accesses, true};
            }
        }
        auto& frame = frames[*selected]; frame.referenced = true;
        if (value) { frame_bytes[*selected][offset] = *value; entry(page).dirty = true; }
        return frame_bytes[*selected][offset];
    }
    void validate() const {
        std::size_t count = 0;
        std::set<std::uint32_t> owners;
        for (std::size_t i = 0; i < frames.size(); ++i) if (frames[i].used) {
            ++count; const auto* pte = lookup(frames[i].page);
            if (!owners.insert(frames[i].page).second || !pte || !pte->present || pte->frame != i)
                throw std::logic_error("frame ownership mismatch");
        }
        std::size_t present = 0;
        for (std::size_t first = 0; first < directory.size(); ++first) if (directory[first]) {
            for (std::size_t second = 0; second < table_width; ++second) {
                const auto& pte = (*directory[first])[second];
                if (!pte.present) continue;
                ++present;
                if (!pte.touched || pte.frame >= frames.size() || !frames[pte.frame].used ||
                    frames[pte.frame].page != first * table_width + second)
                    throw std::logic_error("page-table ownership mismatch");
            }
        }
        std::set<std::uint32_t> cached_pages;
        for (const auto& cached : tlb) if (cached.valid) {
            const auto* pte = lookup(cached.page);
            if (!cached_pages.insert(cached.page).second || !pte || !pte->present || pte->frame != cached.frame)
                throw std::logic_error("stale or duplicate TLB entry");
        }
        if (count != stats.resident_pages || present != count || count > config.frames ||
            stats.tlb_hits + stats.tlb_misses != stats.accesses)
            throw std::logic_error("VM accounting mismatch");
        if (config.policy == Replacement::fifo) {
            std::set<std::size_t> entries(fifo.begin(), fifo.end());
            if (entries.size() != count || fifo.size() != count) throw std::logic_error("FIFO frame mismatch");
        }
    }
};
Memory::Memory(Config config) : impl_(std::make_unique<Impl>(config)) {}
Memory::~Memory() = default;
std::uint8_t Memory::read(std::uint32_t address) { return impl_->access(address, std::nullopt); }
void Memory::write(std::uint32_t address, std::uint8_t value) { impl_->access(address, value); }
const Statistics& Memory::statistics() const { return impl_->stats; }
bool Memory::resident(std::uint32_t page) const { auto pte = impl_->lookup(page); return pte && pte->present; }
void Memory::validate() const { impl_->validate(); }
Replacement replacement(const std::string& name) {
    if (name == "fifo") return Replacement::fifo;
    if (name == "clock") return Replacement::clock;
    if (name == "aging") return Replacement::aging;
    throw std::invalid_argument("unknown replacement policy");
}
std::vector<Access> trace(const std::string& name, std::size_t count, std::uint32_t seed) {
    std::vector<Access> accesses;
    if (name == "belady") {
        for (std::uint32_t page : {1U,2U,3U,4U,1U,2U,5U,1U,2U,3U,4U,5U})
            accesses.push_back({static_cast<std::uint32_t>(page * page_size), std::nullopt});
        return accesses;
    }
    if (name != "sequential" && name != "random" && name != "looping") throw std::invalid_argument("unknown trace");
    if (!count || count > 10000000) throw std::invalid_argument("invalid trace length");
    std::mt19937 random(seed); accesses.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t page = name == "random" ? random() % 256 :
            name == "sequential" ? (i / 16) % 256 : (i / 4) % 64;
        std::uint32_t offset = static_cast<std::uint32_t>(i % 16);
        std::optional<std::uint8_t> value;
        if (i % 7 == 0) value = static_cast<std::uint8_t>(i * 17 + seed);
        accesses.push_back({static_cast<std::uint32_t>(page * page_size + offset), value});
    }
    return accesses;
}
std::vector<Access> read_trace(std::istream& stream) {
    std::string line;
    if (!std::getline(stream, line) || line != "address,operation,value") throw std::invalid_argument("invalid trace header");
    auto number = [](const std::string& text, std::uint64_t limit) {
        std::uint64_t value = 0;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value > limit)
            throw std::invalid_argument("invalid trace number");
        return value;
    };
    std::vector<Access> result;
    while (std::getline(stream, line)) {
        auto first = line.find(','), second = first == std::string::npos ? first : line.find(',', first + 1);
        if (first == std::string::npos || second == std::string::npos || line.find(',', second + 1) != std::string::npos)
            throw std::invalid_argument("invalid trace row");
        auto address = static_cast<std::uint32_t>(number(line.substr(0, first), 0xffffffff));
        auto operation = line.substr(first + 1, second - first - 1), value = line.substr(second + 1);
        if (operation == "read" && value.empty()) result.push_back({address, std::nullopt});
        else if (operation == "write") result.push_back({address, static_cast<std::uint8_t>(number(value, 255))});
        else throw std::invalid_argument("invalid trace operation");
        if (result.size() > 10000000) throw std::invalid_argument("trace is too large");
    }
    if (result.empty() || stream.bad()) throw std::invalid_argument("empty or unreadable trace");
    return result;
}
}

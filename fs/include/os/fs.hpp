#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace os::fs {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t block_size = 4096;
constexpr std::uint64_t max_file_size = (8ULL + 1024 + 1024 * 1024) * block_size;
using Handle = std::uint64_t;
struct Stat {
    std::uint32_t inode = 0, links = 0, uid = 0, gid = 0;
    std::uint16_t mode = 0;
    bool directory = false;
    std::uint64_t size = 0, generation = 0, mtime_ns = 0, ctime_ns = 0;
};
struct DirectoryEntry { std::uint32_t inode; std::string name; };
struct Space { std::uint64_t blocks, free_blocks, inodes, free_inodes; };
struct IoStatistics { std::uint64_t block_reads = 0, block_writes = 0, syncs = 0; };
struct CheckReport {
    std::vector<std::string> issues;
    std::uint64_t allocated_inodes = 0, referenced_blocks = 0, reachable_inodes = 0;
    bool clean() const { return issues.empty(); }
};
void format(const std::string& path, std::uint32_t blocks = 8192, std::uint32_t inodes = 256);
CheckReport check(const std::string& path);
class Image {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Image(const std::string& path);
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Stat stat(const std::string& path);
    Stat fstat(Handle handle);
    void create(const std::string& path, std::uint16_t mode = 0644);
    Handle create_open(const std::string& path, std::uint16_t mode, int flags);
    Handle open(const std::string& path, int flags);
    void release(Handle handle);
    Bytes read(Handle handle, std::uint64_t offset, std::size_t count);
    std::size_t write(Handle handle, std::uint64_t offset, std::span<const std::uint8_t> bytes);
    void truncate(const std::string& path, std::uint64_t size);
    void ftruncate(Handle handle, std::uint64_t size);
    void unlink(const std::string& path);
    void mkdir(const std::string& path, std::uint16_t mode = 0755);
    void rmdir(const std::string& path);
    void link(const std::string& source, const std::string& destination);
    void rename(const std::string& source, const std::string& destination);
    std::vector<DirectoryEntry> list(const std::string& path);
    void chmod(const std::string& path, std::uint16_t mode);
    void set_mtime(const std::string& path, std::uint64_t nanoseconds);
    Space space();
    void sync();
    IoStatistics io_statistics();
};
}

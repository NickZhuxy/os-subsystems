#pragma once
#include "os/fs.hpp"
#include "os/fd.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits>
#include <system_error>

namespace os::fs::disk {
using Block = std::array<std::uint8_t, block_size>;
constexpr std::size_t inode_size = 128, directory_record_size = 64, max_name = 56;
constexpr std::size_t max_directory_size = 65536 * directory_record_size;
[[noreturn]] inline void fail(int error, const char* message) { throw std::system_error(error, std::generic_category(), message); }
inline std::uint64_t get(std::span<const std::uint8_t> bytes, std::size_t offset, unsigned count) {
    if (offset + count > bytes.size() || count > 8) fail(EIO, "invalid disk field");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    return value;
}
inline void put(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value, unsigned count) {
    if (offset + count > bytes.size() || count > 8) fail(EIO, "invalid disk field");
    for (unsigned i = 0; i < count; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
inline std::uint32_t hash(std::span<const std::uint8_t> bytes) {
    std::uint32_t value = 2166136261U;
    for (auto byte : bytes) value = (value ^ byte) * 16777619U;
    return value;
}
inline void transfer(int fd, std::span<std::uint8_t> bytes, off_t offset, bool write) {
    while (!bytes.empty()) {
        auto count = write ? pwrite(fd, bytes.data(), bytes.size(), offset) : pread(fd, bytes.data(), bytes.size(), offset);
        if (count < 0) { if (errno == EINTR) continue; os::system_error(write ? "image pwrite" : "image pread"); }
        if (count == 0) fail(EIO, "truncated image I/O");
        bytes = bytes.subspan(static_cast<std::size_t>(count)); offset += count;
    }
}
inline std::uint64_t timestamp() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
struct Super {
    std::uint32_t blocks, inodes, inode_bitmap, inode_bitmap_blocks, block_bitmap, block_bitmap_blocks;
    std::uint32_t inode_table, inode_table_blocks, data_start;
};
inline Super geometry(std::uint32_t blocks, std::uint32_t inodes) {
    if (blocks < 32 || blocks > 1048576 || inodes < 8 || inodes > 65536) fail(EINVAL, "invalid filesystem geometry");
    Super s{}; s.blocks = blocks; s.inodes = inodes;
    s.inode_bitmap = 1; s.inode_bitmap_blocks = (inodes + 32767) / 32768;
    s.block_bitmap = s.inode_bitmap + s.inode_bitmap_blocks;
    s.block_bitmap_blocks = (blocks + 32767) / 32768;
    s.inode_table = s.block_bitmap + s.block_bitmap_blocks;
    s.inode_table_blocks = (inodes * inode_size + block_size - 1) / block_size;
    s.data_start = s.inode_table + s.inode_table_blocks;
    if (s.data_start >= blocks) fail(EINVAL, "metadata consumes image");
    return s;
}
inline Block encode(const Super& s) {
    Block bytes{}; const std::array<std::uint8_t, 8> magic{'O','S','F','S','2','6',0,1};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    std::array<std::uint32_t, 12> fields{1,block_size,s.blocks,s.inodes,s.inode_bitmap,s.inode_bitmap_blocks,
        s.block_bitmap,s.block_bitmap_blocks,s.inode_table,s.inode_table_blocks,s.data_start,1};
    for (std::size_t i = 0; i < fields.size(); ++i) put(bytes, 8 + i * 4, fields[i], 4);
    put(bytes, 56, hash(std::span(bytes).first(56)), 4); return bytes;
}
inline Super decode_super(const Block& bytes) {
    const std::array<std::uint8_t, 8> magic{'O','S','F','S','2','6',0,1};
    if (!std::equal(magic.begin(), magic.end(), bytes.begin()) || get(bytes,8,4) != 1 || get(bytes,12,4) != block_size ||
        get(bytes,56,4) != hash(std::span(bytes).first(56))) fail(EIO, "invalid superblock or checksum");
    auto expected = geometry(static_cast<std::uint32_t>(get(bytes,16,4)), static_cast<std::uint32_t>(get(bytes,20,4)));
    auto encoded = encode(expected);
    if (!std::equal(encoded.begin(), encoded.begin() + 60, bytes.begin())) fail(EIO, "invalid metadata layout");
    return expected;
}
struct Inode {
    std::uint16_t type = 0, mode = 0;
    std::uint32_t links = 0, uid = 0, gid = 0;
    std::uint64_t size = 0, generation = 0, mtime = 0, ctime = 0;
    std::array<std::uint32_t, 8> direct{};
    std::uint32_t single = 0, indirect = 0;
};
inline Inode decode_inode(std::span<const std::uint8_t> bytes) {
    Inode node;
    node.type = get(bytes,0,2); node.mode = get(bytes,2,2); node.links = get(bytes,4,4);
    node.size = get(bytes,8,8); node.generation = get(bytes,16,8); node.uid = get(bytes,24,4); node.gid = get(bytes,28,4);
    node.mtime = get(bytes,32,8); node.ctime = get(bytes,40,8);
    for (std::size_t i = 0; i < node.direct.size(); ++i) node.direct[i] = get(bytes,48 + i * 4,4);
    node.single = get(bytes,80,4); node.indirect = get(bytes,84,4); return node;
}
inline void encode_inode(std::span<std::uint8_t> bytes, const Inode& node) {
    std::fill(bytes.begin(), bytes.end(), 0);
    put(bytes,0,node.type,2); put(bytes,2,node.mode,2); put(bytes,4,node.links,4);
    put(bytes,8,node.size,8); put(bytes,16,node.generation,8); put(bytes,24,node.uid,4); put(bytes,28,node.gid,4);
    put(bytes,32,node.mtime,8); put(bytes,40,node.ctime,8);
    for (std::size_t i = 0; i < node.direct.size(); ++i) put(bytes,48 + i * 4,node.direct[i],4);
    put(bytes,80,node.single,4); put(bytes,84,node.indirect,4);
}
inline Stat stat(std::uint32_t id, const Inode& node) {
    return {id,node.links,node.uid,node.gid,node.mode,node.type == 2,node.size,node.generation,node.mtime,node.ctime};
}
inline void encode_record(std::span<std::uint8_t> bytes, std::uint32_t inode, const std::string& name) {
    if (bytes.size() != directory_record_size || name.size() > max_name) fail(EINVAL, "invalid directory record");
    std::fill(bytes.begin(), bytes.end(), 0); put(bytes,0,inode,4); put(bytes,4,name.size(),2);
    std::copy(name.begin(), name.end(), bytes.begin() + 8);
}
inline DirectoryEntry decode_record(std::span<const std::uint8_t> bytes) {
    auto id = static_cast<std::uint32_t>(get(bytes,0,4));
    auto length = static_cast<std::size_t>(get(bytes,4,2));
    if (!id) {
        if (std::any_of(bytes.begin(), bytes.end(), [](auto value) { return value != 0; })) fail(EIO, "dirty unused directory record");
        return {0,""};
    }
    if (!length || length > max_name) fail(EIO, "invalid directory name length");
    std::string name(bytes.begin() + 8, bytes.begin() + 8 + length);
    if (name.find('/') != std::string::npos || name.find('\0') != std::string::npos) fail(EIO, "invalid directory name");
    return {id,std::move(name)};
}
class Reader {
public:
    Fd fd;
    Super super{};
    explicit Reader(const std::string& path, bool writable) : fd(::open(path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC)) {
        if (fd.get() < 0) os::system_error("open image");
        if (flock(fd.get(), (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) < 0) os::system_error("lock image");
        Block bytes{}; transfer(fd.get(), bytes, 0, false); super = decode_super(bytes);
        struct stat metadata{};
        if (::fstat(fd.get(), &metadata) < 0) os::system_error("stat image");
        if (metadata.st_size != static_cast<off_t>(super.blocks) * block_size) fail(EIO, "image length mismatch");
    }
    Block raw_block(std::uint32_t id) const {
        if (id >= super.blocks) fail(EIO, "block outside image");
        Block block{}; transfer(fd.get(), block, static_cast<off_t>(id) * block_size, false); return block;
    }
};
}

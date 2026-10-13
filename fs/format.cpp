#include "format.hpp"
namespace os::fs {
void format(const std::string& path, std::uint32_t blocks, std::uint32_t inodes) {
    auto super = disk::geometry(blocks, inodes);
    Fd fd(::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600));
    if (fd.get() < 0) os::system_error("create image (will not overwrite)");
    if (flock(fd.get(), LOCK_EX | LOCK_NB) < 0) os::system_error("lock new image");
    if (ftruncate(fd.get(), static_cast<off_t>(blocks) * block_size) < 0) os::system_error("size image");
    auto write_block = [&](std::uint32_t id, disk::Block block) {
        disk::transfer(fd.get(), block, static_cast<off_t>(id) * block_size, true);
    };
    write_block(0, disk::encode(super));
    std::vector<disk::Block> inode_map(super.inode_bitmap_blocks), block_map(super.block_bitmap_blocks);
    inode_map[0][0] = 3; // Reserved inode zero and root inode one.
    for (std::uint32_t id = 0; id <= super.data_start; ++id)
        block_map[id / 32768][(id % 32768) / 8] |= static_cast<std::uint8_t>(1U << (id % 8));
    for (std::size_t i = 0; i < inode_map.size(); ++i) write_block(super.inode_bitmap + i, inode_map[i]);
    for (std::size_t i = 0; i < block_map.size(); ++i) write_block(super.block_bitmap + i, block_map[i]);
    disk::Inode root; root.type = 2; root.mode = 0755; root.links = 2;
    root.uid = getuid(); root.gid = getgid(); root.size = 2 * disk::directory_record_size;
    root.generation = 1; root.mtime = root.ctime = disk::timestamp(); root.direct[0] = super.data_start;
    disk::Block table{};
    disk::encode_inode(std::span(table).subspan(disk::inode_size, disk::inode_size), root);
    write_block(super.inode_table, table);
    disk::Block directory{};
    disk::encode_record(std::span(directory).first(disk::directory_record_size), 1, ".");
    disk::encode_record(std::span(directory).subspan(disk::directory_record_size, disk::directory_record_size), 1, "..");
    write_block(super.data_start, directory);
    if (fsync(fd.get()) < 0) os::system_error("sync new image");
}
}

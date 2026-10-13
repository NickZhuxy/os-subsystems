#include "format.hpp"
#include <csignal>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace os::fs {
using disk::Block;
using disk::Inode;
using disk::fail;
struct Image::Impl : disk::Reader {
    std::mutex mutex;
    std::unordered_map<std::uint32_t, Block> pending;
    std::vector<std::uint32_t> write_order;
    struct OpenFile { std::uint32_t inode; int flags; };
    std::unordered_map<Handle, OpenFile> handles;
    std::vector<std::size_t> open_counts;
    std::uint32_t allocation_hint;
    Handle next_handle = 1;
    IoStatistics io;
    explicit Impl(const std::string& path) : Reader(path, true), open_counts(super.inodes), allocation_hint(super.data_start) {
        if (inode(1).type != 2) fail(EIO, "root is not a directory");
    }
    Block read_block(std::uint32_t id) {
        auto found = pending.find(id);
        if (found != pending.end()) return found->second;
        ++io.block_reads; return raw_block(id);
    }
    void stage(std::uint32_t id, const Block& block) {
        if (id >= super.blocks) fail(EIO, "staged block outside image");
        if (!pending.contains(id)) write_order.push_back(id);
        pending[id] = block;
    }
    void synchronize() { if (::fsync(fd.get()) < 0) os::system_error("sync image"); ++io.syncs; }
    void crash_point(const char* name) {
        const char* requested = std::getenv("OS_FS_CRASH_POINT");
        if (!requested || std::string(requested) != name) return;
        synchronize();
        std::string marker = std::string("CRASH_POINT ") + name + "\n";
        (void)::write(STDERR_FILENO, marker.data(), marker.size());
        raise(SIGSTOP); // The external crash test kills this real writer here.
    }
    void transaction(const std::function<void()>& operation) {
        if (!pending.empty()) throw std::logic_error("nested filesystem transaction");
        auto old_hint = allocation_hint;
        try {
            operation();
            for (auto id : write_order) {
                auto& block = pending.at(id);
                disk::transfer(fd.get(), block, static_cast<off_t>(id) * block_size, true); ++io.block_writes;
                if (id >= super.block_bitmap && id < super.block_bitmap + super.block_bitmap_blocks) crash_point("bitmap-written");
                if (id >= super.data_start) crash_point("data-written");
            }
            if (!write_order.empty()) synchronize();
            pending.clear(); write_order.clear();
        } catch (...) {
            pending.clear(); write_order.clear(); allocation_hint = old_hint; throw;
        }
    }
    bool bit(std::uint32_t start, std::uint32_t index) {
        auto block = read_block(start + index / 32768);
        return block[(index % 32768) / 8] & (1U << (index % 8));
    }
    void set_bit(std::uint32_t start, std::uint32_t index, bool value) {
        auto id = start + index / 32768; auto block = read_block(id);
        auto& byte = block[(index % 32768) / 8]; auto mask = static_cast<std::uint8_t>(1U << (index % 8));
        if (value) byte |= mask; else byte &= static_cast<std::uint8_t>(~mask);
        stage(id, block);
    }
    void data_id(std::uint32_t id) const {
        if (id < super.data_start || id >= super.blocks) fail(EIO, "invalid data block pointer");
    }
    std::uint32_t allocate_block() {
        auto available = super.blocks - super.data_start;
        for (std::uint32_t attempt = 0; attempt < available; ++attempt) {
            auto id = allocation_hint++;
            if (allocation_hint == super.blocks) allocation_hint = super.data_start;
            if (!bit(super.block_bitmap, id)) {
                set_bit(super.block_bitmap, id, true); stage(id, Block{}); return id;
            }
        }
        fail(ENOSPC, "no free data blocks");
    }
    void free_block(std::uint32_t id) {
        data_id(id);
        if (!bit(super.block_bitmap, id)) fail(EIO, "double free block");
        set_bit(super.block_bitmap, id, false); allocation_hint = std::min(allocation_hint, id);
    }
    Inode raw_inode(std::uint32_t id) {
        if (id >= super.inodes) fail(EIO, "inode outside image");
        auto offset = static_cast<std::uint64_t>(id) * disk::inode_size;
        auto block = read_block(super.inode_table + offset / block_size);
        return disk::decode_inode(std::span(block).subspan(offset % block_size, disk::inode_size));
    }
    Inode inode(std::uint32_t id) {
        if (!id || id >= super.inodes || !bit(super.inode_bitmap, id)) fail(ENOENT, "inode does not exist");
        auto node = raw_inode(id);
        if ((node.type != 1 && node.type != 2) || node.size > max_file_size) fail(EIO, "invalid inode");
        return node;
    }
    void save_inode(std::uint32_t id, const Inode& node) {
        auto offset = static_cast<std::uint64_t>(id) * disk::inode_size;
        auto block_id = super.inode_table + offset / block_size; auto block = read_block(block_id);
        disk::encode_inode(std::span(block).subspan(offset % block_size, disk::inode_size), node); stage(block_id, block);
    }
    std::uint32_t allocate_inode(std::uint16_t type, std::uint16_t mode) {
        for (std::uint32_t id = 2; id < super.inodes; ++id) if (!bit(super.inode_bitmap, id)) {
            auto previous = raw_inode(id);
            if (previous.generation == std::numeric_limits<std::uint64_t>::max()) continue;
            Inode node; node.type = type; node.mode = mode & 0777; node.links = type == 2 ? 2 : 1;
            node.generation = previous.generation + 1; node.uid = getuid(); node.gid = getgid();
            node.mtime = node.ctime = disk::timestamp();
            set_bit(super.inode_bitmap, id, true); save_inode(id, node); return id;
        }
        fail(ENOSPC, "no free inodes");
    }
    std::uint32_t pointer(Inode& node, std::uint64_t logical, bool create) {
        if (logical >= max_file_size / block_size) fail(EFBIG, "file block limit");
        if (logical < 8) {
            auto& id = node.direct[logical]; if (!id && create) id = allocate_block();
            if (id) data_id(id); return id;
        }
        logical -= 8;
        auto in_table = [&](std::uint32_t table_id, std::size_t index) {
            data_id(table_id); auto table = read_block(table_id);
            auto id = static_cast<std::uint32_t>(disk::get(table, index * 4, 4));
            if (!id && create) { id = allocate_block(); disk::put(table,index * 4,id,4); stage(table_id,table); }
            if (id) data_id(id); return id;
        };
        if (logical < 1024) {
            if (!node.single && create) node.single = allocate_block();
            return node.single ? in_table(node.single, logical) : 0;
        }
        logical -= 1024;
        if (!node.indirect && create) node.indirect = allocate_block();
        if (!node.indirect) return 0;
        auto table = in_table(node.indirect, logical / 1024);
        return table ? in_table(table, logical % 1024) : 0;
    }
    Bytes read_data(Inode node, std::uint64_t offset, std::size_t count) {
        if (count > 64 * 1024 * 1024) fail(EINVAL, "read exceeds per-call limit");
        if (offset >= node.size) return {};
        count = static_cast<std::size_t>(std::min<std::uint64_t>(count, node.size - offset)); Bytes output(count,0);
        std::size_t done = 0;
        while (done < count) {
            auto position = offset + done; auto within = position % block_size;
            auto chunk = std::min<std::size_t>(count - done, block_size - within);
            auto id = pointer(node, position / block_size, false);
            if (id) { auto block = read_block(id); std::copy_n(block.begin() + within, chunk, output.begin() + done); }
            done += chunk;
        }
        return output;
    }
    void write_data(Inode& node, std::uint64_t offset, std::span<const std::uint8_t> input) {
        if (input.size() > 1024 * 1024) fail(EINVAL, "write exceeds per-call limit");
        if (offset > max_file_size || input.size() > max_file_size - offset) fail(EFBIG, "file size limit");
        if (input.empty()) return;
        std::size_t done = 0;
        while (done < input.size()) {
            auto position = offset + done; auto within = position % block_size;
            auto chunk = std::min<std::size_t>(input.size() - done, block_size - within);
            auto id = pointer(node, position / block_size, true); auto block = read_block(id);
            std::copy_n(input.begin() + done, chunk, block.begin() + within); stage(id,block); done += chunk;
        }
        node.size = std::max(node.size, offset + input.size()); node.mtime = node.ctime = disk::timestamp();
    }
    void resize(Inode& node, std::uint64_t size) {
        if (size > max_file_size) fail(EFBIG, "file size limit");
        if (size < node.size) {
            auto keep = (size + block_size - 1) / block_size;
            for (std::size_t i = 0; i < node.direct.size(); ++i) if (i >= keep && node.direct[i]) {
                free_block(node.direct[i]); node.direct[i] = 0;
            }
            auto prune = [&](std::uint32_t& id, std::uint64_t first) {
                if (!id) return;
                data_id(id); auto table = read_block(id); bool any = false, changed = false;
                for (std::size_t i = 0; i < 1024; ++i) {
                    auto value = static_cast<std::uint32_t>(disk::get(table,i * 4,4));
                    if (value && first + i >= keep) {
                        free_block(value); disk::put(table,i * 4,0,4); value = 0; changed = true;
                    }
                    any = any || value != 0;
                }
                if (!any) { free_block(id); id = 0; }
                else if (changed) stage(id,table);
            };
            prune(node.single,8);
            if (node.indirect) {
                data_id(node.indirect); auto root = read_block(node.indirect); bool any = false, changed = false;
                for (std::size_t i = 0; i < 1024; ++i) {
                    auto child = static_cast<std::uint32_t>(disk::get(root,i * 4,4)), before = child;
                    prune(child,8 + 1024 + i * 1024);
                    if (child != before) { disk::put(root,i * 4,child,4); changed = true; }
                    any = any || child != 0;
                }
                if (!any) { free_block(node.indirect); node.indirect = 0; }
                else if (changed) stage(node.indirect,root);
            }
            if (size % block_size) {
                auto id = pointer(node,size / block_size,false);
                if (id) { auto block = read_block(id); std::fill(block.begin() + size % block_size,block.end(),0); stage(id,block); }
            }
        }
        node.size = size; node.mtime = node.ctime = disk::timestamp();
    }
    void free_inode(std::uint32_t id, Inode node) {
        if (id < 2 || open_counts.at(id)) fail(EBUSY, "inode is in use");
        resize(node,0); auto generation = node.generation; node = Inode{}; node.generation = generation;
        save_inode(id,node); set_bit(super.inode_bitmap,id,false);
    }
    struct Slot { DirectoryEntry entry; std::uint64_t offset; };
    std::vector<Slot> records(const Inode& node) {
        if (node.type != 2) fail(ENOTDIR, "not a directory");
        if (node.size % disk::directory_record_size || node.size > disk::max_directory_size) fail(EIO, "invalid directory size");
        auto data = read_data(node,0,node.size); std::vector<Slot> result;
        for (std::size_t offset = 0; offset < data.size(); offset += disk::directory_record_size) {
            auto entry = disk::decode_record(std::span(data).subspan(offset,disk::directory_record_size));
            if (entry.inode >= super.inodes) fail(EIO, "directory target outside image");
            result.push_back({std::move(entry),offset});
        }
        return result;
    }
    std::optional<Slot> find(const Inode& directory, const std::string& name) {
        for (auto& slot : records(directory)) if (slot.entry.inode && slot.entry.name == name) return slot;
        return std::nullopt;
    }
    static std::vector<std::string> components(const std::string& path) {
        if (path.empty() || path.front() != '/' || path.size() > 4096 || path.find('\0') != std::string::npos)
            fail(EINVAL, "expected an absolute image path");
        std::vector<std::string> result; std::size_t begin = 1;
        while (begin < path.size()) {
            auto end = path.find('/',begin); if (end == std::string::npos) end = path.size();
            if (end > begin) {
                if (end - begin > disk::max_name) fail(ENAMETOOLONG, "directory name limit");
                result.push_back(path.substr(begin,end - begin));
            }
            begin = end + 1;
        }
        return result;
    }
    std::uint32_t resolve(const std::string& path) {
        std::uint32_t current = 1;
        for (const auto& name : components(path)) {
            auto slot = find(inode(current),name);
            if (!slot) fail(ENOENT, "path does not exist"); current = slot->entry.inode;
        }
        auto node = inode(current);
        if (path.back() == '/' && node.type != 2) fail(ENOTDIR, "trailing slash requires a directory");
        return current;
    }
    std::pair<std::uint32_t,std::string> parent(const std::string& path) {
        auto names = components(path);
        if (names.empty()) fail(EBUSY, "cannot change root");
        auto name = names.back(); names.pop_back();
        if (name == "." || name == "..") fail(EINVAL, "cannot change dot entries");
        std::string prefix = "/";
        for (const auto& part : names) prefix += part + '/';
        auto id = resolve(prefix);
        if (inode(id).type != 2) fail(ENOTDIR, "parent is not a directory");
        return {id,name};
    }
    void add_entry(std::uint32_t parent, const std::string& name, std::uint32_t child) {
        auto node = inode(parent); auto slots = records(node);
        for (const auto& slot : slots) if (slot.entry.inode && slot.entry.name == name) fail(EEXIST, "entry exists");
        auto empty = std::find_if(slots.begin(),slots.end(),[](const auto& slot) { return slot.entry.inode == 0; });
        auto offset = empty == slots.end() ? node.size : empty->offset;
        if (offset + disk::directory_record_size > disk::max_directory_size) fail(ENOSPC, "directory capacity limit");
        std::array<std::uint8_t,disk::directory_record_size> bytes{}; disk::encode_record(bytes,child,name);
        write_data(node,offset,bytes); save_inode(parent,node);
    }
    void remove_entry(std::uint32_t parent, const std::string& name) {
        auto node = inode(parent); auto slot = find(node,name);
        if (!slot) fail(ENOENT, "entry does not exist");
        std::array<std::uint8_t,disk::directory_record_size> bytes{};
        write_data(node,slot->offset,bytes); save_inode(parent,node);
    }
    std::uint32_t create_node(const std::string& path, std::uint16_t type, std::uint16_t mode) {
        auto [parent_id,name] = parent(path);
        if (type == 1 && path.back() == '/') fail(ENOTDIR, "file path has trailing slash");
        if (find(inode(parent_id),name)) fail(EEXIST, "entry exists");
        auto id = allocate_inode(type,mode);
        if (type == 2) {
            auto node = inode(id); std::array<std::uint8_t,2 * disk::directory_record_size> bytes{};
            disk::encode_record(std::span(bytes).first(disk::directory_record_size),id,".");
            disk::encode_record(std::span(bytes).subspan(disk::directory_record_size),parent_id,"..");
            write_data(node,0,bytes); save_inode(id,node);
            auto parent_node = inode(parent_id); ++parent_node.links; save_inode(parent_id,parent_node);
        }
        add_entry(parent_id,name,id); return id;
    }
    void drop_link(std::uint32_t id, bool directory, std::uint32_t parent_id) {
        auto node = inode(id);
        if (directory) {
            if (node.type != 2) fail(ENOTDIR, "target is not a directory");
            for (const auto& slot : records(node))
                if (slot.entry.inode && slot.entry.name != "." && slot.entry.name != "..") fail(ENOTEMPTY, "directory is not empty");
            node.links = 0;
            auto parent_node = inode(parent_id);
            if (parent_node.links < 3) fail(EIO, "invalid directory link count");
            --parent_node.links; save_inode(parent_id,parent_node);
        } else {
            if (node.type != 1) fail(EISDIR, "unlink requires a file");
            if (!node.links) fail(EIO, "invalid file link count"); --node.links;
        }
        node.ctime = disk::timestamp();
        if (!node.links && !open_counts[id]) free_inode(id,node); else save_inode(id,node);
    }
    Handle pin(std::uint32_t id, int flags) {
        if (handles.size() >= 65536 || next_handle >= (1ULL << 63)) fail(EMFILE, "open file limit");
        auto handle = next_handle++; handles.emplace(handle,OpenFile{id,flags}); ++open_counts[id]; return handle;
    }
    OpenFile file(Handle handle, bool writing = false) {
        auto found = handles.find(handle); if (found == handles.end()) fail(EBADF, "invalid file handle");
        if (writing && (found->second.flags & O_ACCMODE) == O_RDONLY) fail(EBADF, "handle is read-only");
        return found->second;
    }
};

Image::Image(const std::string& path) : impl_(std::make_unique<Impl>(path)) {}
Image::~Image() {
    // Open but unlinked files survive until their last handle closes.
    try {
        std::unordered_set<std::uint32_t> ids;
        for (const auto& [handle,file] : impl_->handles) { (void)handle; ids.insert(file.inode); }
        impl_->handles.clear(); std::fill(impl_->open_counts.begin(),impl_->open_counts.end(),0);
        impl_->transaction([&] { for (auto id : ids) { auto node = impl_->inode(id); if (!node.links) impl_->free_inode(id,node); } });
    } catch (...) { /* An I/O failure remains discoverable by offline fsck. */ }
}
Stat Image::stat(const std::string& path) {
    std::lock_guard lock(impl_->mutex); auto id = impl_->resolve(path); return disk::stat(id,impl_->inode(id));
}
Stat Image::fstat(Handle handle) {
    std::lock_guard lock(impl_->mutex); auto id = impl_->file(handle).inode; return disk::stat(id,impl_->inode(id));
}
void Image::create(const std::string& path, std::uint16_t mode) {
    std::lock_guard lock(impl_->mutex); impl_->transaction([&] { impl_->create_node(path,1,mode); });
}
Handle Image::create_open(const std::string& path, std::uint16_t mode, int flags) {
    std::lock_guard lock(impl_->mutex); std::uint32_t id = 0;
    impl_->transaction([&] { id = impl_->create_node(path,1,mode); }); return impl_->pin(id,flags);
}
Handle Image::open(const std::string& path, int flags) {
    std::lock_guard lock(impl_->mutex); auto id = impl_->resolve(path); auto node = impl_->inode(id);
    if (node.type != 1) fail(EISDIR, "open requires a regular file");
    if (flags & O_DIRECTORY) fail(ENOTDIR, "open requires a directory");
    if (flags & O_TRUNC) {
        if ((flags & O_ACCMODE) == O_RDONLY) fail(EACCES, "read-only truncate");
        impl_->transaction([&] { impl_->resize(node,0); impl_->save_inode(id,node); });
    }
    return impl_->pin(id,flags);
}
void Image::release(Handle handle) {
    std::lock_guard lock(impl_->mutex); auto id = impl_->file(handle).inode;
    impl_->handles.erase(handle); --impl_->open_counts[id]; auto node = impl_->inode(id);
    if (!node.links && !impl_->open_counts[id]) impl_->transaction([&] { impl_->free_inode(id,node); });
}
Bytes Image::read(Handle handle, std::uint64_t offset, std::size_t count) {
    std::lock_guard lock(impl_->mutex); auto file = impl_->file(handle);
    if ((file.flags & O_ACCMODE) == O_WRONLY) fail(EBADF, "handle is write-only");
    return impl_->read_data(impl_->inode(file.inode),offset,count);
}
std::size_t Image::write(Handle handle, std::uint64_t offset, std::span<const std::uint8_t> bytes) {
    std::lock_guard lock(impl_->mutex); auto file = impl_->file(handle,true); auto node = impl_->inode(file.inode);
    if (file.flags & O_APPEND) offset = node.size;
    impl_->transaction([&] { impl_->write_data(node,offset,bytes); impl_->save_inode(file.inode,node); }); return bytes.size();
}
void Image::truncate(const std::string& path, std::uint64_t size) {
    std::lock_guard lock(impl_->mutex); auto id = impl_->resolve(path); auto node = impl_->inode(id);
    if (node.type != 1) fail(EISDIR, "truncate requires a file");
    impl_->transaction([&] { impl_->resize(node,size); impl_->save_inode(id,node); });
}
void Image::ftruncate(Handle handle, std::uint64_t size) {
    std::lock_guard lock(impl_->mutex); auto id = impl_->file(handle,true).inode; auto node = impl_->inode(id);
    impl_->transaction([&] { impl_->resize(node,size); impl_->save_inode(id,node); });
}
void Image::unlink(const std::string& path) {
    std::lock_guard lock(impl_->mutex);
    impl_->transaction([&] {
        auto id = impl_->resolve(path); auto [parent,name] = impl_->parent(path);
        impl_->drop_link(id,false,parent); impl_->remove_entry(parent,name);
    });
}
void Image::mkdir(const std::string& path, std::uint16_t mode) {
    std::lock_guard lock(impl_->mutex); impl_->transaction([&] { impl_->create_node(path,2,mode); });
}
void Image::rmdir(const std::string& path) {
    std::lock_guard lock(impl_->mutex);
    impl_->transaction([&] {
        auto id = impl_->resolve(path); auto [parent,name] = impl_->parent(path);
        impl_->drop_link(id,true,parent); impl_->remove_entry(parent,name);
    });
}
void Image::link(const std::string& source, const std::string& destination) {
    std::lock_guard lock(impl_->mutex);
    impl_->transaction([&] {
        auto id = impl_->resolve(source); auto node = impl_->inode(id);
        if (node.type != 1) fail(EPERM,"directory hard links are forbidden");
        auto [parent,name] = impl_->parent(destination);
        if (destination.back() == '/') fail(ENOTDIR,"file destination has trailing slash");
        if (node.links == std::numeric_limits<std::uint32_t>::max()) fail(EMLINK,"link count limit");
        impl_->add_entry(parent,name,id); ++node.links; node.ctime = disk::timestamp(); impl_->save_inode(id,node);
    });
}
void Image::rename(const std::string& source, const std::string& destination) {
    std::lock_guard lock(impl_->mutex);
    impl_->transaction([&] {
        auto id = impl_->resolve(source); auto node = impl_->inode(id);
        auto [old_parent,old_name] = impl_->parent(source);
        auto [new_parent,new_name] = impl_->parent(destination);
        if (destination.back() == '/' && node.type != 2) fail(ENOTDIR,"file destination has trailing slash");
        auto target = impl_->find(impl_->inode(new_parent),new_name);
        if (target && target->entry.inode == id) return; // POSIX: two links to the same inode.
        if (node.type == 2) {
            auto ancestor = new_parent; std::unordered_set<std::uint32_t> seen;
            while (true) {
                if (ancestor == id) fail(EINVAL,"directory cannot move into its descendant");
                if (!seen.insert(ancestor).second) fail(EIO,"directory parent cycle");
                if (ancestor == 1) break;
                auto up = impl_->find(impl_->inode(ancestor),"..");
                if (!up) fail(EIO,"missing parent entry"); ancestor = up->entry.inode;
            }
        }
        if (target) {
            auto other = impl_->inode(target->entry.inode);
            if (other.type != node.type) fail(node.type == 2 ? ENOTDIR : EISDIR,"rename type mismatch");
            impl_->drop_link(target->entry.inode,other.type == 2,new_parent);
            impl_->remove_entry(new_parent,new_name);
        }
        impl_->remove_entry(old_parent,old_name); impl_->add_entry(new_parent,new_name,id);
        if (node.type == 2 && old_parent != new_parent) {
            auto up = impl_->find(node,".."); if (!up) fail(EIO,"missing parent entry");
            std::array<std::uint8_t,disk::directory_record_size> record{};
            disk::encode_record(record,new_parent,".."); impl_->write_data(node,up->offset,record);
            auto old = impl_->inode(old_parent); if (old.links < 3) fail(EIO,"invalid parent link count");
            --old.links; impl_->save_inode(old_parent,old);
            auto next = impl_->inode(new_parent); ++next.links; impl_->save_inode(new_parent,next);
        }
        node.ctime = disk::timestamp(); impl_->save_inode(id,node);
    });
}
std::vector<DirectoryEntry> Image::list(const std::string& path) {
    std::lock_guard lock(impl_->mutex); std::vector<DirectoryEntry> entries;
    for (auto& slot : impl_->records(impl_->inode(impl_->resolve(path))))
        if (slot.entry.inode) entries.push_back(std::move(slot.entry));
    return entries;
}
void Image::chmod(const std::string& path, std::uint16_t mode) {
    std::lock_guard lock(impl_->mutex); auto id = impl_->resolve(path); auto node = impl_->inode(id);
    impl_->transaction([&] { node.mode = mode & 0777; node.ctime = disk::timestamp(); impl_->save_inode(id,node); });
}
void Image::set_mtime(const std::string& path, std::uint64_t nanoseconds) {
    std::lock_guard lock(impl_->mutex); auto id = impl_->resolve(path); auto node = impl_->inode(id);
    impl_->transaction([&] { node.mtime = nanoseconds; node.ctime = disk::timestamp(); impl_->save_inode(id,node); });
}
Space Image::space() {
    std::lock_guard lock(impl_->mutex); Space result{impl_->super.blocks,0,impl_->super.inodes,0};
    for (std::uint32_t id = impl_->super.data_start; id < impl_->super.blocks; ++id)
        result.free_blocks += !impl_->bit(impl_->super.block_bitmap,id);
    for (std::uint32_t id = 2; id < impl_->super.inodes; ++id)
        result.free_inodes += !impl_->bit(impl_->super.inode_bitmap,id);
    return result;
}
void Image::sync() { std::lock_guard lock(impl_->mutex); impl_->synchronize(); }
IoStatistics Image::io_statistics() { std::lock_guard lock(impl_->mutex); return impl_->io; }
}

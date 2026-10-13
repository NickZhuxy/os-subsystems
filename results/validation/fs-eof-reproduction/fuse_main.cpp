#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include "os/fs.hpp"
#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <limits>
#include <system_error>

namespace callbacks {
struct Mount {
    os::fs::Image image;
    std::mutex mutex;
    struct Directory { os::fs::Stat attributes; std::vector<os::fs::DirectoryEntry> entries; };
    std::unordered_map<std::uint64_t,Directory> directories;
    std::uint64_t next_directory = 1ULL << 63;
    explicit Mount(const char* path) : image(path) {}
};
Mount& state() { return *static_cast<Mount*>(fuse_get_context()->private_data); }
template<class F> int guarded(F operation) noexcept {
    try { return operation(); }
    catch (const std::system_error& e) { return -e.code().value(); }
    catch (const std::bad_alloc&) { return -ENOMEM; }
    catch (...) { return -EIO; }
}
void fill_stat(const os::fs::Stat& value, struct stat& result) {
    result = {}; result.st_ino = value.inode; result.st_nlink = value.links;
    result.st_uid = value.uid; result.st_gid = value.gid;
    result.st_mode = (value.directory ? S_IFDIR : S_IFREG) | value.mode;
    result.st_size = value.size; result.st_blksize = os::fs::block_size;
    result.st_mtim = {static_cast<time_t>(value.mtime_ns / 1000000000),static_cast<long>(value.mtime_ns % 1000000000)};
    result.st_ctim = {static_cast<time_t>(value.ctime_ns / 1000000000),static_cast<long>(value.ctime_ns % 1000000000)};
    result.st_atim = result.st_mtim; // This format does not track access time.
}
int attributes(const char* path, struct stat* result, fuse_file_info* file) {
    return guarded([&] {
        os::fs::Stat value;
        if (file && (file->fh >> 63)) {
            std::lock_guard lock(state().mutex); value = state().directories.at(file->fh).attributes;
        } else value = file ? state().image.fstat(file->fh) : state().image.stat(path);
        fill_stat(value,*result); return 0;
    });
}
int create(const char* path, mode_t mode, fuse_file_info* file) {
    return guarded([&] { file->fh = state().image.create_open(path,mode,file->flags); file->direct_io = 1; return 0; });
}
int open(const char* path, fuse_file_info* file) {
    return guarded([&] { file->fh = state().image.open(path,file->flags); file->direct_io = 1; return 0; });
}
int release(const char*, fuse_file_info* file) {
    return guarded([&] { state().image.release(file->fh); return 0; });
}
int read(const char*, char* buffer, std::size_t size, off_t offset, fuse_file_info* file) {
    return guarded([&] {
        if (offset < 0 || size > std::numeric_limits<int>::max()) return -EINVAL;
        auto data = state().image.read(file->fh,offset,size); std::memcpy(buffer,data.data(),data.size()); return static_cast<int>(data.size());
    });
}
int write(const char*, const char* buffer, std::size_t size, off_t offset, fuse_file_info* file) {
    return guarded([&] {
        if (offset < 0 || size > 1024 * 1024) return -EINVAL;
        return static_cast<int>(state().image.write(file->fh,offset,{reinterpret_cast<const std::uint8_t*>(buffer),size}));
    });
}
int truncate(const char* path, off_t size, fuse_file_info* file) {
    return guarded([&] {
        if (size < 0) return -EINVAL;
        if (file) state().image.ftruncate(file->fh,size); else state().image.truncate(path,size); return 0;
    });
}
int mkdir(const char* path, mode_t mode) { return guarded([&] { state().image.mkdir(path,mode); return 0; }); }
int unlink(const char* path) { return guarded([&] { state().image.unlink(path); return 0; }); }
int rmdir(const char* path) { return guarded([&] { state().image.rmdir(path); return 0; }); }
int link(const char* source, const char* to) { return guarded([&] { state().image.link(source,to); return 0; }); }
int rename(const char* source, const char* to, unsigned flags) {
    return guarded([&] { if (flags) return -EOPNOTSUPP; state().image.rename(source,to); return 0; });
}
int chmod(const char* path, mode_t mode, fuse_file_info*) {
    return guarded([&] { if (!path) return -EOPNOTSUPP; state().image.chmod(path,mode); return 0; });
}
int utimens(const char* path, const timespec times[2], fuse_file_info*) {
    return guarded([&] {
        if (!path) return -EOPNOTSUPP;
        if (times[1].tv_nsec == UTIME_OMIT) return 0;
        timespec value = times[1]; if (value.tv_nsec == UTIME_NOW) clock_gettime(CLOCK_REALTIME,&value);
        if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000) return -EINVAL;
        state().image.set_mtime(path,static_cast<std::uint64_t>(value.tv_sec) * 1000000000 + value.tv_nsec); return 0;
    });
}
int sync(const char*, int, fuse_file_info*) { return guarded([&] { state().image.sync(); return 0; }); }
int statfs(const char*, struct statvfs* result) {
    return guarded([&] {
        auto space = state().image.space(); *result = {}; result->f_bsize = result->f_frsize = os::fs::block_size;
        result->f_blocks = space.blocks; result->f_bfree = result->f_bavail = space.free_blocks;
        result->f_files = space.inodes; result->f_ffree = result->f_favail = space.free_inodes; result->f_namemax = 56; return 0;
    });
}
int opendir(const char* path, fuse_file_info* file) {
    return guarded([&] {
        auto entries = state().image.list(path); auto attributes = state().image.stat(path); std::lock_guard lock(state().mutex);
        if (state().directories.size() >= 65536 || state().next_directory == UINT64_MAX) return -EMFILE;
        auto id = state().next_directory++; state().directories.emplace(id,Mount::Directory{attributes,std::move(entries)}); file->fh = id; return 0;
    });
}
int readdir(const char*, void* buffer, fuse_fill_dir_t filler, off_t offset, fuse_file_info* file, fuse_readdir_flags) {
    return guarded([&] {
        if (offset < 0) return -EINVAL;
        std::lock_guard lock(state().mutex); auto found = state().directories.find(file->fh);
        if (found == state().directories.end()) return -EBADF;
        const auto& entries = found->second.entries;
        for (auto i = static_cast<std::size_t>(offset); i < entries.size(); ++i) {
            struct stat metadata{}; metadata.st_ino = entries[i].inode;
            if (filler(buffer,entries[i].name.c_str(),&metadata,i + 1,static_cast<fuse_fill_dir_flags>(0))) break;
        }
        return 0;
    });
}
int releasedir(const char*, fuse_file_info* file) {
    return guarded([&] { std::lock_guard lock(state().mutex); state().directories.erase(file->fh); return 0; });
}
void* init(fuse_conn_info* connection, fuse_config* config) {
    config->entry_timeout = config->attr_timeout = config->negative_timeout = 0;
    config->use_ino = 1; config->nullpath_ok = 1;
    connection->max_write = 1024 * 1024; return fuse_get_context()->private_data;
}
}
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: fs_mount IMAGE MOUNTPOINT (foreground, owner-only mount)\n"; return 1; }
    try {
        callbacks::Mount mount(argv[1]); fuse_operations ops{};
        ops.getattr = callbacks::attributes; ops.create = callbacks::create; ops.open = callbacks::open;
        ops.release = callbacks::release; ops.read = callbacks::read; ops.write = callbacks::write;
        ops.truncate = callbacks::truncate; ops.mkdir = callbacks::mkdir; ops.unlink = callbacks::unlink;
        ops.rmdir = callbacks::rmdir; ops.link = callbacks::link; ops.rename = callbacks::rename;
        ops.chmod = callbacks::chmod; ops.utimens = callbacks::utimens; ops.fsync = callbacks::sync;
        ops.fsyncdir = callbacks::sync; ops.statfs = callbacks::statfs;
        ops.opendir = callbacks::opendir; ops.readdir = callbacks::readdir; ops.releasedir = callbacks::releasedir; ops.init = callbacks::init;
        char foreground[] = "-f", option[] = "-o", permissions[] = "default_permissions";
        char* arguments[]{argv[0],argv[2],foreground,option,permissions,nullptr};
        return fuse_main(5,arguments,&ops,&mount);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

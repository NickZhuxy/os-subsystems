#include "os/fs.hpp"
#include "os/test.hpp"
#include "format.hpp"
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <iostream>
using namespace os::fs;
namespace {
struct Temporary {
    std::string directory;
    Temporary() { char name[] = "/tmp/os-fs-test-XXXXXX"; auto result = mkdtemp(name); CHECK(result); directory = result; }
    ~Temporary() { std::filesystem::remove_all(directory); }
    std::string image(const std::string& name = "image") const { return directory + '/' + name; }
};
template<class F> void error(int expected, F operation) {
    try { operation(); } catch (const std::system_error& e) { CHECK(e.code().value() == expected); return; }
    throw std::runtime_error("expected errno " + std::to_string(expected));
}
Bytes contents(const std::string& path) {
    std::ifstream stream(path,std::ios::binary); return Bytes(std::istreambuf_iterator<char>(stream),{});
}
void clean(const std::string& path) {
    auto report = check(path);
    for (const auto& issue : report.issues) std::cerr << issue << '\n';
    CHECK(report.clean()); CHECK(report.allocated_inodes == report.reachable_inodes);
}
void namespace_operations() {
    Temporary t; format(t.image()); error(EEXIST,[&] { format(t.image()); });
    {
        Image fs(t.image()); CHECK(fs.stat("/").links == 2);
        error(EWOULDBLOCK,[&] { Image duplicate(t.image()); }); CHECK(!check(t.image()).clean());
        fs.mkdir("/a"); fs.mkdir("/b"); fs.mkdir("/a/sub");
        CHECK(fs.stat("/").links == 4); CHECK(fs.stat("/a").links == 3);
        auto h = fs.create_open("/a/file",0640,O_RDWR); Bytes bytes{0,1,2,0,255};
        CHECK(fs.write(h,0,bytes) == bytes.size()); fs.link("/a/file","/b/link");
        CHECK(fs.stat("/b/link").links == 2); fs.rename("/a/file","/b/link");
        CHECK(fs.stat("/a/file").links == 2); // Same inode rename is a no-op.
        fs.unlink("/a/file"); fs.rename("/b/link","/b/renamed");
        auto generation = fs.fstat(h).generation; auto id = fs.fstat(h).inode;
        fs.unlink("/b/renamed"); CHECK(fs.fstat(h).links == 0); CHECK(fs.read(h,0,10) == bytes);
        fs.create("/replacement"); CHECK(fs.stat("/replacement").inode != id);
        fs.release(h); error(EBADF,[&] { fs.read(h,0,1); });
        fs.create("/reused"); CHECK(fs.stat("/reused").inode == id); CHECK(fs.stat("/reused").generation > generation);
        fs.rename("/a/sub","/b/sub"); CHECK(fs.stat("/b/sub/..").inode == fs.stat("/b").inode);
        CHECK(fs.stat("/a").links == 2); CHECK(fs.stat("/b").links == 3);
        error(EINVAL,[&] { fs.rename("/b","/b/sub/child"); });
        error(ENOTEMPTY,[&] { fs.rmdir("/b"); }); error(EPERM,[&] { fs.link("/a","/dirlink"); });
        fs.mkdir("/a/empty"); fs.rename("/b/sub","/a/empty");
        CHECK(fs.stat("/a").links == 3); CHECK(fs.stat("/b").links == 2);
        fs.rmdir("/a/empty"); fs.rmdir("/a"); fs.rmdir("/b"); CHECK(fs.stat("/").links == 2);
        error(EBUSY,[&] { fs.rmdir("/"); }); error(EINVAL,[&] { fs.rmdir("/."); });
        error(ENOTDIR,[&] { fs.stat("/reused/"); }); error(EBUSY,[&] { fs.unlink("/"); });
        fs.chmod("/reused",0600); CHECK(fs.stat("/reused").mode == 0600);
        fs.set_mtime("/reused",123456); CHECK(fs.stat("/reused").mtime_ns == 123456);
        auto read_only = fs.open("/reused",O_RDONLY); error(EBADF,[&] { fs.write(read_only,0,bytes); }); fs.release(read_only);
        auto append = fs.open("/reused",O_WRONLY | O_APPEND); fs.write(append,900,bytes); fs.write(append,0,bytes);
        CHECK(fs.fstat(append).size == 10); error(EBADF,[&] { fs.read(append,0,1); }); fs.release(append);
    }
    clean(t.image());
}
void addressing_and_truncation() {
    Temporary t; format(t.image());
    {
        Image fs(t.image()); auto baseline = fs.space().free_blocks;
        auto h = fs.create_open("/sparse",0644,O_RDWR); Bytes data(7000);
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = i % 251;
        for (std::uint64_t boundary : {8ULL * block_size,(8ULL + 1024) * block_size}) {
            auto offset = boundary - 13; fs.write(h,offset,data);
            CHECK(fs.read(h,offset,data.size()) == data); CHECK(fs.read(h,offset - 1,1) == Bytes{0});
        }
        CHECK(fs.read(h,0,1000) == Bytes(1000,0));
        fs.ftruncate(h,8 * block_size + 5); fs.ftruncate(h,8 * block_size + 100);
        CHECK(fs.read(h,8 * block_size + 5,95) == Bytes(95,0));
        fs.ftruncate(h,max_file_size); CHECK(fs.read(h,max_file_size - 1,1) == Bytes{0});
        fs.write(h,max_file_size - 1,Bytes{77}); CHECK(fs.read(h,max_file_size - 1,1) == Bytes{77});
        error(EFBIG,[&] { fs.write(h,max_file_size,Bytes{1}); });
        fs.ftruncate(h,0); CHECK(fs.space().free_blocks == baseline); fs.release(h);
    }
    clean(t.image());
}
void failed_transaction() {
    Temporary t; os::fs::format(t.image(),32,8);
    {
        Image fs(t.image()); auto h = fs.create_open("/full",0644,O_RDWR);
        auto before = contents(t.image()); auto space = fs.space();
        error(ENOSPC,[&] { fs.write(h,0,Bytes(32 * block_size,9)); });
        CHECK(contents(t.image()) == before); CHECK(fs.fstat(h).size == 0); CHECK(fs.space().free_blocks == space.free_blocks);
        fs.write(h,0,Bytes(5,3)); fs.release(h);
        for (int i = 0; i < 5; ++i) fs.create("/f" + std::to_string(i));
        before = contents(t.image()); error(ENOSPC,[&] { fs.mkdir("/last"); }); CHECK(contents(t.image()) == before);
    }
    clean(t.image());
}
void bitmap_boundaries() {
    Temporary t; os::fs::format(t.image(),32775,32769);
    auto geometry = disk::geometry(32775,32769);
    {
        Image fs(t.image()); auto before = fs.space();
        CHECK(before.free_blocks == 32775 - geometry.data_start - 1); CHECK(before.free_inodes == 32767);
        auto h = fs.create_open("/x",0644,O_WRONLY); fs.write(h,0,Bytes{1}); fs.release(h);
        auto after = fs.space(); CHECK(after.free_blocks + 1 == before.free_blocks); CHECK(after.free_inodes + 1 == before.free_inodes);
    }
    clean(t.image());
}
void randomized_and_concurrent() {
    Temporary t; os::fs::format(t.image(),4096,128); std::mt19937 random(7129);
    {
        Image fs(t.image()); std::vector<Bytes> oracle(8); std::vector<Handle> handles;
        for (int i = 0; i < 8; ++i) handles.push_back(fs.create_open("/f" + std::to_string(i),0644,O_RDWR));
        for (int i = 0; i < 500; ++i) {
            auto index = random() % 8; auto& expected = oracle[index];
            if (random() % 3 == 0) { auto size = random() % 70000; fs.ftruncate(handles[index],size); expected.resize(size,0); }
            else {
                auto offset = random() % 70000; Bytes value(1 + random() % 8000);
                for (auto& byte : value) byte = random();
                fs.write(handles[index],offset,value); if (expected.size() < offset + value.size()) expected.resize(offset + value.size(),0);
                std::copy(value.begin(),value.end(),expected.begin() + offset);
            }
            CHECK(fs.read(handles[index],0,100000) == expected); CHECK(fs.fstat(handles[index]).size == expected.size());
        }
        for (auto h : handles) fs.release(h);
        std::vector<std::thread> threads;
        for (int worker = 0; worker < 4; ++worker) threads.emplace_back([&,worker] {
            auto h = fs.create_open("/thread" + std::to_string(worker),0644,O_RDWR);
            for (int i = 0; i < 20; ++i) fs.write(h,i * 10,Bytes(10,static_cast<std::uint8_t>(worker)));
            CHECK(fs.read(h,0,200) == Bytes(200,static_cast<std::uint8_t>(worker))); fs.release(h);
        });
        for (auto& thread : threads) thread.join();
    }
    clean(t.image());
}
void corruption_detection() {
    Temporary t; format(t.image());
    { Image fs(t.image()); auto h = fs.create_open("/x",0644,O_RDWR); fs.write(h,0,Bytes(8192,3)); fs.release(h); }
    clean(t.image());
    auto original = contents(t.image());
    auto corrupt = [&](const std::string& name, std::size_t offset, std::uint64_t value, unsigned count, const std::string& expected) {
        auto bytes = original; disk::put(bytes,offset,value,count);
        auto path = t.image(name); std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); out.close();
        auto report = check(path); CHECK(!report.clean());
        CHECK(std::any_of(report.issues.begin(),report.issues.end(),[&](const auto& text) { return text.find(expected) != std::string::npos; }));
    };
    auto s = disk::geometry(8192,256); auto inode = s.inode_table * block_size + 2 * disk::inode_size;
    auto first = disk::get(original,inode + 48,4);
    corrupt("bounds",inode + 48,8192,4,"invalid block pointer");
    corrupt("duplicate",inode + 52,first,4,"multiply referenced");
    corrupt("links",inode + 4,7,4,"link count mismatch");
    corrupt("super",0,0,1,"superblock");
    corrupt("free",s.block_bitmap * block_size + first / 8,
        original[s.block_bitmap * block_size + first / 8] & ~(1U << (first % 8)),1,"referenced block marked free");
    corrupt("orphan",s.data_start * block_size + 2 * disk::directory_record_size,0,8,"unreachable allocated inode");
}
}
int main() {
    try { namespace_operations(); addressing_and_truncation(); failed_transaction(); bitmap_boundaries(); randomized_and_concurrent(); corruption_detection();
        std::cout << "filesystem: namespace, sparse pointers, ENOSPC rollback, 500 oracle operations, 4 threads, 6 corruptions passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

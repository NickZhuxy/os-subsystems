#include "os/fs.hpp"
#include "os/cli.hpp"
#include "json.hpp"
#include <fcntl.h>
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
    try {
        os::Arguments args(argc,argv); auto image = args.take("--image"), op = args.take("--op"), path = args.take("--path","/");
        auto to = args.take("--to"), input = args.take("--input"), output = args.take("--output");
        auto offset = args.number("--offset",0,0,os::fs::max_file_size);
        auto size = args.number("--size",0,0,os::fs::max_file_size); args.finish();
        if (image.empty() || op.empty()) throw std::invalid_argument("--image and --op are required");
        os::fs::Image fs(image);
        if (op == "create") fs.create(path);
        else if (op == "mkdir") fs.mkdir(path);
        else if (op == "unlink") fs.unlink(path);
        else if (op == "rmdir") fs.rmdir(path);
        else if (op == "rename") fs.rename(path,to);
        else if (op == "link") fs.link(path,to);
        else if (op == "truncate") fs.truncate(path,size);
        else if (op == "stat") {
            auto stat = fs.stat(path);
            std::cout << "{\"inode\":" << stat.inode << ",\"size\":" << stat.size << ",\"links\":" << stat.links
                << ",\"generation\":" << stat.generation << ",\"directory\":" << (stat.directory ? "true" : "false") << "}\n";
        } else if (op == "ls") {
            std::cout << '['; bool first = true;
            for (const auto& entry : fs.list(path)) {
                if (!first) std::cout << ','; first = false;
                std::cout << "{\"inode\":" << entry.inode << ",\"name\":"; quoted(std::cout,entry.name); std::cout << '}';
            }
            std::cout << "]\n";
        } else if (op == "put" || op == "write") {
            std::ifstream stream(input,std::ios::binary); if (!stream) throw std::runtime_error("cannot open --input");
            os::fs::Handle handle;
            if (op == "put") {
                try { handle = fs.open(path,O_WRONLY | O_TRUNC); }
                catch (const std::system_error& e) {
                    if (e.code().value() != ENOENT) throw;
                    handle = fs.create_open(path,0644,O_WRONLY);
                }
            } else handle = fs.open(path,O_WRONLY);
            os::fs::Bytes buffer(1024 * 1024);
            while (stream.read(reinterpret_cast<char*>(buffer.data()),buffer.size()) || stream.gcount()) {
                auto count = static_cast<std::size_t>(stream.gcount()); fs.write(handle,offset,std::span(buffer).first(count)); offset += count;
            }
            if (!stream.eof()) throw std::runtime_error("input read failed"); fs.release(handle);
        } else if (op == "get") {
            if (output.empty()) throw std::invalid_argument("--output is required for get");
            auto handle = fs.open(path,O_RDONLY); std::ofstream stream(output,std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("cannot open --output");
            while (true) {
                auto buffer = fs.read(handle,offset,65536); if (buffer.empty()) break;
                stream.write(reinterpret_cast<const char*>(buffer.data()),buffer.size()); offset += buffer.size();
            }
            stream.close(); if (!stream) throw std::runtime_error("output write failed"); fs.release(handle);
        } else throw std::invalid_argument("unknown --op");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

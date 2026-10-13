#include "os/fs.hpp"
#include "os/cli.hpp"
#include <iostream>
int main(int argc, char** argv) {
    try {
        os::Arguments args(argc,argv); auto path = args.take("--image");
        auto blocks = args.number("--blocks",8192,32,1048576), inodes = args.number("--inodes",256,8,65536);
        args.finish(); if (path.empty()) throw std::invalid_argument("--image is required");
        os::fs::format(path,blocks,inodes);
        std::cout << "{\"block_size\":4096,\"blocks\":" << blocks << ",\"inodes\":" << inodes << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

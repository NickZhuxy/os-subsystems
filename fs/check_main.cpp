#include "os/fs.hpp"
#include "os/cli.hpp"
#include "json.hpp"
#include <iostream>
int main(int argc, char** argv) {
    try {
        os::Arguments args(argc,argv); auto path = args.take("--image"); args.finish();
        if (path.empty()) throw std::invalid_argument("--image is required");
        auto report = os::fs::check(path);
        std::cout << "{\"clean\":" << (report.clean() ? "true" : "false")
            << ",\"allocated_inodes\":" << report.allocated_inodes << ",\"reachable_inodes\":" << report.reachable_inodes
            << ",\"referenced_data_blocks\":" << report.referenced_blocks << ",\"issues\":[";
        bool first = true;
        for (const auto& issue : report.issues) { if (!first) std::cout << ','; first = false; quoted(std::cout,issue); }
        std::cout << "]}\n"; return report.clean() ? 0 : 2;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

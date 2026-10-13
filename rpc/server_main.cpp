#include "os/rpc.hpp"
#include "os/cli.hpp"
#include <csignal>
#include <iostream>
#include <thread>
namespace { volatile std::sig_atomic_t interrupted = 0; void signal_handler(int) { interrupted = 1; } }
int main(int argc, char** argv) {
    try {
        os::Arguments args(argc, argv);
        os::rpc::ServerOptions options;
        options.port = static_cast<std::uint16_t>(args.number("--port", 9000, 0, 65535));
        args.finish();
        os::rpc::Server server(options);
        std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
        std::atomic<bool> finished{false};
        std::exception_ptr error;
        std::thread worker([&] { try { server.run(); } catch (...) { error = std::current_exception(); } finished.store(true); });
        std::cout << "PORT " << server.port() << std::endl;
        while (!interrupted && !finished.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        server.stop(); worker.join();
        if (error) std::rethrow_exception(error);
        std::cerr << "peak_connections=" << server.peak_connections() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

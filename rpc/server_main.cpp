#include "os/rpc.hpp"
#include "os/cli.hpp"
#include <csignal>
#include <pthread.h>
#include <iostream>
#include <thread>
int main(int argc, char** argv) {
    try {
        os::Arguments args(argc, argv);
        os::rpc::ServerOptions options;
        options.port = static_cast<std::uint16_t>(args.number("--port", 9000, 0, 65535));
        args.finish();
        os::rpc::Server server(options);
        sigset_t signals;
        sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
        int mask_error = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        if (mask_error) throw std::system_error(mask_error, std::generic_category(), "pthread_sigmask");
        std::atomic<bool> finished{false};
        std::exception_ptr error;
        std::thread worker([&] { try { server.run(); } catch (...) { error = std::current_exception(); } finished.store(true); });
        std::cout << "PORT " << server.port() << std::endl;
        int wait_error = 0;
        while (!finished.load()) {
            timespec timeout{0, 20000000};
            int signal = sigtimedwait(&signals, nullptr, &timeout);
            if (signal == SIGINT || signal == SIGTERM) break;
            if (signal < 0 && errno != EAGAIN && errno != EINTR) { wait_error = errno; break; }
        }
        server.stop(); worker.join();
        if (wait_error) throw std::system_error(wait_error, std::generic_category(), "sigtimedwait");
        if (error) std::rethrow_exception(error);
        std::cerr << "peak_connections=" << server.peak_connections() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

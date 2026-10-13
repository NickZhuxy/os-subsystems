#pragma once
#include "os/fd.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace os::rpc {
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr std::size_t header_size = 20;
constexpr std::size_t max_payload = 65536;
enum class Type : std::uint16_t { request = 1, response = 2, error = 3 };
struct Frame { Type type; std::uint64_t id; Bytes body; };
struct Decoded { Frame frame; std::size_t consumed; };
Bytes encode(const Frame& frame);
std::optional<Decoded> decode(std::span<const std::uint8_t> input);
void put_u64(Bytes& bytes, std::uint64_t value);
std::uint64_t get_u64(std::span<const std::uint8_t> bytes);
Bytes echo_request(std::span<const std::uint8_t> body);
Bytes add_request(std::uint64_t left, std::uint64_t right);
Bytes delayed_echo_request(std::uint64_t delay_ms, std::span<const std::uint8_t> body);
struct Reply { Bytes body; std::chrono::milliseconds delay{0}; };
using Handler = std::function<Reply(std::span<const std::uint8_t>)>;
Reply builtin_handler(std::span<const std::uint8_t> request);
struct ServerOptions {
    std::uint16_t port = 0;
    std::size_t max_connections = 4096;
    std::size_t max_inflight = 256;
    std::size_t max_buffer = 4 * 1024 * 1024;
    std::chrono::milliseconds idle_timeout{30000};
    std::chrono::milliseconds frame_timeout{2000};
    std::chrono::milliseconds request_timeout{2000};
    int socket_send_buffer = 0;
};
class Server {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Server(ServerOptions options = {}, Handler handler = builtin_handler);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    std::uint16_t port() const;
    void run(); // One thread owns all connections. Handler calls must not block.
    void stop() noexcept; // Thread-safe wakeup; join run() before destruction.
    std::size_t peak_connections() const;
};
struct Sample { std::size_t connection; std::uint64_t id; double latency_us; };
struct LoadOptions {
    std::uint16_t port = 0;
    std::size_t connections = 64;
    std::size_t requests_per_connection = 200;
    std::size_t warmup_per_connection = 20;
    std::size_t pipeline = 4;
    std::size_t payload_size = 64;
    std::chrono::milliseconds timeout{5000};
};
struct LoadResult {
    std::vector<Sample> samples;
    double elapsed_seconds = 0;
};
// Fails the whole run on any timeout, duplicate, wrong payload, or lost connection.
LoadResult load(const LoadOptions& options);
double percentile(const std::vector<Sample>& samples, double fraction);
Fd connect_loopback(std::uint16_t port, std::chrono::milliseconds timeout);
}

#include "os/rpc.hpp"
#include "os/test.hpp"
#include <poll.h>
#include <sys/socket.h>
#include <array>
#include <iostream>
#include <random>
#include <thread>
using namespace os::rpc;
using namespace std::chrono_literals;
namespace {
struct Running {
    Server server;
    std::exception_ptr error;
    std::thread worker;
    explicit Running(ServerOptions options = {}, Handler handler = builtin_handler)
        : server(options, std::move(handler)), worker([this] {
            try { server.run(); } catch (...) { error = std::current_exception(); }
        }) {}
    ~Running() { server.stop(); if (worker.joinable()) worker.join(); }
    void finish() { server.stop(); worker.join(); if (error) std::rethrow_exception(error); }
};
void ready(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) throw std::runtime_error("test socket deadline");
        pollfd event{fd, events, 0};
        auto count = poll(&event, 1, static_cast<int>(remaining));
        if (count > 0) return;
        if (count < 0 && errno != EINTR) os::system_error("test poll");
    }
}
struct Peer {
    os::Fd fd;
    Bytes input;
    explicit Peer(std::uint16_t port) : fd(connect_loopback(port, 3s)) {}
    void send(std::span<const std::uint8_t> bytes) {
        auto deadline = Clock::now() + 5s;
        while (!bytes.empty()) {
            auto count = ::send(fd.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) { ready(fd.get(), POLLOUT, deadline); continue; }
                os::system_error("test send");
            }
            CHECK(count > 0); bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
    }
    Frame receive() {
        auto deadline = Clock::now() + 5s;
        for (;;) {
            if (auto decoded = decode(input)) {
                auto frame = std::move(decoded->frame);
                input.erase(input.begin(), input.begin() + decoded->consumed); return frame;
            }
            ready(fd.get(), POLLIN, deadline);
            std::array<std::uint8_t, 8192> bytes{};
            auto count = recv(fd.get(), bytes.data(), bytes.size(), 0);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            CHECK(count > 0); input.insert(input.end(), bytes.begin(), bytes.begin() + count);
        }
    }
    void closed() {
        auto deadline = Clock::now() + 3s;
        for (;;) {
            ready(fd.get(), POLLIN, deadline);
            std::array<std::uint8_t, 8192> bytes{};
            auto count = recv(fd.get(), bytes.data(), bytes.size(), 0);
            if (count == 0 || (count < 0 && errno == ECONNRESET)) return;
            if (count < 0 && errno != EINTR && errno != EAGAIN) os::system_error("test close");
        }
    }
};
void protocol() {
    auto bytes = encode({Type::request, 0x0102030405060708ULL, Bytes{0, 1, 255}});
    CHECK(bytes == (Bytes{0x4f,0x53,0x52,0x50,0,1,0,1,1,2,3,4,5,6,7,8,0,0,0,3,0,1,255}));
    for (std::size_t i = 0; i < bytes.size(); ++i) CHECK(!decode(std::span(bytes).first(i)));
    auto parsed = decode(bytes);
    CHECK(parsed && parsed->consumed == bytes.size());
    CHECK(parsed->frame.id == 0x0102030405060708ULL);
    for (auto index : {0U, 5U, 7U, 16U}) {
        auto bad = bytes; bad[index] = 255;
        check_throws<std::invalid_argument>([&] { decode(bad); });
    }
    auto bad = bytes; std::fill(bad.begin() + 8, bad.begin() + 16, 0);
    check_throws<std::invalid_argument>([&] { decode(bad); });
    check_throws<std::invalid_argument>([] { encode({Type::request, 0, {}}); });
    check_throws<std::invalid_argument>([] { encode({Type::request, 1, Bytes(max_payload + 1)}); });
    std::mt19937 random(17);
    for (int i = 0; i < 10000; ++i) {
        Bytes input(random() % 96);
        for (auto& b : input) b = static_cast<std::uint8_t>(random());
        try { (void)decode(input); } catch (const std::invalid_argument&) {}
    }
    CHECK(percentile({{0,1,3}, {0,2,1}, {0,3,2}, {0,4,4}}, .5) == 2);
}
void framing_and_multiplexing() {
    Running running;
    Peer peer(running.server.port());
    auto bytes = encode({Type::request, 1, echo_request(Bytes{0, 99, 255})});
    for (auto b : bytes) peer.send(Bytes{b});
    auto reply = peer.receive(); CHECK(reply.id == 1 && reply.body == (Bytes{0,99,255}));
    Bytes batch = encode({Type::request, 2, delayed_echo_request(60, Bytes{42})});
    auto fast = encode({Type::request, 3, add_request(5, 7)});
    batch.insert(batch.end(), fast.begin(), fast.end()); peer.send(batch);
    reply = peer.receive(); CHECK(reply.id == 3 && get_u64(reply.body) == 12);
    reply = peer.receive(); CHECK(reply.id == 2 && reply.body == Bytes{42});
    peer.send(encode({Type::request, 4, Bytes{255}}));
    CHECK(peer.receive().type == Type::error);
    peer.send(encode({Type::request, 5, echo_request({})}));
    CHECK(shutdown(peer.fd.get(), SHUT_WR) == 0);
    reply = peer.receive(); CHECK(reply.id == 5 && reply.body.empty()); peer.closed();
    running.finish();
}
void malformed_and_timeouts() {
    ServerOptions options; options.idle_timeout = 300ms; options.frame_timeout = 70ms;
    options.request_timeout = 60ms;
    Running running(options);
    {
        Peer peer(running.server.port());
        auto bad = encode({Type::request, 1, {}}); bad[16] = 1;
        peer.send(bad); peer.closed();
    }
    {
        Peer peer(running.server.port());
        auto frame = encode({Type::request, 1, delayed_echo_request(200, Bytes{1})});
        auto duplicate = frame; frame.insert(frame.end(), duplicate.begin(), duplicate.end());
        peer.send(frame); peer.closed();
    }
    {
        Peer peer(running.server.port()); peer.send(Bytes{0x4f}); peer.closed();
    }
    {
        Peer peer(running.server.port()); peer.closed();
    }
    {
        Peer peer(running.server.port());
        peer.send(encode({Type::request, 10, delayed_echo_request(200, Bytes{1})}));
        auto reply = peer.receive(); CHECK(reply.id == 10 && reply.type == Type::error);
        CHECK(std::string(reply.body.begin(), reply.body.end()) == "request timeout");
    }
    running.finish();
}
void backpressure_and_limits() {
    ServerOptions options; options.socket_send_buffer = 1024;
    Running running(options);
    Peer peer(running.server.port());
    Bytes body(60000);
    for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<std::uint8_t>(i);
    Bytes batch;
    for (std::uint64_t i = 1; i <= 16; ++i) {
        auto bytes = encode({Type::request, i, echo_request(body)});
        batch.insert(batch.end(), bytes.begin(), bytes.end());
    }
    peer.send(batch);
    std::this_thread::sleep_for(30ms);
    for (std::uint64_t i = 1; i <= 16; ++i) { auto reply = peer.receive(); CHECK(reply.id == i && reply.body == body); }
    running.finish();
    options.max_inflight = 2;
    Running limited(options);
    Peer many(limited.server.port());
    for (std::uint64_t i = 1; i <= 3; ++i) many.send(encode({Type::request, i, delayed_echo_request(100, {})}));
    many.closed(); limited.finish();
    options.max_inflight = 256; options.max_buffer = 128;
    Running buffer_limited(options);
    Peer overflowing(buffer_limited.server.port());
    Bytes combined;
    for (std::uint64_t i = 1; i <= 2; ++i) {
        auto frame = encode({Type::request, i, delayed_echo_request(100, Bytes(80, 42))});
        combined.insert(combined.end(), frame.begin(), frame.end());
    }
    overflowing.send(combined); overflowing.closed(); buffer_limited.finish();
    options.max_buffer = 4 * 1024 * 1024;
    options.max_connections = 1;
    Running capped(options);
    Peer first(capped.server.port()); first.send(encode({Type::request, 1, echo_request({})}));
    CHECK(first.receive().id == 1);
    Peer extra(capped.server.port()); extra.closed();
    CHECK(capped.server.peak_connections() == 1); capped.finish();
}
void client_and_load() {
    Running running;
    LoadOptions options; options.port = running.server.port(); options.connections = 64;
    options.pipeline = 8; options.requests_per_connection = 30; options.warmup_per_connection = 5;
    auto result = load(options);
    CHECK(result.samples.size() == 64 * 30); CHECK(result.elapsed_seconds > 0);
    CHECK(running.server.peak_connections() == 64);
    running.finish();
    Running stalled({}, [](std::span<const std::uint8_t> bytes) {
        auto reply = builtin_handler(bytes); reply.delay = 500ms; return reply;
    });
    options.port = stalled.server.port(); options.connections = 1; options.timeout = 30ms;
    check_throws<std::runtime_error>([&] { load(options); }); stalled.finish();
    Running wrong({}, [](std::span<const std::uint8_t>) { return Reply{Bytes{42}, 0ms}; });
    options.port = wrong.server.port(); options.timeout = 3s;
    check_throws<std::runtime_error>([&] { load(options); }); wrong.finish();
}
}
int main() {
    for (auto [name, test] : std::vector<std::pair<const char*, void(*)()>>{
        {"protocol", protocol}, {"framing and multiplexing", framing_and_multiplexing},
        {"malformed and timeouts", malformed_and_timeouts}, {"backpressure and limits", backpressure_and_limits},
        {"client and load", client_and_load}}) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; return 1; }
    }
}

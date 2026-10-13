#include "os/rpc.hpp"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <algorithm>
#include <array>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace os::rpc {
namespace {
constexpr std::size_t io_budget = 65536;
void control(int epoll, int op, int fd, std::uint64_t token, std::uint32_t events) {
    epoll_event event{}; event.events = events; event.data.u64 = token;
    if (epoll_ctl(epoll, op, fd, &event) < 0) os::system_error("epoll_ctl");
}
Frame error_frame(std::uint64_t id, const std::string& message) {
    return {Type::error, id, Bytes(message.begin(), message.begin() + std::min(message.size(), max_payload))};
}
}
struct Server::Impl {
    struct Outgoing { Bytes bytes; std::size_t offset = 0; std::uint64_t id; };
    struct Pending { Bytes bytes; std::uint64_t id; Clock::time_point due, deadline; };
    struct Connection {
        Fd fd;
        Bytes input;
        std::deque<Outgoing> output;
        std::vector<Pending> pending;
        std::unordered_set<std::uint64_t> inflight;
        std::size_t buffered = 0;
        Clock::time_point progress = Clock::now();
        std::optional<Clock::time_point> partial_since;
        bool eof = false;
        explicit Connection(int descriptor) : fd(descriptor) {}
    };
    ServerOptions options;
    Handler handler;
    Fd listener{socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    Fd epoll{epoll_create1(EPOLL_CLOEXEC)};
    Fd wake{eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    std::uint16_t bound_port = 0;
    std::atomic<bool> stopped{false};
    std::atomic<std::size_t> peak{0};
    std::uint64_t next_token = 2;
    std::unordered_map<std::uint64_t, Connection> connections;

    Impl(ServerOptions opts, Handler callback) : options(opts), handler(std::move(callback)) {
        if (!handler || !options.max_connections || !options.max_inflight ||
            options.max_buffer < header_size || options.idle_timeout.count() <= 0 ||
            options.frame_timeout.count() <= 0 || options.request_timeout.count() <= 0)
            throw std::invalid_argument("invalid server options");
        if (listener.get() < 0 || epoll.get() < 0 || wake.get() < 0) os::system_error("reactor setup");
        int yes = 1;
        if (setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
            os::system_error("SO_REUSEADDR");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(options.port);
        if (bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            os::system_error("bind");
        if (listen(listener.get(), 4096) < 0) os::system_error("listen");
        socklen_t size = sizeof(address);
        if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &size) < 0)
            os::system_error("getsockname");
        bound_port = ntohs(address.sin_port);
        control(epoll.get(), EPOLL_CTL_ADD, listener.get(), 0, EPOLLIN);
        control(epoll.get(), EPOLL_CTL_ADD, wake.get(), 1, EPOLLIN);
    }
    void accept_ready() {
        for (int i = 0; i < 64; ++i) {
            int fd = accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR) { --i; continue; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                os::system_error("accept4");
            }
            Connection connection(fd);
            if (connections.size() >= options.max_connections) continue;
            int yes = 1;
            if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0)
                os::system_error("TCP_NODELAY");
            if (options.socket_send_buffer > 0 && setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                &options.socket_send_buffer, sizeof(options.socket_send_buffer)) < 0)
                os::system_error("SO_SNDBUF");
            auto token = next_token++;
            control(epoll.get(), EPOLL_CTL_ADD, fd, token, EPOLLIN | EPOLLRDHUP);
            connections.emplace(token, std::move(connection));
            peak.store(std::max(peak.load(), connections.size()));
        }
    }
    void close(std::uint64_t token) {
        auto found = connections.find(token);
        if (found == connections.end()) return;
        epoll_ctl(epoll.get(), EPOLL_CTL_DEL, found->second.fd.get(), nullptr);
        connections.erase(found);
    }
    bool enqueue(Connection& connection, Bytes bytes, std::uint64_t id) {
        if (bytes.size() > options.max_buffer - connection.buffered) return false;
        connection.buffered += bytes.size();
        connection.output.push_back({std::move(bytes), 0, id});
        return true;
    }
    bool dispatch(Connection& connection, Frame frame) {
        if (frame.type != Type::request || connection.inflight.size() >= options.max_inflight ||
            !connection.inflight.insert(frame.id).second) return false;
        try {
            auto reply = handler(frame.body);
            if (reply.delay.count() < 0) throw std::invalid_argument("negative delay");
            auto bytes = encode({Type::response, frame.id, std::move(reply.body)});
            if (reply.delay.count() == 0) return enqueue(connection, std::move(bytes), frame.id);
            if (bytes.size() > options.max_buffer - connection.buffered) return false;
            connection.buffered += bytes.size();
            auto now = Clock::now();
            connection.pending.push_back({std::move(bytes), frame.id, now + reply.delay,
                                          now + options.request_timeout});
            return true;
        } catch (const std::invalid_argument& error) {
            return enqueue(connection, encode(error_frame(frame.id, error.what())), frame.id);
        }
    }
    bool read(Connection& connection) {
        std::array<std::uint8_t, 16384> buffer{};
        std::size_t budget = io_budget;
        while (budget > 0 && !connection.eof) {
            auto count = recv(connection.fd.get(), buffer.data(), std::min(budget, buffer.size()), 0);
            if (count < 0) {
                if (errno == EINTR) continue;
                return errno == EAGAIN || errno == EWOULDBLOCK;
            }
            if (count == 0) {
                connection.eof = true;
                return connection.input.empty(); // A truncated frame is a protocol error.
            }
            auto size = static_cast<std::size_t>(count);
            budget -= size;
            connection.progress = Clock::now();
            if (!connection.partial_since) connection.partial_since = connection.progress;
            connection.input.insert(connection.input.end(), buffer.begin(), buffer.begin() + size);
            std::size_t consumed = 0;
            try {
                while (auto parsed = decode(std::span(connection.input).subspan(consumed))) {
                    consumed += parsed->consumed;
                    if (!dispatch(connection, std::move(parsed->frame))) return false;
                }
            } catch (const std::invalid_argument&) { return false; }
            if (consumed) {
                connection.input.erase(connection.input.begin(), connection.input.begin() + consumed);
                connection.partial_since = connection.input.empty() ? std::nullopt :
                    std::optional<Clock::time_point>(connection.progress);
            }
            if (connection.input.size() > max_payload + header_size) return false;
        }
        return true;
    }
    bool write(Connection& connection) {
        std::size_t budget = io_budget;
        while (!connection.output.empty() && budget > 0) {
            auto& output = connection.output.front();
            auto count = send(connection.fd.get(), output.bytes.data() + output.offset,
                std::min(budget, output.bytes.size() - output.offset), MSG_NOSIGNAL);
            if (count < 0) {
                if (errno == EINTR) continue;
                return errno == EAGAIN || errno == EWOULDBLOCK;
            }
            if (count == 0) return false;
            auto size = static_cast<std::size_t>(count);
            budget -= size;
            output.offset += size;
            connection.progress = Clock::now();
            if (output.offset == output.bytes.size()) {
                connection.buffered -= output.bytes.size();
                connection.inflight.erase(output.id);
                connection.output.pop_front();
            }
        }
        return true;
    }
    void interest(std::uint64_t token, const Connection& connection) {
        std::uint32_t flags = connection.eof ? 0U : EPOLLIN | EPOLLRDHUP;
        if (!connection.output.empty()) flags |= EPOLLOUT;
        control(epoll.get(), EPOLL_CTL_MOD, connection.fd.get(), token, flags);
    }
    bool timer(Connection& connection, Clock::time_point now) {
        if (now - connection.progress >= options.idle_timeout || (connection.partial_since &&
            now - *connection.partial_since >= options.frame_timeout)) return false;
        for (auto i = connection.pending.begin(); i != connection.pending.end();) {
            if (now >= i->deadline || now >= i->due) {
                connection.buffered -= i->bytes.size();
                Bytes bytes = now >= i->deadline ? encode(error_frame(i->id, "request timeout")) : std::move(i->bytes);
                if (!enqueue(connection, std::move(bytes), i->id)) return false;
                i = connection.pending.erase(i);
            } else ++i;
        }
        return write(connection) && !(connection.eof && connection.inflight.empty());
    }
    void run() {
        std::array<epoll_event, 128> events{};
        auto next_tick = Clock::now();
        while (!stopped.load()) {
            auto count = epoll_wait(epoll.get(), events.data(), static_cast<int>(events.size()), 10);
            if (count < 0) {
                if (errno == EINTR) continue;
                os::system_error("epoll_wait");
            }
            for (int i = 0; i < count && !stopped.load(); ++i) {
                auto token = events[i].data.u64;
                auto flags = events[i].events;
                if (token == 0) { accept_ready(); continue; }
                if (token == 1) continue;
                auto found = connections.find(token);
                if (found == connections.end()) continue; // Tokens never alias a reused fd.
                auto& connection = found->second;
                bool ok = !(flags & EPOLLERR);
                if (ok && (flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))) ok = read(connection);
                if (ok) ok = write(connection);
                if (!ok || (connection.eof && connection.inflight.empty())) close(token);
                else interest(token, connection);
            }
            auto now = Clock::now();
            if (now >= next_tick) {
                std::vector<std::uint64_t> expired;
                for (auto& [token, connection] : connections) {
                    if (!timer(connection, now)) expired.push_back(token);
                    else interest(token, connection);
                }
                for (auto token : expired) close(token);
                next_tick = now + std::chrono::milliseconds(10);
            }
        }
        connections.clear();
    }
};
Server::Server(ServerOptions options, Handler handler) : impl_(std::make_unique<Impl>(options, std::move(handler))) {}
Server::~Server() = default;
std::uint16_t Server::port() const { return impl_->bound_port; }
void Server::run() { impl_->run(); }
void Server::stop() noexcept {
    impl_->stopped.store(true);
    std::uint64_t one = 1;
    while (::write(impl_->wake.get(), &one, sizeof(one)) < 0 && errno == EINTR) {}
}
std::size_t Server::peak_connections() const { return impl_->peak.load(); }
}

#include "os/rpc.hpp"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <algorithm>
#include <array>
#include <unordered_map>

namespace os::rpc {
Fd connect_loopback(std::uint16_t port, std::chrono::milliseconds timeout) {
    Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (fd.get() < 0) os::system_error("socket");
    int yes = 1;
    if (setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0)
        os::system_error("TCP_NODELAY");
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int result = connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result < 0 && errno != EINPROGRESS) os::system_error("connect");
    auto deadline = Clock::now() + timeout;
    while (result < 0) {
        auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) throw std::runtime_error("connection timeout");
        pollfd event{fd.get(), POLLOUT, 0};
        auto ready = poll(&event, 1, static_cast<int>(remaining));
        if (ready < 0) { if (errno == EINTR) continue; os::system_error("poll connect"); }
        if (ready == 0) continue;
        int error = 0; socklen_t size = sizeof(error);
        if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &size) < 0) os::system_error("SO_ERROR");
        if (error) throw std::system_error(error, std::generic_category(), "connect");
        break;
    }
    return fd;
}
namespace {
struct Client {
    Fd fd;
    Bytes input, output;
    std::size_t offset = 0, sent = 0, completed = 0;
    std::uint64_t next_id = 1;
    std::unordered_map<std::uint64_t, Clock::time_point> pending;
};
Bytes payload(std::size_t size, std::size_t connection, std::uint64_t id) {
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<std::uint8_t>((id >> ((i % 8) * 8)) ^ (connection + i * 31));
    return bytes;
}
void refill(Client& client, const LoadOptions& options, std::size_t index, std::size_t target) {
    if (client.offset) {
        client.output.erase(client.output.begin(), client.output.begin() + client.offset);
        client.offset = 0;
    }
    while (client.sent < target && client.pending.size() < options.pipeline) {
        auto id = client.next_id++;
        auto bytes = encode({Type::request, id, echo_request(payload(options.payload_size, index, id))});
        // Queue time includes any subsequent client-side send backpressure.
        client.pending.emplace(id, Clock::now());
        client.output.insert(client.output.end(), bytes.begin(), bytes.end());
        ++client.sent;
    }
}
void write(Client& client) {
    std::size_t budget = 65536;
    while (client.offset < client.output.size() && budget) {
        auto count = send(client.fd.get(), client.output.data() + client.offset,
            std::min(budget, client.output.size() - client.offset), MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            os::system_error("send request");
        }
        if (count == 0) throw std::runtime_error("zero-length send");
        auto size = static_cast<std::size_t>(count); client.offset += size; budget -= size;
    }
}
void read(Client& client, const LoadOptions& options, std::size_t index, std::vector<Sample>* samples) {
    std::array<std::uint8_t, 16384> buffer{};
    std::size_t budget = 65536;
    while (budget) {
        auto count = recv(client.fd.get(), buffer.data(), std::min(buffer.size(), budget), 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            os::system_error("recv response");
        }
        if (count == 0) throw std::runtime_error("server closed connection");
        budget -= static_cast<std::size_t>(count);
        client.input.insert(client.input.end(), buffer.begin(), buffer.begin() + count);
        std::size_t consumed = 0;
        while (auto parsed = decode(std::span(client.input).subspan(consumed))) {
            auto& frame = parsed->frame;
            auto pending = client.pending.find(frame.id);
            if (frame.type != Type::response || pending == client.pending.end() ||
                frame.body != payload(options.payload_size, index, frame.id))
                throw std::runtime_error("invalid response type, ID, or payload");
            auto now = Clock::now();
            if (now - pending->second >= options.timeout) throw std::runtime_error("late response");
            if (samples) samples->push_back({index, frame.id,
                std::chrono::duration<double, std::micro>(now - pending->second).count()});
            client.pending.erase(pending);
            ++client.completed;
            consumed += parsed->consumed;
        }
        client.input.erase(client.input.begin(), client.input.begin() + consumed);
        if (client.input.size() > header_size + max_payload) throw std::runtime_error("oversized response");
    }
}
}
LoadResult load(const LoadOptions& options) {
    if (!options.port || !options.connections || options.connections > 4096 ||
        !options.pipeline || options.pipeline > 256 || !options.requests_per_connection ||
        options.payload_size >= max_payload || options.timeout.count() <= 0 ||
        options.requests_per_connection > 10000000 / options.connections)
        throw std::invalid_argument("invalid load options");
    Fd epoll(epoll_create1(EPOLL_CLOEXEC));
    if (epoll.get() < 0) os::system_error("epoll_create1 client");
    std::vector<Client> clients(options.connections);
    for (std::size_t i = 0; i < clients.size(); ++i) {
        clients[i].fd = connect_loopback(options.port, options.timeout);
        epoll_event event{}; event.events = EPOLLIN; event.data.u64 = i;
        if (epoll_ctl(epoll.get(), EPOLL_CTL_ADD, clients[i].fd.get(), &event) < 0)
            os::system_error("epoll add client");
    }
    LoadResult result;
    result.samples.reserve(options.connections * options.requests_per_connection);
    auto phase = [&](std::size_t target, bool measure) {
        auto begin = Clock::now();
        auto next_timeout_check = begin;
        auto update = [&](std::size_t i) {
            auto& client = clients[i];
            epoll_event event{}; event.events = EPOLLIN;
            if (client.offset < client.output.size()) event.events |= EPOLLOUT;
            event.data.u64 = i;
            if (epoll_ctl(epoll.get(), EPOLL_CTL_MOD, client.fd.get(), &event) < 0)
                os::system_error("epoll modify client");
        };
        for (std::size_t i = 0; i < clients.size(); ++i) {
            clients[i].sent = 0; clients[i].completed = 0;
            refill(clients[i], options, i, target); write(clients[i]); update(i);
        }
        std::array<epoll_event, 256> events{};
        std::size_t finished = target == 0 ? clients.size() : 0;
        while (finished < clients.size()) {
            auto count = epoll_wait(epoll.get(), events.data(), static_cast<int>(events.size()), 10);
            if (count < 0) { if (errno == EINTR) continue; os::system_error("epoll wait client"); }
            for (int e = 0; e < count; ++e) {
                auto index = static_cast<std::size_t>(events[e].data.u64);
                auto& client = clients.at(index);
                bool was_done = client.completed == target;
                if (events[e].events & EPOLLERR) throw std::runtime_error("socket error during load");
                if (events[e].events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP))
                    read(client, options, index, measure ? &result.samples : nullptr);
                refill(client, options, index, target);
                write(client); update(index);
                if (!was_done && client.completed == target) ++finished;
            }
            auto now = Clock::now();
            if (now >= next_timeout_check) {
                for (const auto& client : clients) for (const auto& [id, started] : client.pending) {
                    (void)id;
                    if (now - started >= options.timeout) throw std::runtime_error("request timeout during load");
                }
                next_timeout_check = now + std::chrono::milliseconds(10);
            }
        }
        if (measure) result.elapsed_seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    };
    phase(options.warmup_per_connection, false);
    phase(options.requests_per_connection, true);
    return result;
}
}

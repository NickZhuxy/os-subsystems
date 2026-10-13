# RPC contract

The service binds only to IPv4 loopback. It has no authentication, encryption,
remote routing, or durable delivery. It is a local systems experiment.

## Wire format

All integers use unsigned network byte order. No native C++ structs are sent.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic `0x4f535250` (`OSRP`) |
| 4 | 2 | Version `1` |
| 6 | 2 | Type: request `1`, response `2`, error `3` |
| 8 | 8 | Nonzero request ID |
| 16 | 4 | Body length, at most 65,536 bytes |

A request body starts with a one-byte method. Method `1` echoes the remaining
bytes. Method `2` takes two unsigned 64-bit integers and returns their sum modulo
2^64. Method `3` takes a 64-bit delay in milliseconds followed by bytes to echo.
It schedules a reply without blocking the event loop. Error bodies contain text.

IDs match responses to requests on one connection. Replies can arrive out of
order. An ID remains in flight until its full response leaves the server's user
buffer. Reusing an in-flight ID closes the connection. Completed IDs can be reused.
An invalid header, truncated frame at EOF, or resource limit violation also closes
the connection. A valid request with a bad method or arguments returns an error.

## Reactor and limits

The server uses level-triggered epoll, nonblocking sockets, and a byte budget per
event. A partially sent response retains its offset. EPOLLOUT is enabled only
when output remains. Monotonic event tokens prevent descriptor reuse from making
a stale event refer to a new connection. An eventfd wakes the loop for shutdown.

Defaults: 4,096 connections, 256 in-flight requests per connection, and 4 MiB of
encoded response data per connection, including delayed replies. Read buffers
retain at most one incomplete frame after parsing a bounded read. The service
closes a connection when a limit would be exceeded; it does not silently drop a
request. This bounds memory but is not a graceful overload-control protocol.

The idle timeout is 30 seconds. Incomplete frames have a 2-second assembly
deadline, independent of trickled bytes. Delayed replies have a 2-second request
deadline. A 10 ms reactor sweep checks deadlines. The operating-system scheduler
can add delay. Handlers execute synchronously and must not block; the deadline
cannot interrupt arbitrary handler code. A write half-close preserves queued
replies, then closes the connection.

## Load measurements

The C++ load generator establishes all connections before warmup. It checks
every response ID and every payload byte. Each connection has a fixed maximum
number of outstanding requests. A completion queues the next request. The
measurement includes this client queueing time but excludes connection setup and
warmup. The client uses a monotonic clock and a 5-second deadline.

This is a closed-loop test. It does not measure arrivals during an unbounded
stall and must not be presented as an open-loop tail-latency guarantee. The echo
payload is 64 bytes; request framing and the method byte add 21 bytes. Both ends
use TCP_NODELAY. Both processes run in the same Docker container on loopback.

Run `make bench`. The runner saves raw latency samples as compressed CSV, JSON
summaries, all commands, source hashes, compiler packages, and environment data.
The first failure stops the runner and remains in the saved result. It is never
converted to a successful latency sample. Three repetitions cover 64, 256, and
1,024 connections with pipeline depths 1 and 4. Each repetition records 500
measured requests and 50 warmup requests per connection.

Reference: [Linux epoll manual](https://man7.org/linux/man-pages/man7/epoll.7.html).
The code is original; the manual defines the API semantics.

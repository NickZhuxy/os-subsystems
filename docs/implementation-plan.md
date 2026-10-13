# Implementation and evidence plan

This project implements user-space models and Linux services from scratch. No
course source is used. Algorithm descriptions and operating-system API manuals
are references, not implementation sources.

## Order and gates

1. M7: pinned Docker environment, CMake, CTest, independent sanitizer builds, CI
   definition. Record local test output and final runtime image size. CI status
   remains unverified until an approved push runs it.
2. M5: an epoll reactor, binary framing, multiplexed request IDs, bounded buffers,
   deadlines, protocol and socket tests, and a C++ load generator. Save raw
   per-request samples for 64, 256, and 1024 established connections.
3. M1: a discrete-event simulator with interchangeable FIFO, round-robin, MLFQ,
   and CFS policies. Test conservation of CPU time and scheduler invariants.
   Save deterministic workloads and per-process metrics.
4. M2, M3, M4, M6: demand paging, disk-image file system, synchronization,
   profiling case studies. A directory in the plan is not evidence of a feature.

## Measurement rules

- Report observations, not target numbers. Keep unsuccessful runs and explain
  exclusions. Performance runs use optimized binaries without sanitizers.
- Keep the command, source fingerprint, package versions, workload parameters,
  hardware, kernel, architecture, and container limits with each result.
- RPC latency begins when the client queues the complete request and ends when
  it has decoded and validated the complete response. Include client queueing.
  Count every timeout or protocol error. Distinguish closed-loop latency from
  open-loop overload behavior; do not generalize loopback results to networks.
- Scheduler time is simulated microseconds. Report it separately from wall
  time. Fairness must have a defined observation window and normalization.
- Image size means Docker's uncompressed image size in bytes. It is not registry
  transfer size. Compare binary hashes from two clean builds before claiming
  repeatable binary output; do not imply an image is bit-for-bit reproducible.

## Concept references

- [Linux epoll manual](https://man7.org/linux/man-pages/man7/epoll.7.html)
- [Historical CFS design](https://docs.kernel.org/scheduler/sched-design-CFS.html)
- [Clang ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html)

The CFS model will represent the historical vruntime design. It will not claim
to reproduce the current Linux scheduler or all kernel scheduling features.

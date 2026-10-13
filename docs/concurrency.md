# Synchronization and node lifetime

These are original implementations of established algorithms. They use Linux
futexes and C++ atomics; they do not use course code. The stack and queue allocate
fixed arenas at construction and perform no heap allocation during operations.
Their values are 64-bit integers. Destruction requires all operations and queue
participants to have finished.

## Futex mutex and semaphore

The mutex uses three states: unlocked, held, and held with possible waiters.
An uncontended acquire performs one compare-and-swap. A contended acquire marks
the state as contended and waits only while that value remains present. Unlock
publishes protected writes with release ordering and wakes one possible waiter.
The acquire operation pairs with that release. The kernel's atomic compare-and-wait
prevents the unlock/wait interval from losing a wakeup. See the
[Linux futex contract](https://www.man7.org/linux/man-pages/man2/futex.2.html).

The semaphore decrements a positive count with an acquire CAS, or waits for the
zero count to change. Release adds permits with release ordering and wakes up to
that many waiters. The count is bounded by INT_MAX and overflow is rejected.
Both primitives are process-private. The mutex is non-recursive. Neither primitive
promises fairness, cancellation, robust-owner recovery, or priority inheritance.
Releasing an unowned mutex or destroying a primitive with waiters is invalid use.

## Tagged Treiber stack

The stack has two intrusive lists: available nodes and published values. Each
head is one lock-free 64-bit atomic. Its low 16 bits form a relative pointer into
the fixed arena; the high 48 bits are a generation. Each successful head change
increments the generation. An A→B→A change therefore cannot satisfy an older CAS.
The program terminates instead of wrapping at the generation limit. This is
tagged-pointer protection using arena indices, not platform virtual-address bits.

A popped node belongs exclusively to the popper until its value is copied and
the node is returned to the available list. The intrusive link is atomic because
a failed concurrent pop can still inspect an old head's link after that node has
been reused. Release/acquire publication protects the non-atomic value. Arena
storage remains allocated until destruction, so a stale link read never accesses
freed memory. Tests combine a deterministic A→B→A stale-CAS rejection with
contention and independent ordering checks.

## Michael–Scott queue with hazard pointers

The queue follows the dummy-node and tail-helping structure of the
[Michael–Scott algorithm](https://ftp.cs.rochester.edu/research/synchronization/pseudocode/queues.html).
An enqueue linearizes when it links the new node. A dequeue linearizes when it
advances the head; the old dummy becomes retired. An empty result linearizes at
the validated observation of a head equal to the tail with no successor.

Each of up to 64 registered participants owns two hazard slots. Enqueue protects
the tail before reading its successor. Dequeue protects the head and successor,
then rechecks the head before reading the value. A successful dequeue retires
the previous dummy. Every 32 successful dequeues, or when allocation fails, a
thread detaches the shared retirement list and scans the hazard slots. It returns
only unprotected nodes to the free list. Protected nodes remain retired.
This applies [hazard-pointer reclamation](https://research.ibm.com/publications/hazard-pointers-safe-memory-reclamation-for-lock-free-objects)
to node reuse inside the arena; it does not return memory to the operating system.

Queue head, tail, successor links, and hazard publications use sequentially
consistent atomics. This conservative order makes publication, revalidation,
retirement, and scanning easier to audit. Retirement and free lists use tagged
heads. Their link fields are atomic because failed CAS attempts can observe an
old node. A hazard slot must not be shared by simultaneous operations. A moved
participant transfers ownership, and its destructor clears its hazards.

Both containers are bounded try-operation APIs. A push/enqueue can return false
when no arena slot is available, including when slots are owned by other active
operations or deferred by hazards. That does not necessarily mean the logical
container holds its maximum item count. CAS contention can delay an individual
thread; these operations are not wait-free. The implementation statically checks
that its atomic word sizes are lock-free on the target.

## Validation and benchmark scope

The unit workload checks 80,000 mutex-protected increments, 10,000 non-atomic
payload transfers synchronized through semaphores, and 80,000 exact-once items
across stack and queue producer/consumer tests. A 127-node arena forces reuse.
Small concurrent histories record invocation and response order; a backtracking
checker compares 80 twelve-operation histories with an independent deque model.
This checks sampled linearizability; it is not a proof over all executions.

The benchmark compares the custom mutex with `std::mutex`, and each nonblocking
container with a fixed-capacity container protected by the custom mutex. All
containers have 8,192 slots. Each worker performs 2,000 warmup pairs. A separate
100,000-pair-per-thread calibration estimates the iterations for a 0.15-second
measurement. Each case retains at least 100,000 iterations per thread and caps
the total at 32 million values. All three repetitions use that case's selected
iteration count. The first fixed-size experiment contained sub-millisecond runs;
the calibrated results are the reported comparison. Every returned value is saved and checked for exact-once
delivery outside the timed region. Thread creation and sorting are excluded;
barrier release and result storage are included. One push or pop is one operation.
The mutex-only workload counts one protected increment as one operation.
Cases use 1, 2, 4, and 8 threads, three repetitions, and a fixed shuffled order.
The shared Docker VM and four-CPU limit constrain what these timings establish.

## Bug evidence still required

The initial synchronization stress run passed ThreadSanitizer. No real data race
or deadlock has yet been reproduced in these primitives. Therefore the handover's
requirement for a race fix and a deadlock fix is still open. The actual FUSE EOF
undefined behavior and the Valgrind loader livelock are recorded separately;
neither is relabeled to satisfy that requirement. No deliberately broken example
is presented as a discovered project defect.

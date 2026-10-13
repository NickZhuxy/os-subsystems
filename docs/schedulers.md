# Discrete-event scheduler model

The simulator models one CPU. A process has an arrival time, a nice value, and
alternating CPU/I/O bursts. I/O does not consume the simulated CPU and can overlap
with other processes' work. Time is integer microseconds. There is no tick loop,
context-switch cost, multicore balancing, migration, or real kernel scheduling.

## Policy boundary

The event engine owns arrivals, I/O completion, burst accounting, ready waits,
and the observation horizon. Policies implement enqueue, selection, accounting,
execution budget, and preemption. The engine advances to the next arrival,
wakeup, burst end, budget boundary, priority boost, or horizon.

At equal timestamps, it resolves the previous CPU event first, then arrivals and
wakeups in process-index order, then the priority boost, then preemption. A
process that exhausts a quantum is queued before a simultaneous new arrival.
The horizon is an exclusive bound for arrivals; a CPU completion exactly on the
horizon still counts. Processes that have not arrived cannot be starvation cases.

## FIFO and round-robin

FIFO is nonpreemptive within each CPU burst. Round-robin uses a 2,000 us quantum.
Both policies use a FIFO ready queue. They ignore nice values when selecting work.

## MLFQ

The multilevel feedback queue has three FIFO queues with 1,000, 4,000, and 8,000 us
budgets. New processes start at the highest priority. Using the full budget
demotes the process by one level; the lowest level stays at that level. A higher
priority arrival or wakeup preempts lower priority work.

Unused budget survives both preemption and I/O. Thus repeated short bursts do not
reset the budget and prevent demotion. Exhausting a budget exactly at a burst
boundary also causes demotion. Every 20,000 us, all processes, including sleepers,
return to the top level with a new budget. Ready queues merge in priority order,
with FIFO order preserved within each queue. The running process joins the end
of the merged queue. Boosts reduce starvation; they do not guarantee a fixed
latency bound for arbitrary arrivals or an unbounded process count.

## CFS

This is an educational model of the historical Completely Fair Scheduler (CFS),
not a copy of Linux scheduling code. It uses an original intrusive red-black
tree, stored in a fixed node array and ordered by `(vruntime, process_index)`.
The run queue supports insertion and removal of its minimum. Tests check search
order, parent links, black-height equality, a black root, and no adjacent red
nodes after 20,000 randomized operations and ordered insertion/removal cases.

The weight is `1024 * 1.25^(-nice)` for nice values from -20 to 19. These analytic
weights approximate the nice scaling; they are not Linux's rounded weight table.
CPU service increases vruntime by `service * 1024 / weight`. A monotonic minimum
vruntime clamps new and waking processes so they do not receive old CPU credit.

Selection takes the least vruntime. The scheduling period is the larger of
8,000 us and `runnable_count * 500 us`. The dispatch budget is the process's
weighted share of that period, rounded up, with a 500 us minimum. A wakeup can
preempt a process that has received at least this minimum granularity when a
ready process has lower vruntime. Floating-point vruntime uses `long double`;
architecture-specific rounding can change ties. This is not a bit-exact Linux
model or an implementation of Linux's newer EEVDF policy.

## Metrics and workloads

- Response: first CPU start minus arrival. Its mean includes processes that
  started. Per-process files identify any process that did not start.
- Turnaround: completion minus arrival. Its mean includes completed processes
  only and is null if none completed.
- Ready wait: time in the ready queue; it excludes I/O. Maximum ready wait is the
  longest single ready interval, including one truncated by the horizon.
- Starvation count: processes with a ready interval of at least 50,000 us in the
  observed trace. This is a finite-window threshold, not proof of starvation.
- Jain's index: `(sum(x))^2 / (n * sum(x^2))`, where `x = CPU service / weight`
  for processes arriving before the observation end. Compare fairness using the
  always-runnable equal and weighted workloads. Different I/O demands make the
  mixed workload's index descriptive, not a fairness verdict.

The mixed workload has four CPU-bound processes with 80,000 us each and eight
interactive processes with twenty 500 us CPU bursts and 4,500 us I/O intervals.
The equal workload has eight always-runnable processes. The weighted workload
has four always-runnable processes with nice values -5, 0, 5, and 10. The latter
two use a 500,000 us observation window. Mixed runs continue to completion.

Run `make sched-bench`. Each case saves its full input, per-process results, and
CPU timeline. A separate Python calculation checks aggregate metrics against
those files. These are deterministic experiments, one run per policy/workload.
They measure scheduling decisions, not runtime speed or kernel performance.

Reference: [historical CFS design](https://docs.kernel.org/scheduler/sched-design-CFS.html).

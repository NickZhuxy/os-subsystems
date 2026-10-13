# Anonymous demand-paging model

The model has a 32-bit virtual address space, 4,096-byte pages, and a two-level
page table. Each level uses 10 address bits; the final 12 bits select a byte.
Second-level tables are allocated only when first used. The physical frame pool
has a fixed capacity. A virtual page acquires a frame only on an access fault.
New anonymous pages are zero-filled. This is a user-space model, not a kernel
page-fault handler or a mapping of the simulator's own process address space.

The translation lookaside buffer (TLB) is fully associative and uses least
recently used replacement. Capacity zero disables it. Frame eviction invalidates
the matching cached translation. Reads and writes through TLB hits still set
the page's reference state. A write marks the page dirty.

## Replacement policies

- FIFO removes the frame whose current page has been resident longest. Hits do
  not change its position.
- Clock advances a circular hand. A set reference bit grants a second chance and
  is cleared. The first clear bit selects the victim.
- Aging approximates LRU with eight-bit age counters. Before every eighth
  access, used frames shift right and incorporate their reference bit as the
  high bit, then clear the reference bit. The smallest age loses; equal ages use
  the lowest frame index. New pages start with age 128. The sampling period is
  configurable. This policy is not exact LRU.

Dirty eviction writes a whole page with `pwrite` to a sparse temporary backing
file at `virtual_page * page_size`. A later fault reads it with `pread`. Clean
pages with prior swap contents retain that backing slot. Clean zero pages need
no swap write. The temporary file is created with owner-only permissions and
unlinked immediately; the descriptor keeps it alive until model destruction.
This measures file-backed swap operations, not physical disk latency. The host
kernel's page cache can absorb the I/O.

## Correctness evidence

Tests check lazy table allocation, high virtual addresses, zero-fill, preservation
of multiple bytes across dirty eviction, clean-page re-eviction, stale TLB
invalidation, and reference-bit updates on TLB hits. Three policy runs each
execute 5,000 randomized accesses against a separate byte-addressed oracle.
Ownership validation cross-checks every present page and frame and each TLB entry.

The FIFO reference string `1 2 3 4 1 2 5 1 2 3 4 5` produces nine faults with
three frames and ten with four. This is Belady's anomaly: adding a frame can
increase FIFO faults for the same input.

## Reproduction

Run `make vm-bench` for synthetic comparisons. Each standard case has 65,536
accesses, a 16-entry TLB, seed 42, and an aging period of eight accesses. Test
frame capacities are 16, 64, and 128. The runner saves all commands, counters,
source hashes, and a compressed CSV for each trace. Every read is checked
against a separate byte-value oracle. The checksum must agree across policies.

- Sequential: advance through 256 pages, with 16 successive byte references
  per page, then repeat.
- Random: choose from 256 pages using `std::mt19937` and explicit modulo 256.
- Looping: cycle through 64 pages, with four successive references per page.
- Every seventh reference writes a deterministic byte. Other references read.

Run `make vm-lackey` to capture the included original memory-access program with
Valgrind Lackey and replay its data-page references. Startup and shutdown are
included. Instruction fetches are excluded. A reference spanning pages becomes
one touch per page, and a modify operation becomes a read followed by a write.
Original virtual page numbers map one-to-one to dense page IDs. Lackey supplies
addresses and operation sizes, not values; the replay uses synthetic write bytes.
The raw Lackey log and the converted input are saved. This is an address-trace
experiment, not a faithful replay of the original program's values or timing.

See the [Lackey manual](https://valgrind.org/docs/manual/lk-manual.html) for trace
limitations. The tool reports almost every access; it is not a hardware trace.

On this ARM64 Docker VM, two initial captures stalled in the dynamic loader's
LL/SC retry loop. GDB identified `__aarch64_ldadd4_relax` before `main`. The runner
now passes `--sim-hints=fallback-llsc` on ARM64, as documented in the
[Valgrind core manual](https://valgrind.org/docs/manual/manual-core.html).
The successful capture has 657,516 data references, converted to 657,610 page
touches across 321 distinct pages. The incomplete captures remain saved and
have no replay results. Capture output stays on the VM filesystem until it is
compressed, and a file-size limit prevents uncontrolled trace growth.

`vm_bench --trace-file file.csv` can replay a saved input. The header must be
`address,operation,value`. Addresses are decimal 32-bit values; operations are
`read` or `write`; read values are empty, and write values range from 0 to 255.
Fault and TLB rates are counters divided by modeled accesses. The reported
elapsed time includes the oracle and is not a standalone paging speed benchmark.

# Two measured component changes

The baseline source is commit `3424232`. Each run records a hash of every source
file, its command, compiler version, cache geometry, raw Cachegrind output,
function annotations, and validated workload output. `scripts/check_profiles.py`
recomputes all reported comparisons from those files. The results are in
`results/20261013-m6/`.

## Measurement boundary

These are Linux ARM64 Release builds in the shared Docker Desktop VM, limited
to four CPUs and 4 GiB RAM. Native measurements use seven repetitions and report
the median, with all samples retained. Setup and validation are outside the
timed region. Baseline and candidate were measured consecutively, not interleaved
or pinned to dedicated CPUs. Timings are observations in this environment.

Cachegrind instruments only the named workload region. Its configured model has
32 KiB, 8-way, 64-byte-line instruction and data L1 caches and an 8 MiB, 16-way,
64-byte-line last-level cache. This model is deliberately fixed across versions;
it does not describe the Apple M4's physical caches. “LL” is the modeled last
level, not a measured hardware L2. Data miss rates below divide read-plus-write
misses by read-plus-write data references. See the
[Cachegrind manual](https://valgrind.org/docs/manual/cg-manual.html).

On this ARM64 VM, Valgrind uses `--sim-hints=fallback-llsc`. Without it, the first
Lackey attempt livelocked in the loader's LL/SC atomic loop. The GDB stack and
failed captures are preserved in the `*-lackey/` result directories. This is a
tool/environment diagnosis, not a project deadlock fix. Hardware `perf stat`
attempts returned “No permission to enable cycles event”; the exact commands and
errors are saved. No hardware counter result is claimed.

## VM: separate frame metadata from page contents

Initially each frame object held 4,096 data bytes followed by page identity,
age, and reference flags. The aging policy's periodic scan accessed metadata
with a 4,104-byte stride. The change puts frame metadata in one compact vector
and page data in a separate vector. It leaves replacement policy decisions,
TLB behavior, swap I/O, and byte values unchanged.

The profile uses 512 resident pages, a 16-entry TLB, and an aging tick every
eight accesses. It reads pages in a deterministic permutation. Setup writes each
page before the measurement, and the checksum is checked afterward. Cachegrind
records 200,000 accesses; native runs each contain 1,000,000 accesses.

| Metric | Before | After |
| --- | ---: | ---: |
| Modeled L1 data misses | 3,774,197 | 225,206 |
| Modeled L1 data miss rate | 4.267040% | 0.254039% |
| Modeled LL data misses | 723 | 723 |
| Modeled LL data miss rate | 0.000817411% | 0.000815567% |
| Native median, milliseconds | 94.055709 | 61.119833 |
| Native range, milliseconds | 90.378292–101.327834 | 60.692833–62.313125 |

L1 data miss counts fell 94.0330%, and median time fell 35.0174%. LL miss counts
did not change. The small LL rate change comes from the denominator, not fewer
LL misses. These results support a specific aging-scan locality improvement;
they do not support a general “L1/L2 cache-miss rate cut by 55%” claim.

## Filesystem: read each bitmap block once per space query

The original `Image::space` called the one-bit accessor for every block and
inode. Each call read the containing 4 KiB bitmap block again. Cachegrind's
annotations showed repeated copying, while the library's I/O counter exposed
the redundant image reads. The change scans all relevant bits from each loaded
bitmap block before advancing to the next block. It keeps no persistent cache,
so mutations require no new cache-invalidation scheme.

The image has 8,192 blocks, 256 inodes, and 64 one-block files. Each query checks
the same expected free counts. Cachegrind records 20 queries; each native run
contains 100 queries. Tests also cover bitmap ranges that cross 32,768-bit
boundaries and partially occupied final bitmap blocks.

| Metric | Before | After |
| --- | ---: | ---: |
| Image block reads per query | 8,435 | 2 |
| Data references, 20 queries | 94,810,728 | 192,788 |
| Modeled L1 data misses | 81 | 80 |
| Modeled L1 data miss rate | 0.0000854334% | 0.0414964% |
| Modeled LL data misses | 81 | 80 |
| Modeled LL data miss rate | 0.0000854334% | 0.0414964% |
| Native median for 100 queries, milliseconds | 241.914542 | 0.250000 |
| Native range, milliseconds | 240.405000–260.434250 | 0.248875–0.261625 |

Data references fell 99.7967%, and median region time fell 99.8967%. The optimized
region is only about 0.25 ms, so small timing differences within that range are
not a robust performance distinction. Miss counts barely changed. The miss
*rate increased* because almost all redundant cache-hit accesses disappeared.
This case is a reduction in redundant I/O and memory copying, not evidence of a
better cache hit ratio or general filesystem throughput.

## Reproduce

Use a Linux Docker engine. Preserve the current checkout and create a separate
baseline checkout inside the Projects directory:

```sh
git worktree add --detach ../os-subsystems-profile-before 3424232
mkdir -p results/profile-repeat
docker run --rm --cpus=4 --memory=4g \
  -v "$PWD/../os-subsystems-profile-before:/checkout:ro" \
  -v "$PWD/results:/src/results" os-subsystems-dev:local \
  sh /checkout/scripts/container.sh python3 scripts/profile.py \
  --phase before --results results/profile-repeat
docker run --rm --cpus=4 --memory=4g \
  -v "$PWD:/checkout:ro" -v "$PWD/results:/src/results" os-subsystems-dev:local \
  sh /checkout/scripts/container.sh python3 scripts/profile.py \
  --phase after --results results/profile-repeat
python3 scripts/check_profiles.py --results results/profile-repeat
git worktree remove ../os-subsystems-profile-before
```

Run `make dev` first if the development image is absent. Each phase refuses to
overwrite an existing record. Cachegrind and native iteration counts differ,
but each comparison uses equal counts before and after. Native timing from a
Valgrind process is not used in the runtime table.

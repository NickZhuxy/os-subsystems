# Container evidence and size definitions

`make image` uses a compiler stage and a runtime stage. The runtime includes
Debian's base files, required C++ and FUSE runtime libraries, and eleven stripped project
programs. It runs as UID/GID 65534. The smoke test checks that compilers and build
tools are absent, exercises scheduling, paging, synchronization, filesystem tools
and profiling workloads, and exchanges RPC requests. The FUSE mount itself is
tested in the development container, with its device and mount capability.

`make repro` builds from two empty, differently named build directories with the
same pinned toolchain. It strips and hashes installed binaries. Both builds and
the runtime image have matching hashes. This verifies repeatable binaries in
this environment, not reproducible OCI metadata or cross-architecture identity.

## Current measurements

`results/m7-final/` records 104,949,248 uncompressed layer-tar bytes, 30,700,562
compressed layer-blob bytes, and 142 MB of Docker-reported disk use. All eleven
installed binaries have identical hashes in two clean builds and the runtime
image. The runtime smoke test passes as UID 65534.

## Size reporting trap found during the initial measurement

On Docker Engine 29.7.2 with the containerd image store, `docker image inspect`
reported 30,287,980 bytes, while `docker image ls` reported 140 MB. The smaller
value is not the uncompressed image footprint. Docker's release notes identify
an inspect-size bug that omitted unpacked snapshots. The containerd store retains
both compressed blobs and unpacked data.

Sources: [Docker Engine 29 release notes](https://docs.docker.com/engine/release-notes/29),
[containerd storage definitions](https://docs.docker.com/engine/storage/containerd/).

The host-side `scripts/image_evidence.py` exports the local image with `docker
image save`. It reads each layer from the manifest, decompresses gzip layers,
and counts the full uncompressed tar stream. It records every layer and both
Docker CLI observations. The initial checkpoint remains in `results/m7/image.json`;
new measurements default to `results/m7-final/image.json`.

The measured layer archives total 103,430,144 bytes. The compressed layer blobs
total 30,284,053 bytes. Neither is interchangeable with 140 MB of reported local
disk usage. The uncompressed archive count includes tar metadata and padding;
it does not include compressed copies or filesystem block-allocation overhead.
No daemon restart or upgrade was needed to make this distinction.

## Validation provenance

GitHub Actions run [38491239779](https://github.com/NickZhuxy/os-subsystems/actions/runs/38491239779)
tested commit `eedda709a53c7a6b926132e018b556f274c79470` on Ubuntu 24.04 x86-64
runners on 2026-10-14 UTC. All five jobs passed. The normal, ASan, UBSan, and TSan
jobs each passed eleven tests, including the mounted FUSE test, with no skips.
The runtime builder skipped its device-dependent FUSE test; the separate test
jobs supply that coverage. Its non-root runtime smoke test passed.

`results/ci/38491239779/` preserves the full job log, GitHub run metadata, a
test-count summary, and the image record extracted from the log. The x86-64
runtime measured 82,670,080 uncompressed layer-tar bytes and Docker reported
80.2 MB. This runner exported uncompressed layer blobs, so its `layer_blob_bytes`
is not a compressed-size measurement. These measurements are distinct from the
ARM64 image above. They do not establish cross-architecture binary identity.

To fetch the original remote evidence with an authenticated GitHub CLI:

```sh
gh run view 38491239779 --repo NickZhuxy/os-subsystems --json databaseId,headSha,headBranch,event,status,conclusion,url,createdAt,updatedAt,jobs
gh run view 38491239779 --repo NickZhuxy/os-subsystems --log
```

`results/validation/final-sanitizers.log` contains the current successful four-build
test run: eleven tests per configuration, 44 passes, no skips. `final-summary.json`
records source file hashes and scope. The ARM64 runtime builder also executes the
non-mounted tests before installing the programs; its FUSE mount test is skipped
because the image-build sandbox has no mount device. `make test` separately runs
the mounted test in every sanitizer configuration. Earlier checkpoint logs remain.

Earlier logs remain for transparency. `results/m7/bootstrap-tests.log` failed
because a source bind mount exposed the RPC directory while it was being written.
The container wrapper now copies a fixed source snapshot before configuring.
Initial package setup also required Debian's security snapshot and the separate
Clang sanitizer runtime package. The final image builds and sanitizer runs passed
after those fixes. None of these setup failures is reported as a performance run.

The first measurement checkpoint is local commit `6e7ea53`. Later changes replace
asynchronous signal handling in the server CLI with synchronous signal waiting,
add CLI shutdown tests and a buffer-limit test, and correct image-size reporting.
The RPC framing, reactor, and load-generator implementation used in the saved
performance runs is unchanged. Per-run source manifests preserve exact file hashes.

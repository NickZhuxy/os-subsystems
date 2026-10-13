# Container evidence and size definitions

`make image` uses a compiler stage and a runtime stage. The runtime includes
Debian's base files, required C++ runtime libraries, and four stripped project
programs. It runs as UID/GID 65534. The smoke test checks that compilers and build
tools are absent, runs a scheduler experiment, and exchanges RPC requests.

`make repro` builds from two empty, differently named build directories with the
same pinned toolchain. It strips and hashes installed binaries. Both builds and
the runtime image have matching hashes. This verifies repeatable binaries in
this environment, not reproducible OCI metadata or cross-architecture identity.

## Size reporting trap found during measurement

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
Docker CLI observations in `results/m7/image.json`.

The measured layer archives total 103,430,144 bytes. The compressed layer blobs
total 30,284,053 bytes. Neither is interchangeable with 140 MB of reported local
disk usage. The uncompressed archive count includes tar metadata and padding;
it does not include compressed copies or filesystem block-allocation overhead.
No daemon restart or upgrade was needed to make this distinction.

## Validation provenance

`results/validation/all-sanitizers.log` contains the successful final four-build
test run. `summary.json` records the source file hashes and scope. The ARM64
runtime build also executes the tests before installing the programs.

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

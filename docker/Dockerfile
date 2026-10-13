# syntax=docker/dockerfile:1
ARG BASE=debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
FROM ${BASE} AS base
ARG APT_SNAPSHOT=20261013T000000Z
RUN rm -f /etc/apt/sources.list.d/debian.sources && \
    printf 'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s/ trixie main\ndeb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/%s/ trixie-security main\n' "$APT_SNAPSHOT" "$APT_SNAPSHOT" > /etc/apt/sources.list

FROM base AS dev
RUN apt-get update && apt-get install -y --no-install-recommends \
    clang cmake ninja-build make python3 gdb valgrind linux-perf \
    libfuse3-dev fuse3 pkg-config ca-certificates && \
    dpkg-query -W > /toolchain-packages.txt && rm -rf /var/lib/apt/lists/*
RUN apt-get update && apt-get install -y --no-install-recommends libclang-rt-19-dev && \
    dpkg-query -W > /toolchain-packages.txt && rm -rf /var/lib/apt/lists/*
ENV CC=clang CXX=clang++ SOURCE_DATE_EPOCH=1791849600
WORKDIR /src
CMD ["bash"]

FROM dev AS builder
COPY . /src
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /build --parallel 4 && \
    ctest --test-dir /build --output-on-failure && \
    cmake --install /build --prefix /install --strip

FROM base AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 libfuse3-4 fuse3 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=builder /install/bin/ /usr/local/bin/
USER 65534:65534
CMD ["os_info"]

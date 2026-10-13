#!/bin/sh
set -eu
cd /src
if [ "$#" -gt 0 ]; then modes="$*"; else modes='none address undefined thread'; fi
for mode in $modes; do
    cmake -S . -B "/build/$mode" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSANITIZER="$mode"
    cmake --build "/build/$mode" --parallel "${JOBS:-4}"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    TSAN_OPTIONS=halt_on_error=1 \
    ctest --test-dir "/build/$mode" --output-on-failure --timeout 90
done

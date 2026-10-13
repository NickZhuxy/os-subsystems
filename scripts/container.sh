#!/bin/sh
set -eu
# Copy a fixed source snapshot. Concurrent host edits cannot change a running build.
cd /checkout
for path in CMakeLists.txt Makefile .dockerignore .github include src tests scripts docker rpc sched vm fs sync bench; do
    if [ -e "$path" ]; then cp -R "$path" /src/; fi
done
cd /src
exec "$@"

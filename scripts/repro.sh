#!/bin/sh
set -eu
mkdir -p /src/results/m7
for run in a b; do
    cmake -S /src -B "/build/repro-$run" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
    cmake --build "/build/repro-$run" --parallel 4
    cmake --install "/build/repro-$run" --prefix "/build/install-$run" --strip
    (cd "/build/install-$run" && find bin -type f -print0 | sort -z | xargs -0 sha256sum) > "/src/results/m7/binaries-$run.sha256"
done
diff -u /src/results/m7/binaries-a.sha256 /src/results/m7/binaries-b.sha256
echo 'PASS: two clean build directories produced identical installed binary hashes'

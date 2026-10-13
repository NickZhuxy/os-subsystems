#!/bin/sh
set -eu
test "$(id -u)" = 65534
for tool in clang clang++ gcc g++ cmake ninja; do
    if command -v "$tool" >/dev/null 2>&1; then echo "Unexpected build tool: $tool" >&2; exit 1; fi
done
os_info
sched_bench --workload weighted --policy cfs
vm_bench --trace belady --policy fifo --frames 3
sync_bench --kind hazard-queue --threads 2 --iterations 1000
hotspot_bench --component vm --iterations 1024
hotspot_bench --component fs --iterations 1
fs_mkfs --image /tmp/runtime-fs.img --blocks 128 --inodes 32
fs_cli --image /tmp/runtime-fs.img --op mkdir --path /data
fs_cli --image /tmp/runtime-fs.img --op create --path /data/file
fs_check --image /tmp/runtime-fs.img
rpc_server --port 9017 >/tmp/os-subsystems-port 2>/tmp/os-subsystems-server &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true; wait "$server_pid" || true' EXIT
attempt=0
while ! test -s /tmp/os-subsystems-port; do
    attempt=$((attempt + 1))
    if test "$attempt" -ge 50; then echo 'Server did not start' >&2; exit 1; fi
    sleep 0.1
done
rpc_bench --port 9017 --connections 8 --requests 10 --pipeline 4
kill "$server_pid"
wait "$server_pid"
trap - EXIT
cat /tmp/os-subsystems-server

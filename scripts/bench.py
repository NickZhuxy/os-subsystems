#!/usr/bin/env python3
"""Keep commands, environment, failures, summaries, and raw RPC latency samples."""
import argparse
import csv
import datetime as dt
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import selectors
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def source_manifest():
    paths = [ROOT / "CMakeLists.txt", ROOT / "Makefile", ROOT / ".dockerignore"]
    for directory in ("include", "src", "rpc", "sched", "vm", "fs", "sync", "bench", "tests", "scripts", "docker", ".github"):
        paths += sorted(p for p in (ROOT / directory).rglob("*") if p.is_file() and "__pycache__" not in p.parts)
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(paths)}


def output(command):
    return subprocess.check_output(command, text=True).strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--requests", type=int, default=500)
    parser.add_argument("--host-label", default=os.environ.get("HOST_LABEL", "not supplied"))
    args = parser.parse_args()
    if args.repeats < 1 or args.requests < 1:
        parser.error("repeats and requests must be positive")
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    destination = ROOT / "results" / (stamp + "-rpc")
    destination.mkdir(parents=True, exist_ok=False)
    manifest = source_manifest()
    metadata = {
        "utc": stamp, "host_label": args.host_label,
        "kernel": platform.release(), "architecture": platform.machine(),
        "logical_cpus": os.cpu_count(), "cpuinfo": Path("/proc/cpuinfo").read_text(),
        "memory": Path("/proc/meminfo").read_text(),
        "compiler": output(["clang++", "--version"]),
        "cmake": output(["cmake", "--version"]),
        "cgroup_cpu_max": Path("/sys/fs/cgroup/cpu.max").read_text(),
        "cgroup_memory_max": Path("/sys/fs/cgroup/memory.max").read_text(),
        "source_files_sha256": manifest,
        "source_sha256": hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest(),
        "sampling": "closed loop; latency includes client queueing; nearest-rank quantiles; no failed run is summarized as success",
        "environment_caveat": "Shared Docker Desktop VM; other workloads may run concurrently. No CPU pinning or exclusivity.",
    }
    (destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    shutil.copyfile("/toolchain-packages.txt", destination / "packages.txt")
    build = Path("/build/benchmark")
    with (destination / "build.log").open("w") as log:
        for command in (["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DSANITIZER=none"],
                        ["cmake", "--build", str(build), "--parallel", "4"]):
            log.write("COMMAND " + repr(command) + "\n"); log.flush()
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    summaries = []
    for repeat in range(1, args.repeats + 1):
        for pipeline in (1, 4):
            for connections in (64, 256, 1024):
                name = f"c{connections}-p{pipeline}-r{repeat}"
                samples = destination / (name + ".csv")
                server_command = [str(build / "rpc/rpc_server"), "--port", "0"]
                command = [str(build / "rpc/rpc_bench"), "--port", "0", "--connections", str(connections),
                           "--requests", str(args.requests), "--warmup", "50", "--pipeline", str(pipeline),
                           "--payload", "64", "--samples", str(samples)]
                record = {"name": name, "server_command": server_command, "status": "failed"}
                with (destination / (name + "-server.log")).open("w") as server_log:
                    server = subprocess.Popen(server_command, stdout=subprocess.PIPE, stderr=server_log, text=True)
                    try:
                        with selectors.DefaultSelector() as selector:
                            selector.register(server.stdout, selectors.EVENT_READ)
                            if not selector.select(15):
                                raise RuntimeError("server readiness timeout")
                        line = server.stdout.readline().strip()
                        server_log.write(line + "\n"); server_log.flush()
                        if not line.startswith("PORT "):
                            raise RuntimeError("server did not publish its port")
                        command[2] = line.split()[1]
                        record["command"] = command
                        run = subprocess.run(command, capture_output=True, text=True, timeout=180)
                        record.update(returncode=run.returncode, stderr=run.stderr)
                        (destination / (name + ".stdout")).write_text(run.stdout)
                        if run.returncode:
                            raise RuntimeError("load generator failed: " + run.stderr)
                        record.update(json.loads(run.stdout))
                        with samples.open("rb") as src, gzip.open(str(samples) + ".gz", "wb") as dst:
                            shutil.copyfileobj(src, dst)
                        samples.unlink()
                        record["samples"] = samples.name + ".gz"
                        record["status"] = "passed"
                    except Exception as error:
                        record["failure"] = str(error)
                        raise
                    finally:
                        server.terminate()
                        try:
                            server.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            server.kill(); server.wait()
                        record["server_returncode"] = server.returncode
                        if server.returncode != 0:
                            record["status"] = "failed"
                        summaries.append(record)
                        (destination / "rpc.json").write_text(json.dumps(summaries, indent=2) + "\n")
                if record["status"] != "passed":
                    raise RuntimeError("server exited with an error")
                print(name, "p50_us=", record["p50_us"], "p99_us=", record["p99_us"], flush=True)
    if source_manifest() != manifest:
        raise RuntimeError("source changed during measurement; discard comparisons")
    with (destination / "summary.csv").open("w", newline="") as stream:
        fields = ["name", "connections", "pipeline", "completed", "errors", "elapsed_seconds", "throughput_rps", "p50_us", "p95_us", "p99_us"]
        writer = csv.DictWriter(stream, fields, extrasaction="ignore"); writer.writeheader(); writer.writerows(summaries)
    print(destination, flush=True)


if __name__ == "__main__":
    main()

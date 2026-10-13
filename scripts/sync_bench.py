#!/usr/bin/env python3
"""Save contention results; validate all items rather than trusting timing alone."""
import datetime as dt
import json
import math
from pathlib import Path
import platform
import random
import subprocess
from bench import ROOT, source_manifest, output

stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
destination = ROOT / "results" / (stamp + "-sync")
destination.mkdir(parents=True, exist_ok=False)
manifest = source_manifest()
metadata = {"utc": stamp, "architecture": platform.machine(), "kernel": platform.release(),
            "compiler": output(["clang++", "--version"]), "source_files_sha256": manifest,
            "cgroup_cpu_max": Path("/sys/fs/cgroup/cpu.max").read_text(),
            "cgroup_memory_max": Path("/sys/fs/cgroup/memory.max").read_text(),
            "scope": "shared Docker Desktop VM; 4 CPU limit; no affinity or exclusivity; repeated mixed push/pop pairs",
            "operation": "one increment, or one push/enqueue or pop/dequeue; two operations per pair",
            "timing": "after warmup and thread creation; includes barrier release and per-thread result storage; excludes sorting validation",
            "order_seed": 61013, "calibration": "100000 iterations per thread; scale to target 0.15 s; keep at least 100000; cap total values at 32 million",
            "status": "running"}
(destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
build = Path("/build/sync-bench")
records = []
try:
    with (destination / "build.log").open("w") as log:
        for command in (["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                        ["cmake", "--build", str(build), "--target", "sync_bench", "--parallel", "4"]):
            log.write("COMMAND " + repr(command) + "\n"); log.flush()
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    cases = [(kind, threads, repeat) for kind in ("std-mutex", "futex-mutex", "mutex-stack", "tagged-stack", "mutex-queue", "hazard-queue")
             for threads in (1, 2, 4, 8) for repeat in (1, 2, 3)]
    random.Random(61013).shuffle(cases)
    calibrations = {}
    for kind, threads, repeat in cases:
        key = f"{kind}-{threads}"
        if key not in calibrations:
            probe_command = [str(build / "sync/sync_bench"), "--kind", kind, "--threads", str(threads), "--iterations", "100000"]
            probe = json.loads(subprocess.check_output(probe_command, text=True, timeout=90))
            iterations = min(32000000 // threads, max(100000, math.ceil(100000 * 0.15 / probe["seconds"])))
            calibrations[key] = {"command": probe_command, "probe": probe, "iterations": iterations}
            (destination / "calibration.json").write_text(json.dumps(calibrations, indent=2) + "\n")
        iterations = calibrations[key]["iterations"]
        command = [str(build / "sync/sync_bench"), "--kind", kind, "--threads", str(threads), "--iterations", str(iterations)]
        run = subprocess.run(command, capture_output=True, text=True, timeout=90)
        record = {"command": command, "repeat": repeat, "returncode": run.returncode, "stderr": run.stderr, "status": "failed"}
        if run.returncode == 0:
            record.update(json.loads(run.stdout))
            items = threads * iterations
            assert record["validated"] and record["seconds"] > 0
            assert record["operations"] == items * (1 if kind.endswith("mutex") else 2)
            assert record["checksum"] == (items if kind.endswith("mutex") else items * (items + 1) // 2)
            record["status"] = "passed"
        records.append(record)
        (destination / "sync.json").write_text(json.dumps(records, indent=2) + "\n")
        if run.returncode:
            raise RuntimeError(run.stderr)
        print(kind, threads, repeat, record["operations_per_second"], flush=True)
    assert source_manifest() == manifest, "source changed during benchmark"
    metadata["status"] = "passed"
finally:
    if metadata["status"] == "running":
        metadata["status"] = "failed"
    (destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(destination, flush=True)

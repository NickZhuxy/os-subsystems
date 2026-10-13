#!/usr/bin/env python3
"""Measure named regions in real components before and after a source change."""
import argparse
import datetime as dt
import json
from pathlib import Path
import platform
import subprocess
from bench import ROOT, source_manifest, output

parser = argparse.ArgumentParser()
parser.add_argument("--phase", choices=("before", "after"), required=True)
parser.add_argument("--results", type=Path, required=True)
args = parser.parse_args()
destination = args.results.resolve()
destination.mkdir(parents=True, exist_ok=True)
record_path = destination / (args.phase + ".json")
if record_path.exists():
    raise RuntimeError("refusing to overwrite a recorded phase")
manifest = source_manifest()
metadata = {"utc": dt.datetime.now(dt.timezone.utc).isoformat(), "phase": args.phase,
            "architecture": platform.machine(), "kernel": platform.release(),
            "compiler": output(["clang++", "--version"]), "valgrind": output(["valgrind", "--version"]),
            "source_files_sha256": manifest, "status": "running", "cases": [],
            "cgroup_cpu_max": Path("/sys/fs/cgroup/cpu.max").read_text(),
            "cgroup_memory_max": Path("/sys/fs/cgroup/memory.max").read_text(),
            "cache_model": {"I1": "32768,8,64", "D1": "32768,8,64", "LL": "8388608,16,64"},
            "scope": "modeled caches, not host hardware L1/L2; instruction region excludes setup; shared Docker VM; native timing excludes setup and validation"}
record_path.write_text(json.dumps(metadata, indent=2) + "\n")
build = Path("/build/profile-" + args.phase)
try:
    with (destination / (args.phase + "-build.log")).open("w") as log:
        for command in (["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                        ["cmake", "--build", str(build), "--target", "hotspot_bench", "--parallel", "4"]):
            log.write("COMMAND " + repr(command) + "\n"); log.flush()
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    executable = str(build / "bench/hotspot_bench")
    for component, cg_iterations, native_iterations in (("vm", 200000, 1000000), ("fs", 20, 100)):
        base = destination / (args.phase + "-" + component)
        native = []
        native_command = [executable, "--component", component, "--iterations", str(native_iterations)]
        for repeat in range(7):
            run = subprocess.run(native_command, capture_output=True, text=True, check=True, timeout=90)
            native.append({"repeat": repeat + 1, **json.loads(run.stdout)})
        cache_path = Path(str(base) + ".cachegrind")
        command = ["valgrind", "--tool=cachegrind", "--cache-sim=yes", "--instr-at-start=no",
                   "--I1=32768,8,64", "--D1=32768,8,64", "--LL=8388608,16,64", "--cachegrind-out-file=" + str(cache_path)]
        if platform.machine() == "aarch64":
            command.append("--sim-hints=fallback-llsc")
        command += [executable, "--component", component, "--iterations", str(cg_iterations)]
        run = subprocess.run(command, capture_output=True, text=True, timeout=180)
        Path(str(base) + "-valgrind.log").write_text(run.stderr)
        if run.returncode:
            raise RuntimeError(run.stderr)
        workload = json.loads(run.stdout)
        fields, counts = None, None
        for line in cache_path.read_text().splitlines():
            if line.startswith("events:"):
                fields = line.split()[1:]
            elif line.startswith("summary:"):
                counts = list(map(int, line.split()[1:]))
        assert fields and counts and len(fields) == len(counts)
        counters = dict(zip(fields, counts))
        references = counters["Dr"] + counters["Dw"]
        l1_misses = counters["D1mr"] + counters["D1mw"]
        ll_misses = counters["DLmr"] + counters["DLmw"]
        annotation = subprocess.check_output(["cg_annotate", "--auto=no", "--show=Dr,D1mr,Dw,D1mw,DLmr,DLmw", str(cache_path)], text=True)
        Path(str(base) + "-annotation.txt").write_text(annotation)
        perf_command = ["perf", "stat", "-x", ",", "-e", "cycles,instructions,L1-dcache-loads,L1-dcache-load-misses,cache-references,cache-misses", "--", *native_command]
        perf = subprocess.run(perf_command, capture_output=True, text=True, timeout=90)
        Path(str(base) + "-perf.log").write_text(perf.stderr)
        record = {"component": component, "native_command": native_command, "native_runs": native,
                  "cachegrind_command": command, "cachegrind_workload": workload, "counters": counters,
                  "data_references": references, "D1_data_misses": l1_misses, "LL_data_misses": ll_misses,
                  "D1_data_miss_rate": l1_misses / references, "LL_data_miss_rate": ll_misses / references,
                  "perf_command": perf_command, "perf_returncode": perf.returncode, "perf_stdout": perf.stdout,
                  "perf_status": "completed; inspect event availability" if perf.returncode == 0 else "unavailable"}
        metadata["cases"].append(record)
        record_path.write_text(json.dumps(metadata, indent=2) + "\n")
        print(args.phase, component, "D1 misses", l1_misses, "LL data misses", ll_misses, flush=True)
    assert source_manifest() == manifest, "source changed during measurement"
    metadata["status"] = "passed"
finally:
    if metadata["status"] == "running":
        metadata["status"] = "failed"
    record_path.write_text(json.dumps(metadata, indent=2) + "\n")

#!/usr/bin/env python3
"""Save paging-policy comparisons, byte-oracle checks, and the source traces."""
import datetime as dt
import gzip
import json
from pathlib import Path
import platform
import shutil
import subprocess
from bench import ROOT, source_manifest, output

stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
destination = ROOT / "results" / (stamp + "-vm")
destination.mkdir(parents=True, exist_ok=False)
manifest = source_manifest()
metadata = {"utc": stamp, "kernel": platform.release(), "architecture": platform.machine(),
            "compiler": output(["clang++", "--version"]), "source_files_sha256": manifest,
            "measurement": "deterministic policy counters; one run per case; runtime includes independent byte oracle",
            "trace_generator": "std::mt19937 seed 42; explicit modulo selection; first source CSV per trace saved"}
(destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
build = Path("/build/vm")
with (destination / "build.log").open("w") as log:
    for command in (["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                    ["cmake", "--build", str(build), "--parallel", "4"]):
        log.write("COMMAND " + repr(command) + "\n"); log.flush()
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
records = []
saved_traces = set()
cases = [(trace, policy, frames) for trace in ("sequential", "random", "looping")
         for frames in (16, 64, 128) for policy in ("fifo", "clock", "aging")]
cases += [("belady", "fifo", frames) for frames in (3, 4)]
checksums = {}
for trace, policy, frames in cases:
    command = [str(build / "vm/vm_bench"), "--trace", trace, "--policy", policy, "--frames", str(frames),
               "--accesses", "65536", "--seed", "42", "--tlb", "16", "--aging-period", "8"]
    path = destination / (trace + "-input.csv")
    if trace not in saved_traces:
        command += ["--dump-trace", str(path)]
    run = subprocess.run(command, capture_output=True, text=True, timeout=120)
    record = {"command": command, "status": "failed", "returncode": run.returncode, "stderr": run.stderr}
    if run.returncode == 0:
        record.update(json.loads(run.stdout)); record["status"] = "passed"
        assert record["checksum"] == checksums.setdefault(trace, record["checksum"])
        assert record["page_faults"] >= record["first_touches"]
        assert record["resident_pages"] <= frames
        assert record["page_faults"] - record["resident_pages"] == record["evictions"]
        assert record["swap_bytes_written"] == record["swap_writes"] * 4096
        if trace == "belady":
            assert record["page_faults"] == (9 if frames == 3 else 10)
    records.append(record)
    (destination / "vm.json").write_text(json.dumps(records, indent=2) + "\n")
    if run.returncode:
        raise RuntimeError(run.stderr)
    if trace not in saved_traces:
        with path.open("rb") as src, gzip.open(str(path) + ".gz", "wb") as dst:
            shutil.copyfileobj(src, dst)
        path.unlink(); saved_traces.add(trace)
    print(trace, policy, frames, "faults=", record["page_faults"], "TLB hits=", record["tlb_hits"], flush=True)
if source_manifest() != manifest:
    raise RuntimeError("source changed during measurement")
print(destination, flush=True)

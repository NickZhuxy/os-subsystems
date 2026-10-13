#!/usr/bin/env python3
"""Capture an original program's address trace and replay its page touches."""
import csv
import datetime as dt
import gzip
import json
from pathlib import Path
import platform
import re
import resource
import shutil
import subprocess
from bench import ROOT, source_manifest, output

stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
destination = ROOT / "results" / (stamp + "-lackey")
destination.mkdir(parents=True, exist_ok=False)
build = Path("/build/lackey")
metadata = {"utc": stamp, "architecture": platform.machine(), "kernel": platform.release(),
            "source_files_sha256": source_manifest(), "valgrind": output(["valgrind", "--version"]),
            "conversion": "Data references only; dense remapping of virtual page numbers; split cross-page operations; M becomes read then write; writes use synthetic values. Includes program startup and shutdown."}
(destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
with (destination / "build.log").open("w") as log:
    for command in (["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF"],
                    ["cmake", "--build", str(build), "--target", "vm_bench", "vm_trace_workload", "--parallel", "4"]):
        log.write("COMMAND " + repr(command) + "\n"); log.flush()
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
# High-volume trace writes stay on the VM filesystem, not the macOS bind mount.
raw = build / "lackey.log"
command = ["valgrind", "--tool=lackey", "--trace-mem=yes", "--basic-counts=no", "--error-exitcode=99",
           "--log-file=" + str(raw), str(build / "vm/vm_trace_workload")]
if platform.machine() == "aarch64":
    command.insert(1, "--sim-hints=fallback-llsc")
def trace_limit():
    resource.setrlimit(resource.RLIMIT_FSIZE, (128 * 1024 * 1024, 128 * 1024 * 1024))
metadata.update(command=command, capture_status="running")
(destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
try:
    run = subprocess.run(command, text=True, capture_output=True, timeout=120, preexec_fn=trace_limit)
except subprocess.TimeoutExpired:
    metadata["capture_status"] = "timed_out"
    (destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    if raw.exists():
        with raw.open("rb") as src, gzip.open(destination / "lackey.log.gz", "wb") as dst:
            shutil.copyfileobj(src, dst)
    raise
metadata.update(command=command, returncode=run.returncode, stderr=run.stderr)
metadata["capture_status"] = "passed" if run.returncode == 0 else "failed"
(destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
if run.returncode:
    if raw.exists():
        with raw.open("rb") as src, gzip.open(destination / "lackey.log.gz", "wb") as dst:
            shutil.copyfileobj(src, dst)
    raise RuntimeError("Lackey capture failed; log retained")
pattern = re.compile(r"^\s*([LSM])\s+([0-9a-fA-F]+),(\d+)\s*$")
mapping = {}
references = 0
touches = 0
trace = build / "page-touches.csv"
with raw.open() as stream, trace.open("w", newline="") as out:
    writer = csv.writer(out, lineterminator="\n"); writer.writerow(["address", "operation", "value"])
    for line in stream:
        match = pattern.match(line)
        if not match:
            continue
        operation, address, size = match[1], int(match[2], 16), int(match[3])
        if not 0 < size <= 1024 * 1024:
            raise RuntimeError("unexpected Lackey access size")
        references += 1
        end = address + size
        while address < end:
            page = address // 4096
            dense = mapping.setdefault(page, len(mapping))
            if dense >= 1024 * 1024:
                raise RuntimeError("trace exceeds the modeled address space")
            virtual = dense * 4096 + address % 4096
            if operation in ("L", "M"):
                writer.writerow([virtual, "read", ""]); touches += 1
            if operation in ("S", "M"):
                writer.writerow([virtual, "write", references * 17 % 256]); touches += 1
            address = min(end, (page + 1) * 4096)
if references == 0 or touches > 10000000:
    raise RuntimeError("empty or excessive captured trace")
metadata.update(data_references=references, modeled_page_touches=touches, distinct_pages=len(mapping),
                original_page_to_dense_page={hex(page): dense for page, dense in mapping.items()})
(destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
records = []
checksum = None
for frames in (16, 64, 128):
    for policy in ("fifo", "clock", "aging"):
        command = [str(build / "vm/vm_bench"), "--trace-file", str(trace), "--policy", policy, "--frames", str(frames)]
        run = subprocess.run(command, capture_output=True, text=True, timeout=120, check=True)
        result = json.loads(run.stdout)
        if checksum is None:
            checksum = result["checksum"]
        assert result["checksum"] == checksum and result["accesses"] == touches
        result.update(command=command, status="passed")
        records.append(result)
        (destination / "vm.json").write_text(json.dumps(records, indent=2) + "\n")
        print(frames, policy, "faults=", result["page_faults"], "TLB hits=", result["tlb_hits"], flush=True)
for path in (raw, trace):
    with path.open("rb") as src, gzip.open(destination / (path.name + ".gz"), "wb") as dst:
        shutil.copyfileobj(src, dst)
    path.unlink()
print(destination, flush=True)

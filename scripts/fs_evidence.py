#!/usr/bin/env python3
"""Save real CLI, crash, and mounted-FUSE results with source provenance."""
import datetime as dt
import json
from pathlib import Path
import platform
import subprocess
from bench import ROOT, source_manifest, output

stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
destination = ROOT / "results" / (stamp + "-fs")
destination.mkdir(parents=True, exist_ok=False)
manifest = source_manifest()
metadata = {"utc": stamp, "architecture": platform.machine(), "kernel": platform.release(),
            "compiler": output(["clang++", "--version"]), "fuse_version": output(["pkg-config", "--modversion", "fuse3"]),
            "source_files_sha256": manifest, "cgroup_cpu_max": Path("/sys/fs/cgroup/cpu.max").read_text(),
            "cgroup_memory_max": Path("/sys/fs/cgroup/memory.max").read_text(),
            "scope": "functional checks and controlled process death; not a throughput benchmark or power-loss simulation",
            "status": "running"}
(destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
build = Path("/build/fs-evidence")
try:
    with (destination / "build-and-test.log").open("w") as log:
        commands = [
            ["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
            ["cmake", "--build", str(build), "--target", "fs_test", "fs_mkfs", "fs_cli", "fs_check", "fs_mount", "--parallel", "4"],
            [str(build / "fs/fs_test")],
            ["python3", "tests/fs_process_test.py", "--build", str(build / "fs"), "--evidence", str(destination)],
            ["python3", "tests/fs_fuse_test.py", "--build", str(build / "fs"), "--evidence", str(destination)],
        ]
        for command in commands:
            log.write("COMMAND " + repr(command) + "\n"); log.flush()
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=150)
    assert source_manifest() == manifest, "source changed during measurement"
    metadata["status"] = "passed"
finally:
    if metadata["status"] == "running":
        metadata["status"] = "failed"
    (destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(destination, flush=True)

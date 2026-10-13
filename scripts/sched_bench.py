#!/usr/bin/env python3
"""Save deterministic scheduler inputs, timelines, and independently checked metrics."""
import csv
import datetime as dt
import hashlib
import json
import math
import platform
import subprocess
from pathlib import Path
from bench import ROOT, source_manifest, output


def verify(prefix, result):
    with Path(str(prefix) + "-processes.csv").open() as stream:
        rows = list(csv.DictReader(stream))
    with Path(str(prefix) + "-trace.csv").open() as stream:
        trace = list(csv.DictReader(stream))
    service = [0] * len(rows)
    last_end = 0
    for segment in trace:
        begin, end, process = (int(segment[k]) for k in ("begin_us", "end_us", "process"))
        assert last_end <= begin < end <= result["elapsed_us"]
        service[process] += end - begin
        last_end = end
    assert service == [int(row["service_us"]) for row in rows]
    assert sum(service) == result["busy_us"]
    assert result["busy_us"] + result["idle_us"] == result["elapsed_us"]
    responses = [int(row["first_run_us"]) - int(row["arrival_us"]) for row in rows if row["first_run_us"]]
    turnarounds = [int(row["completion_us"]) - int(row["arrival_us"]) for row in rows if row["completion_us"]]
    assert len(turnarounds) == result["completed"]
    assert math.isclose(sum(responses) / len(responses), result["mean_response_us"], abs_tol=1e-6)
    if turnarounds:
        assert math.isclose(sum(turnarounds) / len(turnarounds), result["mean_turnaround_us"], abs_tol=1e-6)
    normalized = [int(row["service_us"]) / float(row["weight"]) for row in rows if int(row["arrival_us"]) < result["elapsed_us"]]
    jain = sum(normalized) ** 2 / (len(normalized) * sum(x*x for x in normalized))
    assert math.isclose(jain, result["jain_weighted_service"], abs_tol=1e-9)
    assert sum(int(row["max_ready_wait_us"]) >= result["starvation_threshold_us"] for row in rows) == result["starved_processes"]


def main():
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    destination = ROOT / "results" / (stamp + "-sched")
    destination.mkdir(parents=True, exist_ok=False)
    manifest = source_manifest()
    metadata = {"utc": stamp, "architecture": platform.machine(), "kernel": platform.release(),
                "compiler": output(["clang++", "--version"]), "source_files_sha256": manifest,
                "source_sha256": hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest(),
                "measurement": "deterministic simulation in microseconds; one execution per policy/workload; not a hardware speed benchmark",
                "configuration": {"rr_quantum_us": 2000, "mlfq_quantums_us": [1000,4000,8000],
                                  "boost_period_us": 20000, "target_latency_us": 8000,
                                  "min_granularity_us": 500, "starvation_threshold_us": 50000}}
    (destination / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    build = Path("/build/scheduler")
    with (destination / "build.log").open("w") as log:
        for command in (["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                        ["cmake", "--build", str(build), "--parallel", "4"]):
            log.write("COMMAND " + repr(command) + "\n"); log.flush()
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    records = []
    for workload in ("mixed", "equal", "weighted"):
        for policy in ("fifo", "rr", "mlfq", "cfs"):
            prefix = destination / (workload + "-" + policy)
            command = [str(build / "sched/sched_bench"), "--workload", workload, "--policy", policy, "--output", str(prefix)]
            run = subprocess.run(command, capture_output=True, text=True, check=True)
            result = json.loads(run.stdout)
            verify(prefix, result)
            result.update(command=command, independent_check="passed")
            records.append(result)
            (destination / "scheduler.json").write_text(json.dumps(records, indent=2) + "\n")
            print(workload, policy, "response_us=", result["mean_response_us"], "Jain=", result["jain_weighted_service"], flush=True)
    if source_manifest() != manifest:
        raise RuntimeError("source changed during measurement")
    print(destination, flush=True)


if __name__ == "__main__":
    main()

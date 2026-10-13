#!/usr/bin/env python3
"""Recompute published RPC quantiles and scheduler metrics from saved raw files."""
import csv
import gzip
import json
import math
from pathlib import Path
from bench import ROOT
from sched_bench import verify

total = 0
for summary in sorted((ROOT / "results").glob("*-rpc/rpc.json")):
    for run in json.loads(summary.read_text()):
        assert run["status"] == "passed" and run["errors"] == 0
        assert run["returncode"] == run["server_returncode"] == 0
        assert f"peak_connections={run['connections']}" in (summary.parent / (run["name"] + "-server.log")).read_text()
        latencies = []
        seen = [set() for _ in range(run["connections"])]
        with gzip.open(summary.parent / run["samples"], "rt") as stream:
            for row in csv.DictReader(stream):
                connection, identifier = int(row["connection"]), int(row["request_id"])
                latency = float(row["latency_us"])
                assert 0 <= connection < run["connections"]
                assert identifier not in seen[connection]
                seen[connection].add(identifier)
                assert math.isfinite(latency) and latency >= 0
                latencies.append(latency)
        expected = set(range(run["warmup_per_connection"] + 1,
                             run["warmup_per_connection"] + run["requests_per_connection"] + 1))
        assert all(ids == expected for ids in seen)
        assert len(latencies) == run["completed"] == run["connections"] * run["requests_per_connection"]
        latencies.sort()
        for field, fraction in (("p50_us", .5), ("p95_us", .95), ("p99_us", .99)):
            value = latencies[math.ceil(fraction * len(latencies)) - 1]
            assert math.isclose(value, run[field], abs_tol=1e-6)
        assert math.isclose(len(latencies) / run["elapsed_seconds"], run["throughput_rps"], rel_tol=1e-9)
        total += len(latencies)
    print("PASS RPC raw samples:", summary.parent.name, flush=True)

cases = 0
for summary in sorted((ROOT / "results").glob("*-sched/scheduler.json")):
    for result in json.loads(summary.read_text()):
        verify(summary.parent / (result["workload"] + "-" + result["policy"]), result)
        cases += 1
    print("PASS scheduler traces:", summary.parent.name, flush=True)

print(f"PASS {total} RPC latency samples; {cases} scheduler cases")

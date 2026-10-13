#!/usr/bin/env python3
"""Recompute comparison metrics from the saved raw Cachegrind files and runs."""
import argparse
import json
from pathlib import Path
import statistics

parser = argparse.ArgumentParser()
parser.add_argument("--results", type=Path, default=Path("results/20261013-m6"))
args = parser.parse_args()
phases = {phase: json.loads((args.results / (phase + ".json")).read_text()) for phase in ("before", "after")}
comparisons = []
for phase, result in phases.items():
    assert result["status"] == "passed"
    assert len(result["cases"]) == 2
    for case in result["cases"]:
        text = (args.results / f"{phase}-{case['component']}.cachegrind").read_text().splitlines()
        events = next(line.split()[1:] for line in text if line.startswith("events:"))
        totals = next(map(int, line.split()[1:]) for line in text if line.startswith("summary:"))
        counters = dict(zip(events, totals))
        assert counters == case["counters"]
        refs = counters["Dr"] + counters["Dw"]
        assert case["data_references"] == refs
        for label, read, write in (("D1", "D1mr", "D1mw"), ("LL", "DLmr", "DLmw")):
            misses = counters[read] + counters[write]
            assert case[label + "_data_misses"] == misses
            assert case[label + "_data_miss_rate"] == misses / refs
for component in ("vm", "fs"):
    pair = [next(case for case in phases[phase]["cases"] if case["component"] == component) for phase in ("before", "after")]
    before, after = pair
    assert phases["before"]["cache_model"] == phases["after"]["cache_model"]
    assert before["cachegrind_workload"]["checksum"] == after["cachegrind_workload"]["checksum"]
    assert before["cachegrind_workload"]["iterations"] == after["cachegrind_workload"]["iterations"]
    times = []
    for case in pair:
        runs = case["native_runs"]
        assert len(runs) == 7 and all(run["validated"] for run in runs)
        assert len({run["checksum"] for run in runs}) == 1
        times.append({"minimum": min(run["seconds"] for run in runs), "median": statistics.median(run["seconds"] for run in runs),
                      "maximum": max(run["seconds"] for run in runs)})
    assert before["native_runs"][0]["checksum"] == after["native_runs"][0]["checksum"]
    record = {"component": component, "cachegrind_iterations": before["cachegrind_workload"]["iterations"],
              "native_iterations": before["native_runs"][0]["iterations"], "native_repeats": 7,
              "native_before_seconds": times[0], "native_after_seconds": times[1],
              "native_median_reduction_percent": 100 * (1 - times[1]["median"] / times[0]["median"])}
    for metric in ("data_references", "D1_data_misses", "LL_data_misses", "D1_data_miss_rate", "LL_data_miss_rate"):
        record[metric] = {"before": before[metric], "after": after[metric], "reduction_percent": 100 * (1 - after[metric] / before[metric])}
    record["image_block_reads"] = {phase: case["cachegrind_workload"]["block_reads"] for phase, case in zip(("before", "after"), pair)}
    comparisons.append(record)
(args.results / "comparison.json").write_text(json.dumps(comparisons, indent=2) + "\n")
print(json.dumps(comparisons, indent=2))

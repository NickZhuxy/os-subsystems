#!/usr/bin/env python3
"""A separate, dictionary-based reference model audits the saved paging counts."""
from collections import OrderedDict, deque
import csv
import gzip
import json
import math
from pathlib import Path
from bench import ROOT


def check(path, run):
    resident = {}
    frames = []
    tlb = OrderedDict()
    fifo = deque()
    touched, swapped, dirty, tables = set(), set(), set(), set()
    values = {}
    hand = 0
    stats = dict(accesses=0, page_faults=0, tlb_hits=0, evictions=0, swap_reads=0, swap_writes=0, checksum=0)
    with gzip.open(path, "rt") as stream:
        for row in csv.DictReader(stream):
            address = int(row["address"]); page = address // 4096
            stats["accesses"] += 1
            if run["policy"] == "aging" and stats["accesses"] % run["aging_period"] == 0:
                for frame in frames:
                    frame["age"] = frame["age"] // 2 + 128 * frame["referenced"]
                    frame["referenced"] = False
            if page in tlb:
                stats["tlb_hits"] += 1
                tlb.move_to_end(page)
                index = resident[page]
            else:
                tables.add(page // 1024)
                if page not in resident:
                    stats["page_faults"] += 1; touched.add(page)
                    if len(frames) < run["frames"]:
                        index = len(frames); frames.append({})
                    else:
                        if run["policy"] == "fifo":
                            index = fifo.popleft()
                        elif run["policy"] == "clock":
                            while frames[hand]["referenced"]:
                                frames[hand]["referenced"] = False; hand = (hand + 1) % len(frames)
                            index = hand; hand = (hand + 1) % len(frames)
                        else:
                            index = min(range(len(frames)), key=lambda i: (frames[i]["age"], i))
                        old = frames[index]["page"]
                        if old in dirty:
                            dirty.remove(old); swapped.add(old); stats["swap_writes"] += 1
                        del resident[old]; tlb.pop(old, None); stats["evictions"] += 1
                    if page in swapped:
                        stats["swap_reads"] += 1
                    resident[page] = index
                    frames[index] = dict(page=page, referenced=True, age=128)
                    if run["policy"] == "fifo":
                        fifo.append(index)
                else:
                    index = resident[page]
                if run["tlb_entries"]:
                    if len(tlb) == run["tlb_entries"]:
                        tlb.popitem(last=False)
                    tlb[page] = index
            frames[index]["referenced"] = True
            if row["operation"] == "write":
                values[address] = int(row["value"]); dirty.add(page)
            else:
                stats["checksum"] += values.get(address, 0)
    stats.update(first_touches=len(touched), resident_pages=len(resident), page_tables=len(tables),
                 swap_bytes_written=stats["swap_writes"] * 4096)
    for field, expected in stats.items():
        assert run[field] == expected, (path, run["policy"], run["frames"], field, expected, run[field])
    for field, counter in (("page_fault_rate", "page_faults"), ("tlb_hit_rate", "tlb_hits")):
        assert math.isclose(run[field], stats[counter] / stats["accesses"], abs_tol=1e-10)


cases = 0
for pattern in ("*-vm/vm.json", "*-lackey/vm.json"):
    for summary in sorted((ROOT / "results").glob(pattern)):
        for run in json.loads(summary.read_text()):
            assert run["status"] == "passed"
            source = "page-touches.csv.gz" if run["trace"] == "file" else run["trace"] + "-input.csv.gz"
            check(summary.parent / source, run); cases += 1
        print("PASS independent paging counters:", summary.parent.name, flush=True)
print("PASS", cases, "paging cases")

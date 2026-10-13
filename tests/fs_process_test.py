#!/usr/bin/env python3
"""Exercise executable boundaries and kill real writers at two commit points."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import tempfile


def run_cases(build, evidence=None):
    records = []
    with tempfile.TemporaryDirectory(prefix="os-fs-process-") as temporary:
        root = Path(temporary)

        def run(tool, *args, expected=0):
            result = subprocess.run([str(build / tool), *map(str, args)], capture_output=True, text=True, timeout=20)
            if result.returncode != expected:
                raise AssertionError((tool, args, result.returncode, result.stdout, result.stderr))
            return result.stdout

        def cli(image, op, *args):
            return run("fs_cli", "--image", image, "--op", op, *args)

        def inspect(image, expected):
            before = hashlib.sha256(image.read_bytes()).hexdigest()
            report = json.loads(run("fs_check", "--image", image, expected=expected))
            assert hashlib.sha256(image.read_bytes()).hexdigest() == before, "fsck modified its source"
            return report

        image = root / "normal.img"
        run("fs_mkfs", "--image", image, "--blocks", 4096)
        source = root / "source.bin"
        source.write_bytes(bytes(range(256)) * 20000)
        cli(image, "mkdir", "--path", "/data")
        cli(image, "put", "--path", "/data/file", "--input", source)
        cli(image, "link", "--path", "/data/file", "--to", "/alias")
        cli(image, "rename", "--path", "/data", "--to", "/moved")
        output = root / "copy.bin"
        cli(image, "get", "--path", "/moved/file", "--output", output)
        assert output.read_bytes() == source.read_bytes()
        assert json.loads(cli(image, "stat", "--path", "/alias"))["links"] == 2
        records.append({"case": "cli_double_indirect_roundtrip", "bytes": source.stat().st_size,
                        "sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "fsck": inspect(image, 0)})

        for point in ("bitmap-written", "data-written"):
            image = root / (point + ".img")
            run("fs_mkfs", "--image", image, "--blocks", 4096)
            cli(image, "create", "--path", "/file")
            if point == "data-written":
                source.write_bytes(b"A" * 8192)
                cli(image, "write", "--path", "/file", "--input", source)
            source.write_bytes(b"B" * 8192)
            command = [str(build / "fs_cli"), "--image", str(image), "--op", "write",
                       "--path", "/file", "--input", str(source)]
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       env={**os.environ, "OS_FS_CRASH_POINT": point})
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stderr, selectors.EVENT_READ)
                    assert selector.select(20), "writer did not reach crash point"
                    marker = process.stderr.readline().decode()
                    assert marker == f"CRASH_POINT {point}\n", marker
                # The writer stopped after fsync of a real image block. Kill it without cleanup.
                process.send_signal(signal.SIGKILL)
                assert process.wait(timeout=10) == -signal.SIGKILL
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            report = inspect(image, 2 if point == "bitmap-written" else 0)
            record = {"case": point, "writer_exit": process.returncode, "marker": marker.strip(), "fsck": report}
            if point == "bitmap-written":
                assert any("unreferenced" in issue for issue in report["issues"]), report
            else:
                cli(image, "get", "--path", "/file", "--output", output)
                data = output.read_bytes()
                assert data == b"B" * 4096 + b"A" * 4096
                record["observed_data"] = "4096 new B bytes followed by 4096 old A bytes; structure clean, write not atomic"
            if evidence:
                with gzip.open(evidence / (point + ".img.gz"), "wb") as stream:
                    stream.write(image.read_bytes())
                record["image_sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
            records.append(record)
    return records


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    records = run_cases(args.build, args.evidence)
    text = json.dumps(records, indent=2) + "\n"
    if args.evidence:
        (args.evidence / "process-cases.json").write_text(text)
    print(text)

#!/usr/bin/env python3
"""Real Linux system calls through a mounted FUSE image, isolated in a container."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import errno
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def run_cases(build):
    with tempfile.TemporaryDirectory(prefix="os-fs-fuse-") as temporary:
        root = Path(temporary)
        image, mountpoint = root / "image", root / "mount"
        opened = set()
        mounted = False
        mountpoint.mkdir()
        subprocess.run([str(build / "fs_mkfs"), "--image", str(image), "--inodes", "512"], check=True, capture_output=True)
        with (root / "mount.log").open("w+") as log:
            process = subprocess.Popen([str(build / "fs_mount"), str(image), str(mountpoint)], stdout=log, stderr=log)
            try:
                for _ in range(100):
                    if os.path.ismount(mountpoint):
                        mounted = True
                        break
                    if process.poll() is not None:
                        log.seek(0)
                        raise AssertionError("mount exited: " + log.read())
                    time.sleep(0.05)
                else:
                    raise AssertionError("mount timeout")
                started = time.monotonic()
                (mountpoint / "a").mkdir()
                (mountpoint / "b").mkdir()
                path = mountpoint / "a" / "file"
                fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o640)
                opened.add(fd)
                data = bytes(range(256)) * 23
                for boundary in (8 * 4096, (8 + 1024) * 4096):
                    assert os.pwrite(fd, data, boundary - 17) == len(data)
                    assert os.pread(fd, len(data), boundary - 17) == data
                assert os.pread(fd, 4096, 0) == bytes(4096)
                os.fsync(fd)
                alias = mountpoint / "b" / "alias"
                os.link(path, alias)
                assert path.stat().st_ino == alias.stat().st_ino
                assert os.fstat(fd).st_nlink == 2
                path.unlink()
                alias.unlink()
                assert os.fstat(fd).st_nlink == 0
                os.ftruncate(fd, 1)
                os.ftruncate(fd, 8192)
                assert os.pread(fd, 8192, 0) == bytes(8192)
                os.close(fd)
                opened.remove(fd)
                # Rename over an open file must preserve the old handle's data.
                old, replacement = mountpoint / "old", mountpoint / "replacement"
                old.write_bytes(b"old")
                old_fd = os.open(old, os.O_RDONLY)
                opened.add(old_fd)
                replacement.write_bytes(b"new")
                replacement.rename(old)
                assert old.read_bytes() == b"new"
                assert os.read(old_fd, 10) == b"old"
                os.close(old_fd)
                opened.remove(old_fd)
                (mountpoint / "a" / "sub").mkdir()
                (mountpoint / "a" / "sub").rename(mountpoint / "b" / "sub")
                assert (mountpoint / "b" / "sub" / "..").stat().st_ino == (mountpoint / "b").stat().st_ino
                try:
                    (mountpoint / "b").rename(mountpoint / "b" / "sub" / "cycle")
                except OSError as error:
                    assert error.errno == errno.EINVAL
                else:
                    raise AssertionError("accepted directory cycle")
                os.chmod(old, 0o600)
                assert old.stat().st_mode & 0o777 == 0o600
                os.utime(old, ns=(123456789000, 123456789000))
                assert old.stat().st_mtime_ns == 123456789000
                directory_fd = os.open(mountpoint / "a", os.O_RDONLY | os.O_DIRECTORY)
                opened.add(directory_fd)
                assert os.fstat(directory_fd).st_ino == (mountpoint / "a").stat().st_ino
                os.close(directory_fd)
                opened.remove(directory_fd)

                def worker(index):
                    for iteration in range(12):
                        p = mountpoint / f"w{index}-{iteration}"
                        value = bytes([index, iteration]) * 8192
                        p.write_bytes(value)
                        assert p.read_bytes() == value
                with ThreadPoolExecutor(max_workers=4) as pool:
                    list(pool.map(worker, range(4)))
                assert len(list(mountpoint.iterdir())) == 51
                assert os.statvfs(mountpoint).f_bavail > 0
                elapsed = time.monotonic() - started
            finally:
                for descriptor in opened:
                    try:
                        os.close(descriptor)
                    except OSError:
                        pass
                if mounted:
                    unmount = subprocess.run(["fusermount3", "-u", str(mountpoint)], capture_output=True, text=True, timeout=10)
                    if unmount.returncode:
                        print(unmount.stderr, file=sys.stderr)
                        subprocess.run(["fusermount3", "-uz", str(mountpoint)], check=True, timeout=10)
                if process.poll() is None:
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                log.seek(0)
                mount_log = log.read()
                if mount_log:
                    print(mount_log, file=sys.stderr)
            assert process.returncode == 0
        report = json.loads(subprocess.check_output([str(build / "fs_check"), "--image", str(image)], text=True))
        assert report["clean"]
        return {"mounted": True, "workers": 4, "concurrent_roundtrips": 48, "roundtrip_bytes_each": 16384,
                "functional_workload_seconds": elapsed, "fsck": report, "mount_log": mount_log}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    if os.environ.get("OS_FS_TEST_FUSE") != "1":
        print("FUSE test requires OS_FS_TEST_FUSE=1, /dev/fuse, and mount capability")
        raise SystemExit(77)
    result = run_cases(args.build)
    text = json.dumps(result, indent=2) + "\n"
    if args.evidence:
        (args.evidence / "fuse-cases.json").write_text(text)
    print(text)

#!/usr/bin/env python3
"""Run on the host after make image; save Docker's image measurement verbatim."""
import datetime as dt
import gzip
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

root = Path(__file__).resolve().parents[1]
destination = root / "results/m7"
destination.mkdir(parents=True, exist_ok=True)
image = "os-subsystems:local"
command = ["docker", "image", "inspect", image]
inspection = json.loads(subprocess.check_output(command, text=True))[0]
smoke_command = ["docker", "run", "--rm", "-v", f"{root}/scripts/runtime-smoke.sh:/smoke.sh:ro", image, "sh", "/smoke.sh"]
smoke = subprocess.run(smoke_command, capture_output=True, text=True, timeout=60)
(destination / "runtime-smoke.log").write_text(smoke.stdout + smoke.stderr)
hash_command = ["docker", "run", "--rm", image, "sh", "-c", "cd /usr/local && sha256sum bin/*"]
hashes = subprocess.check_output(hash_command, text=True)
(destination / "runtime-binaries.sha256").write_text(hashes)
listing_command = ["docker", "image", "ls", "--format", "{{json .}}", image]
listing = json.loads(subprocess.check_output(listing_command, text=True))
(root / "work").mkdir(exist_ok=True)
layers = []
with tempfile.TemporaryDirectory(dir=root / "work") as temporary:
    archive = Path(temporary) / "image.tar"
    subprocess.run(["docker", "image", "save", "-o", str(archive), image], check=True)
    with tarfile.open(archive) as saved:
        manifest = json.load(saved.extractfile("manifest.json"))
        if len(manifest) != 1:
            raise RuntimeError("expected one exported image")
        for name in manifest[0]["Layers"]:
            source = saved.extractfile(name)
            magic = source.read(2); source.seek(0)
            stream = gzip.GzipFile(fileobj=source) if magic == b"\x1f\x8b" else source
            size = 0
            while block := stream.read(1024 * 1024):
                size += len(block)
            layers.append({"blob": name, "stored_bytes": saved.getmember(name).size,
                           "uncompressed_tar_bytes": size})
uncompressed = sum(layer["uncompressed_tar_bytes"] for layer in layers)
record = {"utc": dt.datetime.now(dt.timezone.utc).isoformat(), "command": command,
          "image_id": inspection["Id"], "architecture": inspection["Architecture"], "os": inspection["Os"],
          "inspect_size_bytes": inspection["Size"], "docker_ls_size": listing["Size"],
          "docker_ls_command": listing_command,
          "uncompressed_layer_tar_bytes": uncompressed, "uncompressed_layer_tar_MB": uncompressed / 1_000_000,
          "layer_blob_bytes": sum(layer["stored_bytes"] for layer in layers), "layers": layers,
          "user": inspection["Config"]["User"],
          "definition": "Sum of uncompressed layer tar streams, including tar metadata and padding; does not include compressed copies or storage-block allocation",
          "inspect_caveat": "Engine 29.7.2 with containerd reports content-only size in inspect. image ls also counts unpacked snapshots. Do not equate these metrics.",
          "docker_version": json.loads(subprocess.check_output(["docker", "version", "--format", "{{json .}}"], text=True)),
          "smoke_command": smoke_command, "smoke_returncode": smoke.returncode,
          "binary_hash_command": hash_command}
(destination / "image.json").write_text(json.dumps(record, indent=2) + "\n")
if smoke.returncode:
    raise RuntimeError("runtime smoke test failed; see results/m7/runtime-smoke.log")
print(json.dumps(record, indent=2))

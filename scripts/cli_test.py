#!/usr/bin/env python3
"""Exercise real executables, including graceful signal handling under TSan."""
import json
import selectors
import signal
import subprocess
import sys

for stop_signal in (signal.SIGTERM, signal.SIGINT):
    server = subprocess.Popen([sys.argv[1], "--port", "0"], text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(server.stdout, selectors.EVENT_READ)
            assert selector.select(10), "server readiness timeout"
        line = server.stdout.readline().strip()
        assert line.startswith("PORT "), line
        client = subprocess.run([sys.argv[2], "--port", line.split()[1], "--connections", "8",
                                 "--requests", "10", "--pipeline", "4"],
                                capture_output=True, text=True, timeout=20)
        assert client.returncode == 0, client.stderr
        assert json.loads(client.stdout)["completed"] == 80
        server.send_signal(stop_signal)
        _, stderr = server.communicate(timeout=10)
        assert server.returncode == 0, stderr
        assert "peak_connections=8" in stderr, stderr
        assert "Sanitizer" not in stderr, stderr
    finally:
        if server.poll() is None:
            server.kill(); server.communicate()

bad = subprocess.run([sys.argv[2], "--connections", "-1"], capture_output=True, text=True, timeout=5)
assert bad.returncode != 0 and "invalid value" in bad.stderr
print("PASS CLI requests, signal shutdown, and argument rejection")

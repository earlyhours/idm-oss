#!/usr/bin/env python3
"""Compare ordinary curl downloads with the shipped application, including hashes."""
import argparse
from datetime import datetime, timezone
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import platform
from pathlib import Path
import statistics
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = bytes(range(256)) * 8192
DIGEST = hashlib.sha256(PAYLOAD).hexdigest()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError):
            pass

    def do_HEAD(self):
        self.respond(False)

    def do_GET(self):
        self.respond(True)

    def respond(self, body):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/fast.bin")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/missing":
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        ranged = self.path not in ("/no-range.bin", "/unknown-size.bin")
        first, last = 0, len(PAYLOAD) - 1
        header = self.headers.get("Range", "")
        if ranged and header:
            import re
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", header)
            if not match:
                self.send_error(416)
                return
            first = int(match[1])
            last = min(int(match[2]) if match[2] else last, last)
            if not 0 <= first <= last:
                self.send_error(416)
                return
        self.send_response(206 if ranged and header else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("ETag", '\"' + DIGEST + '\"')
        if self.path != "/unknown-size.bin":
            self.send_header("Content-Length", str(last - first + 1))
        else:
            self.send_header("Connection", "close")
            self.close_connection = True
        if ranged:
            self.send_header("Accept-Ranges", "bytes")
        if ranged and header:
            self.send_header("Content-Range", f"bytes {first}-{last}/{len(PAYLOAD)}")
        self.end_headers()
        if not body:
            return
        try:
            for offset in range(first, last + 1, 32768):
                chunk = PAYLOAD[offset:min(offset + 32768, last + 1)]
                if self.path == "/per-connection-limit.bin":
                    time.sleep(len(chunk) / (2 * 1024 * 1024))
                self.wfile.write(chunk)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def download(url, client, destination, state):
    destination.mkdir()
    if client == "curl":
        target = destination / "ordinary.bin"
        command = ["curl", "--fail", "--location", "--silent", "--show-error",
                   "--connect-timeout", "15", "--max-time", "120", "--output", str(target), url]
    else:
        command = [str(ROOT / "build/idm-native"), "download", url, "--directory", str(destination),
                   "--state-directory", str(state), "--connections", client.rsplit("-", 1)[1], "--json"]
    started = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True, timeout=150)
    elapsed = time.monotonic() - started
    if result.returncode:
        raise AssertionError(f"{client} failed: {result.stderr[-1500:]}")
    if client != "curl":
        job = json.loads(result.stdout.splitlines()[-1])
        assert job["status"] == "complete", job
        target = Path(job["path"])
    return {"seconds": round(elapsed, 6), "bytes": target.stat().st_size,
            "sha256": hashlib.sha256(target.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--public-url", help="Optional public file fetched by every client; timings include Internet variability")
    args = parser.parse_args()
    if not 1 <= args.rounds <= 10:
        parser.error("rounds must be between 1 and 10")
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    clients = ["curl", "idm-native-1", "idm-native-4"]
    report = {"measuredAt": datetime.now(timezone.utc).isoformat(),
              "baseline": "curl default single connection with redirects and HTTP failures enabled",
              "originalWindowsIdmTested": False, "rounds": args.rounds,
              "platform": platform.platform(),
              "sourceCommit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "curlVersion": subprocess.check_output(["curl", "--version"], text=True).splitlines()[0],
              "scenarios": []}
    try:
        with tempfile.TemporaryDirectory(prefix="idm-comparison-") as temporary:
            root = Path(temporary)
            cases = [(name, f"http://127.0.0.1:{server.server_port}/{name}", DIGEST)
                     for name in ["fast.bin", "per-connection-limit.bin", "no-range.bin", "unknown-size.bin", "redirect"]]
            if args.public_url:
                cases.append(("public-file", args.public_url, None))
            for case, url, expected in cases:
                samples = {client: [] for client in clients}
                hashes = set()
                for trial in range(args.rounds):
                    order = clients[trial % len(clients):] + clients[:trial % len(clients)]
                    for client in order:
                        identifier = f"{case}-{trial}-{client}"
                        sample = download(url, client, root / identifier, root / (identifier + "-state"))
                        hashes.add(sample["sha256"])
                        if expected:
                            assert sample["sha256"] == expected, (case, client, sample)
                        samples[client].append(sample)
                assert len(hashes) == 1, f"Different file bytes in {case}: {hashes}"
                summary = {client: {"medianSeconds": round(statistics.median(s["seconds"] for s in values), 6),
                                    "bytes": values[0]["bytes"], "sha256": values[0]["sha256"], "samples": values}
                           for client, values in samples.items()}
                report["scenarios"].append({"name": case, "url": url if case == "public-file" else "local fixture", "clients": summary, "identicalBytes": True})
                print(f"PASS {case}: identical SHA-256 for all clients", flush=True)
            collision = root / "collision"
            collision.mkdir()
            original = collision / "fast.bin"
            original.write_bytes(b"user-owned data")
            result = subprocess.run([str(ROOT / "build/idm-native"), "download",
                                     f"http://127.0.0.1:{server.server_port}/fast.bin", "--directory", str(collision),
                                     "--state-directory", str(root / "collision-state"), "--json"],
                                    capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, result.stderr
            job = json.loads(result.stdout.splitlines()[-1])
            assert original.read_bytes() == b"user-owned data"
            assert hashlib.sha256(Path(job["path"]).read_bytes()).hexdigest() == DIGEST
            assert Path(job["path"]) != original
            report["collisionPreservesExistingFile"] = True
            print("PASS existing destination retained; app publishes a separate complete file", flush=True)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print("Comparison passed. This does not measure original Windows IDM.")


if __name__ == "__main__":
    main()

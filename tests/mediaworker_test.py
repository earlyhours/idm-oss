"""Verify both yt-dlp distribution forms with an isolated worker interpreter."""
import json
import contextlib
import http.server
import re
import shutil
import threading
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
import zipfile


WORKER = Path(__file__).resolve().parents[1] / "src/mediaworker.py"
WORKER_FUNCTIONS = runpy.run_path(str(WORKER))


class MediaWorkerTests(unittest.TestCase):
    def load(self, module=None, executable=None):
        with tempfile.TemporaryDirectory(prefix="idm-ytdlp-") as directory:
            root = Path(directory)
            if executable is not None:
                with zipfile.ZipFile(root / "yt-dlp", "w") as archive:
                    archive.writestr("yt_dlp/__init__.py", executable)
                (root / "yt-dlp").chmod(0o700)
            prefix = ""
            if module is not None:
                package = root / "yt_dlp"
                package.mkdir()
                (package / "__init__.py").write_text(module)
                prefix = f"sys.path.insert(0, {str(root)!r}); "
            script = (f"import runpy, sys; {prefix}"
                      f"worker = runpy.run_path({str(WORKER)!r}); "
                      "print(worker['load_ytdlp']().MARKER)")
            return subprocess.run(["/usr/bin/python3", "-I", "-S", "-c", script],
                                  env={**os.environ, "PATH": directory},
                                  capture_output=True, text=True, timeout=10)

    def test_standalone_zipapp_loads_without_site_packages(self):
        result = self.load(executable="MARKER = 'standalone'")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "standalone")

    def test_installed_python_module_takes_precedence(self):
        result = self.load(module="MARKER = 'module'", executable="MARKER = 'standalone'")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "module")

    def test_installed_python_module_loads_without_executable(self):
        result = self.load(module="MARKER = 'module'")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "module")

    def test_custom_context_headers_are_scoped_case_insensitively(self):
        source = WORKER_FUNCTIONS["origin"]("https://example.com/watch")
        context = {"X-Api-Key": "captured-secret"}
        headers = {"x-api-key": "extractor-secret", "Accept": "video/*", "Cookie": "session=private"}
        scope = WORKER_FUNCTIONS["scope_headers"]
        self.assertEqual(scope(headers, "https://example.com:8443/video", source, context),
                         {"Accept": "video/*"})
        self.assertEqual(scope(headers, "https://example.com/video", source, context),
                         {"Accept": "video/*", "X-Api-Key": "captured-secret"})

    def test_broken_module_dependency_is_not_hidden_by_fallback(self):
        result = self.load(module="import missing_dependency_for_idm_fixture", executable="MARKER = 'standalone'")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing_dependency_for_idm_fixture", result.stderr)

    def test_missing_package_reports_import_failure(self):
        result = self.load()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ModuleNotFoundError", result.stderr)

    def test_malformed_or_non_object_input_emits_one_private_json_error(self):
        expected = {"type": "error", "error": "Media capture failed. The source is unavailable, needs different authorization, or uses an unsupported media format."}
        for value in ('{"private-token":', '[]', 'null', '42', '"private-token"'):
            with self.subTest(value=value):
                result = subprocess.run(["/usr/bin/python3", str(WORKER)], input=value,
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stderr, "")
                self.assertEqual(json.loads(result.stdout), expected)


class HTTPResumeTests(unittest.TestCase):
    @contextlib.contextmanager
    def fixture(self, switch_after_probe=False, query=False):
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                if self.path == "/entry":
                    target = self.server.target
                    self.send_response(302)
                    self.send_header("Location", target)
                    self.end_headers()
                    if switch_after_probe:
                        self.server.target = self.server.new
                    return
                first, last = map(int, re.fullmatch(r"bytes=(\d+)-(\d+)", self.headers["Range"]).groups())
                body = b"AAAAAAAA" if self.path == self.server.old else b"BBBBBBBB"
                self.send_response(206)
                self.send_header("ETag", '\"v1\"')
                self.send_header("Content-Range", f"bytes {first}-{last}/8")
                self.send_header("Content-Length", str(last-first+1))
                self.end_headers()
                data = body[first:last+1]
                if self.path == self.server.old and first == 0 and last == 7:
                    data = data[:4]
                self.wfile.write(data)
                self.close_connection = True

        with tempfile.TemporaryDirectory() as directory:
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            server.old, server.new = ("/file?version=old", "/file?version=new") if query else ("/old", "/new")
            server.target = server.old
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                config = dict(kind="http", directory=directory, curl=shutil.which("curl"),
                              url=f"http://127.0.0.1:{server.server_port}/entry")
                yield server, Path(directory), config
            finally:
                server.shutdown()
                server.server_close()
                thread.join()

    def test_redirect_change_after_interruption_preserves_prefix(self, query=False):
        with self.fixture(query=query) as (server, work, config):
            run = WORKER_FUNCTIONS["run_http"]
            with self.assertRaises(ValueError):
                run(config)
            self.assertEqual((work / "incoming.bin").read_bytes(), b"AAAA")
            record = json.loads((work / "resume.json").read_text())
            server.target = server.new
            # Reload the worker to exercise persisted metadata with no in-memory state.
            with self.assertRaises(ValueError):
                runpy.run_path(str(WORKER))["run_http"](config)
            self.assertEqual((work / "partial.bin").read_bytes(), b"AAAA")
            self.assertEqual(record["effectiveUrl"], config["url"].replace("/entry", server.old))
            self.assertFalse((work / "verified.bin").exists())
            server.target = server.old
            run(config)
            self.assertEqual((work / "verified.bin").read_bytes(), b"AAAAAAAA")

    def test_query_redirect_change_after_interruption_preserves_prefix(self):
        self.test_redirect_change_after_interruption_preserves_prefix(query=True)

    def test_query_redirect_change_between_probe_and_body_is_rejected(self):
        self.test_redirect_change_between_probe_and_body_is_not_recovered_as_valid(query=True)

    def test_redirect_change_between_probe_and_body_is_not_recovered_as_valid(self, query=False):
        with self.fixture(switch_after_probe=True, query=query) as (_, work, config):
            for attempt in range(2):
                with self.assertRaises(ValueError):
                    runpy.run_path(str(WORKER))["run_http"](config)
                self.assertFalse((work / "verified.bin").exists())
                self.assertFalse((work / "partial.bin").exists())

    def test_explicit_direct_url_refresh_can_renew_query(self):
        with self.fixture(query=True) as (server, work, config):
            config["url"] = config["url"].replace("/entry", server.old)
            with self.assertRaises(ValueError):
                WORKER_FUNCTIONS["run_http"](config)
            server.old = "/file?token=fresh"
            config["url"] = config["url"].split("?")[0] + "?token=fresh"
            WORKER_FUNCTIONS["run_http"](config)
            self.assertEqual((work / "verified.bin").read_bytes(), b"AAAAAAAA")

    def test_legacy_redirect_partial_is_preserved_without_url_identity(self):
        with self.fixture() as (_, work, config):
            with self.assertRaises(ValueError):
                WORKER_FUNCTIONS["run_http"](config)
            path = work / "resume.json"
            record = json.loads(path.read_text())
            record.pop("effectiveUrl")
            path.write_text(json.dumps(record))
            with self.assertRaisesRegex(ValueError, "verified resource URL"):
                WORKER_FUNCTIONS["run_http"](config)
            self.assertEqual((work / "incoming.bin").read_bytes(), b"AAAA")


if __name__ == "__main__":
    unittest.main()

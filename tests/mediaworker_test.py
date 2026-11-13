"""Verify both yt-dlp distribution forms with an isolated worker interpreter."""
import json
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


if __name__ == "__main__":
    unittest.main()

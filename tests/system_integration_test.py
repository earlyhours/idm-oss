#!/usr/bin/env python3
"""Exercise real ClamAV with private, disposable antivirus-test fixtures."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def eicar_fixture():
    payload = b"X5O!P%@AP[4\\PZX54(P^)7CC)7}$" + b"EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*"
    assert len(payload) == 68
    assert hashlib.md5(payload).hexdigest() == "44d88612fea8a8f36de82e1278abb02f"
    return payload


class SystemIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.scanner = shutil.which("clamscan")
        self.assertIsNotNone(self.scanner, "Install the free clamav package before running system integration tests")
        self.scratch = tempfile.TemporaryDirectory(prefix="idm-system-test-")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.database = self.root / "custom.hdb"
        payload = eicar_fixture()
        self.database.write_text(f"{hashlib.md5(payload).hexdigest()}:{len(payload)}:IDM.Native.EICAR.Test\n")
        self.database.chmod(0o600)

    def scan(self, name, body, database=None):
        path = self.root / name
        path.write_bytes(body)
        path.chmod(0o600)
        result = subprocess.run([self.scanner, "--database", str(database or self.database), "--no-summary", str(path)],
                                capture_output=True, text=True, timeout=15, check=False)
        self.assertEqual(path.read_bytes(), body, "The test scanner must not modify or execute the fixture")
        self.assertEqual(path.stat().st_mode & 0o111, 0)
        return result

    def test_real_signature_detects_eicar_and_accepts_clean_file(self):
        detected = self.scan("eicar-test.txt", eicar_fixture())
        self.assertEqual(detected.returncode, 1, detected.stdout + detected.stderr)
        self.assertIn("IDM.Native.EICAR.Test", detected.stdout)
        self.assertIn("FOUND", detected.stdout)
        clean = self.scan("clean.txt", b"Clean IDM Native integration test fixture.\n")
        self.assertEqual(clean.returncode, 0, clean.stdout + clean.stderr)
        self.assertIn("OK", clean.stdout)

    def test_missing_signature_database_is_a_scanner_error(self):
        result = self.scan("clean.txt", b"Clean fixture", self.root / "missing.hdb")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_default_official_database_detects_eicar_and_accepts_clean_file(self):
        infected = self.root / "official-eicar.txt"
        clean = self.root / "official-clean.txt"
        infected.write_bytes(eicar_fixture())
        clean.write_bytes(b"Clean default-database fixture.\n")
        infected.chmod(0o600)
        clean.chmod(0o600)
        result = subprocess.run([self.scanner, "--no-summary", str(infected), str(clean)],
                                capture_output=True, text=True, timeout=45, check=False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("EICAR", result.stdout.upper())
        self.assertIn("FOUND", result.stdout)
        self.assertIn(str(clean) + ": OK", result.stdout)

    def test_installed_tools_are_found_without_running_destructive_actions(self):
        names = ("clamscan", "systemctl", "nmcli", "canberra-gtk-play", "notify-send")
        tools = {name: shutil.which(name) for name in names}
        for name, path in tools.items():
            self.assertIsNotNone(path, f"Missing optional system integration tool: {name}")
        version = subprocess.run([self.scanner, "--version"], capture_output=True, text=True, timeout=5, check=True).stdout.strip()
        definitions = Path("/var/lib/clamav")
        production = sorted(path.name for pattern in ("*.cvd", "*.cld") for path in definitions.glob(pattern))
        print(json.dumps({"tools": tools, "scannerVersion": version, "productionDefinitions": production,
                          "executedSystemActions": [], "signatureDatabase": "private custom.hdb test signature only"}, sort_keys=True))


if __name__ == "__main__":
    unittest.main()

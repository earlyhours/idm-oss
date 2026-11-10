#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import unittest

root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('installer',root/'tools/install_browser.py')
installer=importlib.util.module_from_spec(spec);spec.loader.exec_module(installer)
class InstallationTests(unittest.TestCase):
    def test_real_manifest_registration_and_repeat(self):
        with tempfile.TemporaryDirectory() as temporary:
            home=Path(temporary)
            result=installer.install(home,Path('/usr/bin/true'),root/'build')
            self.assertEqual(len(result['manifests']),7)
            manifest=json.loads(Path(result['manifests'][0]).read_text())
            self.assertEqual(manifest['allowed_origins'],['chrome-extension://hjjceibkcmhbdlljimdlpbffdllhjhcb/'])
            wrapper=Path(manifest['path'])
            self.assertIn('browser-host',wrapper.read_text())
            self.assertEqual(stat.S_IMODE(wrapper.stat().st_mode),0o700)
            self.assertEqual(stat.S_IMODE(Path(result['manifests'][0]).stat().st_mode),0o600)
            self.assertEqual(installer.install(home,Path('/usr/bin/true'),root/'build'),result)
    def test_unrelated_registration_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            home=Path(temporary)
            target=home/'.config/chromium/NativeMessagingHosts'/f'{installer.HOST}.json'
            target.parent.mkdir(parents=True);target.write_text('{"name":"unrelated"}')
            with self.assertRaises(ValueError): installer.install(home,Path('/usr/bin/true'),root/'build')
            self.assertEqual(target.read_text(),'{"name":"unrelated"}')
if __name__=='__main__':unittest.main()

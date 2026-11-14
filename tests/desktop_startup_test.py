"""Exercise desktop URL entry points in the real app, with isolated config and no display."""
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/idm-native'


def receive(connection):
    def read(size):
        data = b''
        while len(data) < size:
            part = connection.recv(size - len(data))
            if not part:
                raise AssertionError('Incomplete IPC frame')
            data += part
        return data
    return json.loads(read(struct.unpack('<I', read(4))[0]))


class DesktopStartupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='idm-desktop-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / 'state'
        self.env = dict(os.environ, HOME=str(self.root),
                        XDG_CONFIG_HOME=str(self.root / 'config'),
                        XDG_DATA_HOME=str(self.root / 'data'),
                        XDG_CACHE_HOME=str(self.root / 'cache'),
                        QT_QPA_PLATFORM='offscreen', QT_QUICK_BACKEND='software')

    def run_app(self, *arguments):
        return subprocess.run([str(BINARY), *arguments, '--state-directory', str(self.state)],
                              env=self.env, capture_output=True, timeout=10)

    def assert_no_downloads(self):
        queue = self.state / 'queue.json'
        if queue.exists():
            self.assertEqual(json.loads(queue.read_text())['downloads'], [])
        self.assertFalse((self.root / 'config/autostart/idm-native.desktop').exists())
        self.assertFalse((self.root / 'data/applications/idm-native-url.desktop').exists())

    def test_invalid_external_urls_fail_before_queue_creation(self):
        for url in ('file:///etc/passwd', 'javascript:alert(1)', 'https://',
                    'https://example.com/\nExec=bad',
                    'idm-native://download/?url=file%3A%2F%2F%2Fetc%2Fpasswd',
                    'idm-native://download/?url=https%3A%2F%2Fexample.com&url=https%3A%2F%2Fother.com'):
            result = self.run_app('open', url)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('error', json.loads(result.stderr.splitlines()[-1]))
        self.assertFalse(self.state.exists())

    def test_existing_instance_receives_decoded_url(self):
        self.state.mkdir()
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(str(self.state / 'browser.sock'))
            server.listen(1)
            server.settimeout(5)

            def accept():
                connection, _ = server.accept()
                with connection:
                    connection.settimeout(5)
                    message = receive(connection)
                    reply = b'{"ok":true}'
                    connection.sendall(struct.pack('<I', len(reply)) + reply)
                    return message

            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                incoming = pool.submit(accept)
                result = self.run_app('open', 'idm-native://download/?url=https%3A%2F%2Fexample.com%2Fa%2520file%3Fx%3D1%26y%3D2')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(incoming.result(timeout=5), {
                    'command': 'show', 'url': 'https://example.com/a%20file?x=1&y=2'})
        self.assert_no_downloads()

    def test_cold_open_renders_confirmation_without_downloading(self):
        image = self.root / 'confirmation.png'
        result = self.run_app('open', 'https://example.com/confirmation.bin', '--smoke-test', str(image))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(image.read_bytes().startswith(b'\x89PNG\r\n\x1a\n'))
        self.assertNotIn(b'qrc:/Main.qml:', result.stderr)
        self.assert_no_downloads()

    def test_running_app_accepts_url_without_downloading(self):
        image = self.root / 'warm.png'
        process = subprocess.Popen([str(BINARY), 'open', '--state-directory', str(self.state),
                                    '--smoke-test', str(image)], env=self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while not (self.state / 'browser.sock').exists():
                self.assertIsNone(process.poll(), 'App exited before listening')
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.02)
            result = self.run_app('open', 'ftp://example.com/file.bin')
            self.assertEqual(result.returncode, 0, result.stderr)
            _, error = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, error)
            self.assertNotIn(b'qrc:/Main.qml:', error)
            self.assert_no_downloads()
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


if __name__ == '__main__':
    unittest.main()

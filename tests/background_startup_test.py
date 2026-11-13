"""Cold browser handoff stays hidden and shares its queue with an explicit open."""
import http.server
import threading
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/idm-native'


def frame(value):
    data = json.dumps(value).encode()
    return struct.pack('<I', len(data)) + data


def receive(stream):
    header = stream.read(4)
    assert len(header) == 4
    return json.loads(stream.read(struct.unpack('<I', header)[0]))


with tempfile.TemporaryDirectory(prefix='idm-background-') as temporary:
    state = Path(temporary)
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_QUICK_BACKEND='software')
    host = subprocess.Popen([str(BINARY), 'browser-host', '--state-directory', temporary],
                            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        host.stdin.write(frame({'command': 'ping'}))
        host.stdin.flush()
        assert receive(host.stdout) == {'ok': True, 'running': False, 'name': 'IDM Native'}
        assert not (state / 'browser.sock').exists(), 'Popup ping launched the app'
        host.stdin.write(frame({'command': 'download', 'request': {
            'url': 'https://example.com/background.bin', 'start': False}}))
        host.stdin.flush()
        accepted = receive(host.stdout)
        assert accepted['ok'] and accepted['id'], accepted
        host.stdin.close()
        assert host.wait(timeout=10) == 0
        opened = subprocess.run([str(BINARY), 'open', '--state-directory', temporary],
                                env=env, capture_output=True, timeout=10)
        assert opened.returncode == 0, opened.stderr.decode()
        records = json.loads((state / 'queue.json').read_text())['downloads']
        assert len(records) == 1 and records[0]['id'] == accepted['id']
        assert records[0]['status'] == 'paused'
        payload = b'background-download-fixture' * 4096
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            def log_message(self, *args):
                pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            download_host = subprocess.run([str(BINARY), 'browser-host', '--state-directory', temporary],
                env=env, input=frame({'command': 'download', 'request': {
                    'url': f'http://127.0.0.1:{server.server_port}/file.bin', 'directory': temporary}}),
                capture_output=True, timeout=10)
            import io
            download = receive(io.BytesIO(download_host.stdout))
            assert download['ok'], download
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                jobs = json.loads((state / 'queue.json').read_text())['downloads']
                job = next(row for row in jobs if row['id'] == download['id'])
                assert job['status'] != 'error', job.get('error')
                if job['status'] == 'complete':
                    assert Path(job['path']).read_bytes() == payload
                    break
                time.sleep(.05)
            else:
                raise AssertionError('Background transfer did not complete')
        finally:
            server.shutdown()
            server.server_close()
        print('Background startup passed: passive ping, cold acceptance, shared queue open, exact file bytes')
    finally:
        if host.poll() is None:
            host.kill()
            host.wait()
        for process in Path('/proc').iterdir():
            if not process.name.isdigit():
                continue
            try:
                args = (process / 'cmdline').read_bytes().split(b'\0')
                if str(BINARY).encode() in args and b'background' in args and temporary.encode() in args:
                    os.kill(int(process.name), signal.SIGTERM)
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                pass

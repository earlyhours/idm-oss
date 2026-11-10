#!/usr/bin/env python3
"""Exercise the shipped executable against a local HTTP server."""
import hashlib
import http.server
import json
from pathlib import Path
import subprocess
import ssl
import tempfile
import threading

binary = Path(__file__).resolve().parents[1] / 'build/idm-native'
with tempfile.TemporaryDirectory(prefix='idm-cli-test-') as temporary:
    root = Path(temporary)
    serve, output, state = root/'server', root/'output', root/'state'
    serve.mkdir(); output.mkdir()
    data = bytes(range(256)) * 8192
    (serve/'sample.bin').write_bytes(data)
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(serve), **kwargs)
        def log_message(self, *args): pass
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    def run(url, expected):
        result = subprocess.run([str(binary), 'download', url, '--directory', str(output),
                                 '--state-directory', str(state), '--json'], capture_output=True, text=True, timeout=30)
        assert result.returncode == expected, result.stderr
        return json.loads(result.stdout if expected == 0 else result.stderr.splitlines()[-1])
    try:
        url = f'http://127.0.0.1:{server.server_port}/sample.bin'
        row = run(url, 0)
        assert row['status'] == 'complete'
        assert hashlib.sha256(Path(row['path']).read_bytes()).digest() == hashlib.sha256(data).digest()
        again = run(url, 0)
        assert again['path'] != row['path']
        assert Path(row['path']).read_bytes() == data
        assert Path(again['path']).read_bytes() == data
        assert run(f'http://127.0.0.1:{server.server_port}/missing', 1)['status'] == 'error'
        assert 'error' in run('file:///etc/passwd', 1)
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-keyout', str(root/'key.pem'), '-out', str(root/'cert.pem'),
                        '-subj', '/CN=localhost'], check=True, capture_output=True)
        tls = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(root/'cert.pem'), str(root/'key.pem'))
        tls.socket = context.wrap_socket(tls.socket, server_side=True)
        tls_thread = threading.Thread(target=tls.serve_forever, daemon=True)
        tls_thread.start()
        try:
            failed = run(f'https://127.0.0.1:{tls.server_port}/sample.bin', 1)
            assert failed['status'] == 'error'
            assert not Path(failed['path']).exists()
        finally:
            tls.shutdown(); tls.server_close(); tls_thread.join()
        print('CLI: local download, content hash, existing-file preservation, HTTP error, invalid URL and TLS rejection passed')
    finally:
        server.shutdown(); server.server_close(); thread.join()

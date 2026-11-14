#!/usr/bin/env python3
"""Exercise the real website worker against isolated local HTTP servers."""
import hashlib
import gzip
import fcntl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from urllib.parse import urlsplit

WORKER = Path(__file__).resolve().parents[1] / 'tools' / 'website_worker.py'


class WebsiteTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.output = self.root / 'site'
        self.requests = []
        self.external_requests = []
        self.version = 1
        self.file_body = bytes(range(256)) * 3 + b'exact tail'
        self.range_behavior = 'normal'
        self.on_file_request = None
        owner = self

        class External(BaseHTTPRequestHandler):
            def log_message(self, *_args): pass
            def do_GET(self):
                owner.external_requests.append(self.path)
                self.send_response(200); self.end_headers(); self.wfile.write(b'outside')
        self.external = ThreadingHTTPServer(('127.0.0.1', 0), External)
        self.external_url = f'http://127.0.0.1:{self.external.server_port}'

        class Site(BaseHTTPRequestHandler):
            def log_message(self, *_args): pass
            def do_GET(self):
                owner.requests.append((self.path, dict(self.headers)))
                route = urlsplit(self.path).path
                if route == '/resumable' and owner.range_behavior == 'redirect':
                    self.send_response(302); self.send_header('Location', '/resumable-other'); self.end_headers(); return
                if route in ('/resumable', '/resumable-other'):
                    if owner.on_file_request:
                        owner.on_file_request()
                    body = owner.file_body
                    etag = f'"file-{owner.version}"'
                    modified = 'Mon, 01 Jun 2026 00:00:00 GMT'
                    if self.headers.get('If-None-Match') == etag:
                        self.send_response(304); self.end_headers(); return
                    start, end = 0, len(body) - 1
                    ranged = self.headers.get('Range') and self.headers.get('If-Range') == etag
                    if owner.range_behavior == 'ignore': ranged = False
                    if ranged:
                        start, end = map(int, self.headers['Range'].removeprefix('bytes=').split('-'))
                    self.send_response(206 if ranged else 200)
                    if ranged:
                        value = f'bytes {start}-{end}/{len(body)}'
                        if owner.range_behavior.startswith('range:'):
                            value = owner.range_behavior.removeprefix('range:')
                        self.send_header('Content-Range', value)
                    if owner.range_behavior != 'no-length':
                        self.send_header('Content-Length', str(end - start + 1))
                    if owner.range_behavior != 'no-etag':
                        returned_etag = '"unexpected"' if owner.range_behavior == 'changed-etag' else etag
                        self.send_header('ETag', 'W/' + returned_etag if owner.range_behavior == 'weak' else returned_etag)
                    self.send_header('Last-Modified', modified)
                    self.send_header('Content-Type', 'application/octet-stream')
                    self.end_headers()
                    payload = body[start:end + 1]
                    if owner.range_behavior == 'truncate': payload = payload[:max(1, len(payload) // 2)]
                    try:
                        self.wfile.write(payload)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    return
                if route in ('/jump', '/external-jump'):
                    self.send_response(302)
                    self.send_header('Location', '/index' if route == '/jump' else owner.external_url + '/secret')
                    self.end_headers(); return
                if route == '/missing':
                    self.send_response(503); self.end_headers(); return
                if route in ('/index', '/failed', '/plain', '/gzip', '/bomb'):
                    broken = '<a href="/missing">Broken</a>' if route == '/failed' else ''
                    body = f'''<!doctype html><html><head><title>Generation {owner.version}</title>
<link rel="stylesheet" href="/styles/main.css"><style>.inline{{background:url('/img/inline.png')}}</style></head>
<body><h1>Generation {owner.version}</h1><a href="/page">Next</a><a href="/index">Cycle</a>
<a href="/query?id=one">One</a><a href="/query?id=two">Two</a><img src="/img/photo.png">
<img srcset="data:image/png;base64,AAAA 1x, /img/photo.png?size=2 2x">
<img style="background-image:url('/img/attribute.png')" src="{owner.external_url}/external.png">
<a href="#anchor">Anchor</a><a href="javascript:alert(1)">JS</a><a href="file:///etc/passwd">File</a>
{broken}<p id="anchor">End</p></body></html>'''.encode()
                    mime = 'text/plain' if route == '/plain' else 'text/html; charset=utf-8'
                    etag = f'"index-{owner.version}-{route}"'
                elif route == '/page':
                    body = b'<html><head></head><body><a href="/index">Back</a><a href="/deep">Deep</a><img src="/img/photo.png"></body></html>'
                    mime, etag = 'text/html', '"page-1"'
                elif route == '/deep':
                    body, mime, etag = b'<html><body>Deep page</body></html>', 'text/html', '"deep"'
                elif route == '/query':
                    body, mime, etag = ('<html><body>' + self.path + '</body></html>').encode(), 'text/html', '"' + self.path + '"'
                elif route == '/styles/main.css':
                    body = b'@import "more.css"; .hero{background:url("../img/bg.png")} .data{background:url(data:image/png;base64,AAAA)}'
                    mime, etag = 'text/css; charset=utf-8', ''
                elif route == '/styles/more.css':
                    body, mime, etag = b'.more{background:url(/img/more.png)}', 'text/css', '"more"'
                elif route.startswith('/img/'):
                    body, mime, etag = ('PNG fixture ' + self.path).encode(), 'image/png', '"' + self.path + '"'
                else:
                    self.send_response(404); self.end_headers(); return
                compressed = route in ('/gzip', '/bomb')
                if route == '/bomb': body = b'<html><body>' + b'x' * 100000 + b'</body></html>'
                if compressed: body = gzip.compress(body)
                modified = 'Mon, 01 Jun 2026 00:00:00 GMT'
                if (etag and self.headers.get('If-None-Match') == etag) or (not etag and self.headers.get('If-Modified-Since') == modified):
                    self.send_response(304); self.end_headers(); return
                ranged = self.headers.get('Range') and self.headers.get('If-Range') == etag
                total = len(body)
                if ranged:
                    start, end = map(int, self.headers['Range'].removeprefix('bytes=').split('-'))
                    body = body[start:end + 1]
                self.send_response(206 if ranged else 200)
                if ranged: self.send_header('Content-Range', f'bytes {start}-{end}/{total}')
                self.send_header('Content-Type', mime)
                self.send_header('Content-Length', str(len(body)))
                if compressed: self.send_header('Content-Encoding', 'gzip')
                self.send_header('Last-Modified', modified)
                if etag: self.send_header('ETag', etag)
                self.end_headers(); self.wfile.write(body)
        self.site = ThreadingHTTPServer(('127.0.0.1', 0), Site)
        self.url = f'http://127.0.0.1:{self.site.server_port}'
        for server in (self.site, self.external):
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()

    def tearDown(self):
        for server in (self.site, self.external): server.shutdown(); server.server_close()
        self.temp.cleanup()

    def run_worker(self, path='/index', **extra):
        job = {'url': self.url + path, 'directory': str(self.output), 'depth': 2, 'maxFiles': 40, 'maxBytes': 1000000, **extra}
        file = self.root / 'job.json'; file.write_text(json.dumps(job)); file.chmod(0o600)
        process = subprocess.run([sys.executable, str(WORKER), str(file)], text=True, capture_output=True, timeout=20)
        self.assertFalse(process.stderr, process.stderr)
        rows = [json.loads(line) for line in process.stdout.splitlines()]
        self.assertTrue(rows)
        return process.returncode, rows

    def manifest(self):
        return json.loads((self.output / '.idm-site-manifest.json').read_text())

    def test_real_crawl_rewrites_html_css_assets_queries_and_cycles(self):
        code, rows = self.run_worker(headers={'Cookie': 'session=private', 'Referer': self.url + '/start'})
        self.assertEqual(code, 0, rows)
        self.assertEqual(rows[-1]['event'], 'complete'); self.assertTrue(rows[-1]['ok'])
        self.assertFalse(rows[-1]['limited'])
        manifest = self.manifest(); resources = manifest['resources']
        one, two = resources[self.url + '/query?id=one'], resources[self.url + '/query?id=two']
        self.assertNotEqual(one['path'], two['path'])
        self.assertIn('id=one', (self.output / one['path']).read_text())
        self.assertIn('id=two', (self.output / two['path']).read_text())
        html = (self.output / 'index.html').read_text()
        self.assertIn('href="' + resources[self.url + '/page']['path'] + '"', html)
        self.assertIn('src="' + resources[self.url + '/img/photo.png']['path'] + '"', html)
        self.assertIn('data:image/png;base64,AAAA 1x', html)
        self.assertIn(resources[self.url + '/img/photo.png?size=2']['path'] + ' 2x', html)
        css_path = self.output / resources[self.url + '/styles/main.css']['path']
        css = css_path.read_text()
        self.assertIn(Path(resources[self.url + '/styles/more.css']['path']).name, css)
        self.assertIn(Path(resources[self.url + '/img/bg.png']['path']).name, css)
        self.assertNotIn('../img/bg.png', css)
        self.assertIn(self.url + '/img/inline.png', resources)
        self.assertIn(self.url + '/img/attribute.png', resources)
        self.assertEqual(self.external_requests, [])
        requested = [path for path, _ in self.requests]
        self.assertEqual(len(requested), len(set(requested)))
        self.assertTrue(all(headers.get('Cookie') == 'session=private' for _, headers in self.requests))
        self.assertNotIn('private', json.dumps(rows))
        self.assertEqual((self.output / '.idm-site-manifest.json').stat().st_mode & 0o077, 0)
        for relative in manifest['managed']:
            self.assertTrue((self.output / relative).resolve().is_relative_to(self.output))

    def test_conditional_sync_304_then_changed_page_and_unrelated_file_preserved(self):
        self.output.mkdir(); unrelated = self.output / 'notes.txt'; unrelated.write_bytes(b'keep this original')
        code, rows = self.run_worker(); self.assertEqual(code, 0, rows)
        original = (self.output / 'index.html').read_bytes()
        original_mtime = (self.output / 'index.html').stat().st_mtime_ns
        self.requests.clear()
        code, rows = self.run_worker(); self.assertEqual(code, 0, rows)
        self.assertTrue(all(row.get('status') == 304 for row in rows[:-1]))
        self.assertEqual(rows[-1]['transferredBytes'], 0)
        self.assertEqual(original, (self.output / 'index.html').read_bytes())
        self.assertEqual(original_mtime, (self.output / 'index.html').stat().st_mtime_ns)
        self.assertTrue(any('If-None-Match' in headers for _, headers in self.requests))
        self.assertTrue(any('If-Modified-Since' in headers for _, headers in self.requests))
        self.version = 2
        code, rows = self.run_worker(); self.assertEqual(code, 0, rows)
        self.assertIn('Generation 2', (self.output / 'index.html').read_text())
        self.assertEqual(unrelated.read_bytes(), b'keep this original')

    def test_mime_sniff_and_same_origin_redirect_keep_offline_fragment(self):
        code, rows = self.run_worker('/plain'); self.assertEqual(code, 0, rows)
        self.assertIn('<meta charset="utf-8">', (self.output / 'index.html').read_text())
        self.output = self.root / 'redirected'
        code, rows = self.run_worker('/jump'); self.assertEqual(code, 0, rows)
        html = (self.output / 'index.html').read_text()
        self.assertIn('href="index.html#anchor"', html)
        self.assertEqual(sum(value == '/jump' for value, _ in self.requests), 1)

    def test_origin_redirect_block_and_network_failure_report_error_after_progress(self):
        code, rows = self.run_worker('/external-jump', headers={'Cookie': 'do-not-leak'})
        self.assertEqual(code, 1); self.assertEqual(rows[-1]['event'], 'error')
        self.assertIn('origin', rows[-1]['error']); self.assertEqual(self.external_requests, [])
        self.output = self.root / 'failed'
        code, rows = self.run_worker('/failed')
        self.assertEqual(code, 1); self.assertEqual(rows[0]['event'], 'progress')
        self.assertEqual(rows[-1]['event'], 'error'); self.assertIn('HTTP 503', rows[-1]['error'])
        self.assertFalse(any(row['event'] == 'complete' for row in rows))

    def test_existing_originals_user_edits_and_symlinks_are_never_overwritten(self):
        self.output.mkdir(); original = self.output / 'index.html'; original.write_bytes(b'original document')
        code, rows = self.run_worker(); self.assertEqual(code, 1); self.assertIn('overwrite', rows[-1]['error'])
        self.assertEqual(original.read_bytes(), b'original document')
        self.output = self.root / 'managed'
        code, rows = self.run_worker(); self.assertEqual(code, 0, rows)
        edited = self.output / 'index.html'; edited.write_bytes(b'user changed this')
        code, rows = self.run_worker(); self.assertEqual(code, 1); self.assertIn('modified', rows[-1]['error'])
        self.assertEqual(edited.read_bytes(), b'user changed this')
        self.output = self.root / 'symlink'; self.output.mkdir(); outside = self.root / 'outside'; outside.mkdir()
        (self.output / 'files').symlink_to(outside, target_is_directory=True)
        code, rows = self.run_worker(); self.assertEqual(code, 1); self.assertIn('Symlinks', rows[-1]['error'])
        self.assertEqual(list(outside.iterdir()), [])

    def test_depth_file_and_byte_budgets_are_visible_and_bounded(self):
        code, rows = self.run_worker(depth=0, maxFiles=2); self.assertEqual(code, 0, rows)
        self.assertTrue(rows[-1]['limited']); self.assertEqual(rows[-1]['files'], 2)
        self.assertNotIn(self.url + '/page', self.manifest()['resources'])
        self.output = self.root / 'tiny'
        code, rows = self.run_worker(maxBytes=10)
        self.assertEqual(code, 1); self.assertIn('byte budget', rows[-1]['error'])
        self.assertFalse((self.output / 'index.html').exists())

    def test_interrupted_cache_metadata_never_reuses_a_mismatched_validator_body(self):
        code, rows = self.run_worker(); self.assertEqual(code, 0, rows)
        manifest = self.manifest()
        manifest['resources'][self.url + '/index']['rawSha256'] = '0' * 64
        (self.output / '.idm-site-manifest.json').write_text(json.dumps(manifest))
        self.requests.clear()
        code, rows = self.run_worker(); self.assertEqual(code, 0, rows)
        root_headers = next(headers for path, headers in self.requests if path == '/index')
        self.assertNotIn('If-None-Match', root_headers)
        self.assertEqual(rows[0]['status'], 200)
        self.assertIn('Generation 1', (self.output / 'index.html').read_text())

    def test_compression_decodes_real_html_and_bounds_expansion(self):
        code, rows = self.run_worker('/gzip'); self.assertEqual(code, 0, rows)
        self.assertIn('Generation 1', (self.output / 'index.html').read_text())
        self.assertLess(rows[-1]['transferredBytes'], rows[-1]['bytes'])
        self.output = self.root / 'bomb'
        code, rows = self.run_worker('/bomb', maxBytes=1000)
        self.assertEqual(code, 1); self.assertIn('byte budget', rows[-1]['error'])
        self.assertFalse((self.output / 'index.html').exists())

    def test_directory_lock_rejects_an_overlapping_job_before_any_fetch(self):
        self.output.mkdir()
        with (self.output / '.idm-site.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            code, rows = self.run_worker()
        self.assertEqual(code, 1); self.assertIn('already using', rows[-1]['error'])
        self.assertEqual(self.requests, [])

    def test_individual_file_sync_retains_validators_and_protects_outputs(self):
        code, rows = self.run_worker('/img/photo.png', mode='file', filename='photo.png')
        self.assertEqual(code, 0, rows)
        target = self.output / 'photo.png'
        original = target.read_bytes()
        stamp = target.stat().st_mtime_ns
        code, rows = self.run_worker('/img/photo.png', mode='file', filename='photo.png')
        self.assertEqual(code, 0, rows)
        self.assertEqual(rows[-1]['status'], 304)
        self.assertTrue(rows[-1]['unchanged'])
        self.assertEqual(rows[-1]['transferredBytes'], 0)
        self.assertEqual(target.stat().st_mtime_ns, stamp)
        self.assertEqual(target.read_bytes(), original)
        target.write_bytes(b'user edit')
        code, rows = self.run_worker('/img/photo.png', mode='file', filename='photo.png')
        self.assertEqual(code, 1)
        self.assertEqual(target.read_bytes(), b'user edit')
        self.output = self.root / 'unrelated'
        self.output.mkdir()
        (self.output / 'photo.png').write_bytes(b'original')
        code, rows = self.run_worker('/img/photo.png', mode='file', filename='photo.png')
        self.assertEqual(code, 1)
        self.assertEqual((self.output / 'photo.png').read_bytes(), b'original')

    def test_individual_changed_file_atomically_replaces_owned_bytes(self):
        code, rows = self.run_worker(mode='file', filename='document.html')
        self.assertEqual(code, 0, rows)
        old = (self.output / 'document.html').open('rb')
        self.version = 2
        code, rows = self.run_worker(mode='file', filename='document.html')
        self.assertEqual(code, 0, rows)
        self.assertFalse(rows[-1]['unchanged'])
        self.assertIn(b'Generation 1', old.read())
        old.close()
        self.assertIn(b'Generation 2', (self.output / 'document.html').read_bytes())
        self.assertEqual(len(list(self.output.glob('.idm-file-*.json'))), 1)

    def test_quota_counts_real_bytes_on_failed_and_successful_worker_runs(self):
        code, rows = self.run_worker('/img/photo.png', mode='file', filename='photo.png', transferLimit=5)
        self.assertEqual(code, 2, rows)
        self.assertEqual(rows[-1]['event'], 'quota')
        self.assertEqual(max(row.get('transferredBytes', 0) for row in rows), 5)
        self.assertFalse((self.output / 'photo.png').exists())
        code, rows = self.run_worker('/img/photo.png', mode='file', filename='photo.png', transferLimit=100)
        self.assertEqual(code, 0, rows)
        self.assertEqual(rows[-1]['transferredBytes'], len((self.output / 'photo.png').read_bytes()) - 5)
        code, rows = self.run_worker('/img/photo.png', mode='file', filename='photo.png', transferLimit=0)
        self.assertEqual(code, 0, rows)
        self.assertEqual(rows[-1]['transferredBytes'], 0)

    def file_sync(self, budget=100, **extra):
        return self.run_worker('/resumable', mode='file', filename='payload.bin', transferLimit=budget, **extra)

    def file_manifest(self):
        return json.loads(next(self.output.glob('.idm-file-*.json')).read_text())

    def test_completed_file_project_can_change_url_without_adopting_user_edits(self):
        self.assertEqual(self.file_sync(2000)[0], 0)
        self.file_body = b'new URL content'
        code, rows = self.run_worker('/resumable-other', mode='file', filename='payload.bin')
        self.assertEqual(code, 0, rows)
        self.assertEqual((self.output / 'payload.bin').read_bytes(), b'new URL content')
        (self.output / 'payload.bin').write_bytes(b'user edit')
        before = len(self.requests)
        code, rows = self.file_sync(2000)
        self.assertEqual(code, 1, rows)
        self.assertEqual(len(self.requests), before)
        self.assertEqual((self.output / 'payload.bin').read_bytes(), b'user edit')

    def test_streamed_file_budget_can_exceed_the_website_memory_budget(self):
        code, rows = self.file_sync(2000, maxBytes=2 * 1024 ** 3)
        self.assertEqual(code, 0, rows)
        self.assertEqual((self.output / 'payload.bin').read_bytes(), self.file_body)
        code, rows = self.run_worker(maxBytes=2 * 1024 ** 3)
        self.assertEqual(code, 1)
        self.assertIn('maxBytes', rows[-1]['error'])

    def test_file_resumes_across_real_worker_restarts_with_bounded_private_partials(self):
        target = self.output / 'payload.bin'
        total = 0
        for offset in range(0, len(self.file_body), 100):
            code, rows = self.file_sync()
            spent = min(100, len(self.file_body) - offset)
            total += spent
            self.assertEqual(max(row.get('transferredBytes', 0) for row in rows), spent, rows)
            headers = self.requests[-1][1]
            if offset:
                self.assertEqual(headers.get('Range'), f'bytes={offset}-{offset + spent - 1}')
                self.assertEqual(headers.get('If-Range'), '"file-1"')
                self.assertNotIn('If-None-Match', headers)
            if total < len(self.file_body):
                self.assertEqual(code, 2, rows)
                self.assertFalse(target.exists())
                partial = next(self.output.glob('*.part'))
                self.assertEqual(partial.read_bytes(), self.file_body[:total])
                self.assertEqual(partial.stat().st_mode & 0o077, 0)
                state = self.file_manifest()['partial']
                self.assertEqual(state['size'], total)
                self.assertEqual(state['sha256'], hashlib.sha256(self.file_body[:total]).hexdigest())
            else:
                self.assertEqual(code, 0, rows)
        self.assertEqual(target.read_bytes(), self.file_body)
        self.assertEqual(list(self.output.glob('*.part')), [])
        self.assertNotIn('partial', self.file_manifest())
        code, rows = self.file_sync(0)
        self.assertEqual(code, 0, rows)
        self.assertEqual(rows[-1]['status'], 304)
        self.assertEqual(rows[-1]['transferredBytes'], 0)

    def test_changed_remote_validator_discards_old_partial_before_restart(self):
        code, rows = self.file_sync()
        self.assertEqual(code, 2, rows)
        self.version = 2
        self.file_body = b'new generation ' * 13
        code, rows = self.file_sync()
        self.assertEqual(code, 1, rows)
        self.assertIn('restart', rows[-1]['error'])
        self.assertFalse((self.output / 'payload.bin').exists())
        self.assertEqual(list(self.output.glob('*.part')), [])
        for _ in range(2):
            code, rows = self.file_sync()
        self.assertEqual(code, 0, rows)
        self.assertEqual((self.output / 'payload.bin').read_bytes(), self.file_body)

    def test_ignored_range_reports_restart_and_never_publishes_partial(self):
        self.assertEqual(self.file_sync()[0], 2)
        self.range_behavior = 'ignore'
        code, rows = self.file_sync()
        self.assertEqual(code, 1, rows)
        self.assertIn('ignored Range', rows[-1]['error'])
        self.assertIn('restart', rows[-1]['error'])
        self.assertFalse((self.output / 'payload.bin').exists())
        self.assertEqual(list(self.output.glob('*.part')), [])
        self.assertEqual(max(row.get('transferredBytes', 0) for row in rows), 0)

    def test_malicious_content_ranges_are_rejected_before_appending(self):
        total = len(self.file_body)
        for value in (f'bytes 0-199/{total}', f'bytes 101-199/{total}', f'bytes 100-200/{total}',
                      f'bytes 100-199/{total + 1}', f'bytes 199-100/{total}', 'bytes 100-199/*',
                      f'bytes -100-199/{total}', f'bytes 100-199/{total}, 200-299/{total}'):
            with self.subTest(value=value):
                self.output = self.root / hashlib.sha256(value.encode()).hexdigest()
                self.range_behavior = 'normal'
                self.assertEqual(self.file_sync()[0], 2)
                self.range_behavior = 'range:' + value
                code, rows = self.file_sync()
                self.assertEqual(code, 1, rows)
                self.assertIn('Content-Range', rows[-1]['error'])
                self.assertFalse((self.output / 'payload.bin').exists())
                self.assertEqual(list(self.output.glob('*.part')), [])

    def test_truncated_responses_are_not_published_and_valid_prefix_can_resume(self):
        self.range_behavior = 'truncate'
        code, rows = self.file_sync(2000)
        self.assertEqual(code, 1, rows)
        self.assertIn('Incomplete', rows[-1]['error'])
        self.assertFalse((self.output / 'payload.bin').exists())
        offset = self.file_manifest()['partial']['size']
        self.assertEqual(offset, len(self.file_body) // 2)
        self.range_behavior = 'normal'
        code, rows = self.file_sync(2000)
        self.assertEqual(code, 0, rows)
        self.assertEqual(rows[-1]['transferredBytes'], len(self.file_body) - offset)
        self.assertEqual((self.output / 'payload.bin').read_bytes(), self.file_body)

    def test_resume_requires_strong_identity_and_unchanged_private_bytes(self):
        for behavior in ('weak', 'no-etag', 'no-length'):
            self.output = self.root / behavior
            self.range_behavior = behavior
            code, rows = self.file_sync()
            self.assertEqual(code, 1, rows)
            self.assertIn('Cannot safely resume', rows[-1]['error'])
            self.assertEqual(list(self.output.glob('*.part')), [])
            code, rows = self.file_sync(2000)
            self.assertEqual(code, 0, rows)
            self.assertEqual((self.output / 'payload.bin').read_bytes(), self.file_body)
        self.output = self.root / 'tampered'
        self.range_behavior = 'normal'
        self.assertEqual(self.file_sync()[0], 2)
        part = next(self.output.glob('*.part'))
        part.write_bytes(b'x' * 100)
        requests = len(self.requests)
        code, rows = self.file_sync()
        self.assertEqual(code, 1, rows)
        self.assertIn('modified partial', rows[-1]['error'])
        self.assertEqual(len(self.requests), requests)
        self.assertFalse((self.output / 'payload.bin').exists())

    def test_changed_range_validator_or_redirect_target_cannot_mix_bytes(self):
        for behavior in ('changed-etag', 'no-etag', 'redirect'):
            self.output = self.root / behavior
            self.range_behavior = 'normal'
            self.assertEqual(self.file_sync()[0], 2)
            self.range_behavior = behavior
            code, rows = self.file_sync()
            self.assertEqual(code, 1, rows)
            self.assertIn('identity changed', rows[-1]['error'])
            self.assertFalse((self.output / 'payload.bin').exists())
            self.assertEqual(list(self.output.glob('*.part')), [])

    def test_new_user_file_created_during_resume_is_never_replaced(self):
        self.assertEqual(self.file_sync()[0], 2)
        target = self.output / 'payload.bin'
        self.on_file_request = lambda: target.write_bytes(b'new user file')
        code, rows = self.file_sync(2000)
        self.assertEqual(code, 1, rows)
        self.assertIn('overwrite', rows[-1]['error'])
        self.assertEqual(target.read_bytes(), b'new user file')

    def test_file_partial_symlinks_and_uncommitted_appends_are_not_trusted(self):
        self.assertEqual(self.file_sync()[0], 2)
        part = next(self.output.glob('*.part'))
        with part.open('ab') as stream:
            stream.write(b'not checkpointed')
        requests = len(self.requests)
        code, rows = self.file_sync()
        self.assertEqual(code, 1, rows)
        self.assertIn('modified partial', rows[-1]['error'])
        self.assertEqual(len(self.requests), requests)
        self.assertEqual(self.file_sync()[0], 2)
        outside = self.root / 'outside.bin'
        outside.write_bytes(b'do not touch')
        part.unlink()
        part.symlink_to(outside)
        code, rows = self.file_sync()
        self.assertEqual(code, 1, rows)
        self.assertIn('Symlinks', rows[-1]['error'])
        self.assertEqual(outside.read_bytes(), b'do not touch')
        self.assertEqual(len(self.requests), requests + 1)

    def test_last_modified_only_file_sync_still_uses_304_and_checks_quota_and_edits(self):
        args = dict(mode='file', filename='style.css')
        code, rows = self.run_worker('/styles/main.css', **args)
        self.assertEqual(code, 0, rows)
        target = self.output / 'style.css'
        stamp = target.stat().st_mtime_ns
        code, rows = self.run_worker('/styles/main.css', transferLimit=0, **args)
        self.assertEqual(code, 0, rows)
        self.assertEqual(rows[-1]['status'], 304)
        self.assertIn('If-Modified-Since', self.requests[-1][1])
        self.assertNotIn('If-None-Match', self.requests[-1][1])
        self.assertEqual(target.stat().st_mtime_ns, stamp)
        code, rows = self.run_worker('/styles/main.css', maxBytes=1, **args)
        self.assertEqual(code, 1, rows)
        self.assertIn('byte budget', rows[-1]['error'])

    def test_request_identity_changes_discard_partial_without_network(self):
        self.assertEqual(self.file_sync(headers={'Authorization': 'Bearer first'})[0], 2)
        requests = len(self.requests)
        code, rows = self.file_sync(headers={'Authorization': 'Bearer second'})
        self.assertEqual(code, 1, rows)
        self.assertIn('identity', rows[-1]['error'])
        self.assertEqual(len(self.requests), requests)
        self.assertFalse((self.output / 'payload.bin').exists())

    def test_local_edits_before_and_during_resume_are_never_overwritten(self):
        self.assertEqual(self.file_sync(2000)[0], 0)
        target = self.output / 'payload.bin'
        old = target.read_bytes()
        self.version = 2
        self.file_body = b'replacement ' * 12
        self.assertEqual(self.file_sync()[0], 2)
        self.assertEqual(target.read_bytes(), old)
        target.write_bytes(b'user edit')
        requests = len(self.requests)
        code, rows = self.file_sync()
        self.assertEqual(code, 1, rows)
        self.assertIn('modified', rows[-1]['error'])
        self.assertEqual(len(self.requests), requests)
        self.assertEqual(target.read_bytes(), b'user edit')
        target.write_bytes(old)
        self.on_file_request = lambda: target.write_bytes(b'edit while downloading')
        code, rows = self.file_sync()
        self.assertEqual(code, 1, rows)
        self.assertIn('modified', rows[-1]['error'])
        self.assertEqual(target.read_bytes(), b'edit while downloading')
        target.write_bytes(old)
        self.on_file_request = None
        requests = len(self.requests)
        code, rows = self.file_sync()
        self.assertEqual(code, 0, rows)
        self.assertEqual(len(self.requests), requests)
        self.assertEqual(rows[-1]['transferredBytes'], 0)
        self.assertEqual(target.read_bytes(), self.file_body)

    def test_images_only_include_and_exclude_filters_bound_published_outputs(self):
        code, rows = self.run_worker(include=['*.png'], exclude=['img/photo*'])
        self.assertEqual(code, 0, rows)
        manifest = self.manifest()
        published = [name for name in manifest['managed'] if not name.startswith('.idm-')]
        self.assertTrue(published)
        self.assertTrue(all(name.endswith('.png') for name in published))
        self.assertNotIn(self.url + '/img/photo.png', manifest['resources'])
        self.assertFalse(any(path.startswith('/img/photo') for path, _ in self.requests))
        self.assertIn(manifest['resources'][self.url + '/img/bg.png']['path'], published)
        self.assertFalse((self.output / 'index.html').exists())
        self.assertEqual(rows[-1]['entry'], '')
        for filters in ({'include': '*.png'}, {'exclude': ['']}, {'include': ['x'] * 33}, {'include': ['x' * 257]}):
            code, rows = self.run_worker(**filters)
            self.assertEqual(code, 1, rows)

    def test_file_configuration_rejects_unsafe_names_and_modes(self):
        for filename in ('../escape', 'a/b', 'a\\b', '.idm-site.lock', '.', '', 'bad\x01'):
            code, rows = self.run_worker(mode='file', filename=filename)
            self.assertEqual(code, 1, (filename, rows))
        code, rows = self.run_worker(mode='bogus')
        self.assertEqual(code, 1)

    def test_private_job_headers_and_query_progress_redaction(self):
        file = self.root / 'job.json'
        file.write_text(json.dumps({'url': self.url + '/index', 'directory': str(self.output), 'headers': {'Cookie': 'secret'}})); file.chmod(0o644)
        result = subprocess.run([sys.executable, str(WORKER), str(file)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 1); self.assertIn('private', json.loads(result.stdout)['error'])
        code, rows = self.run_worker('/index?token=very-secret')
        self.assertEqual(code, 0, rows); self.assertNotIn('very-secret', json.dumps(rows))
        self.assertTrue(all(not '?' in row['url'] for row in rows if 'url' in row))


if __name__ == '__main__':
    unittest.main()

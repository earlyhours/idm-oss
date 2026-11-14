"""Real FTP/HTTP/authenticated SOCKS/TLS-proxy tests through the public manager API.

Run with tests/run_transfer_features. The Kerberos fixture needs MIT's kdb5_util,
kadmin.local, krb5kdc and kinit plus libgssapi_krb5, entirely under a temporary realm.
"""
import base64
import contextlib
import ctypes
import ctypes.util
import hashlib
import email.utils
import runpy
import hmac
import http.server
import json
import os
from pathlib import Path
import queue
import re
import select
import shutil
import socket
import socketserver
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.parse

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "build/transfer-tests/transfer_driver"
BODY = bytes((i * 37 + i // 251) % 256 for i in range(512 * 1024))


class Engine:
    def __init__(self, state, env=None):
        self.process = subprocess.Popen([str(DRIVER), str(state)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, env={**os.environ, **(env or {})})
        self.events = queue.Queue()
        self.latest = {}
        def read():
            for line in self.process.stdout:
                self.events.put(json.loads(line))
        self.thread = threading.Thread(target=read, daemon=True)
        self.thread.start()
        self.wait(lambda event: "downloads" in event)

    def send(self, action, **values):
        self.process.stdin.write(json.dumps(dict(action=action, **values)) + "\n")
        self.process.stdin.flush()

    def wait(self, predicate, timeout=40):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                self.latest = self.events.get(timeout=max(.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if predicate(self.latest):
                return self.latest
        raise AssertionError(f"No matching engine event; latest={self.latest}")

    def row(self, predicate=lambda row: True, timeout=40):
        event = self.wait(lambda e: e.get("downloads") and predicate(e["downloads"][0]), timeout)
        return event["downloads"][0]

    def done(self):
        return self.row(lambda row: row["status"] in ("complete", "error"))

    def close(self):
        if self.process.poll() is None:
            self.send("quit")
        self.process.wait(timeout=5)
        self.thread.join(timeout=2)
        self.process.stdin.close()
        self.process.stdout.close()
        error = self.process.stderr.read()
        self.process.stderr.close()
        if self.process.returncode and not getattr(self, "killed", False):
            raise AssertionError(error)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class Threads(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    def handle_error(self, *args):
        pass  # Interrupted data sockets are expected during pause/negative tests.


@contextlib.contextmanager
def serving(handler, **attributes):
    server = Threads(("127.0.0.1", 0), handler)
    for key, value in attributes.items():
        setattr(server, key, value)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def address(server, scheme="http", path="file.bin"):
    return f"{scheme}://127.0.0.1:{server.server_address[1]}/{path}"


class HTTP(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args):
        pass

    def send_body(self, status, body=b"", headers=None):
        self.send_response(status)
        for key, value in (headers or {}).items():
            self.send_header(key, str(value))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        for offset in range(0, len(body), 8192):
            self.wfile.write(body[offset:offset + 8192])
            self.wfile.flush()
            if getattr(self.server, "slow", False):
                time.sleep(getattr(self.server,"delay",.015))

    def do_HEAD(self):
        self.server.requests.append((self.path, dict(self.headers)))
        self.send_response(200)
        self.send_header("Content-Length", "123")
        self.end_headers()

    def do_GET(self):
        self.server.requests.append((self.path, dict(self.headers)))
        for key, value in getattr(self.server, "required_headers", {}).items():
            if self.headers.get(key) != value:
                return self.send_body(403)
        auth = getattr(self.server, "auth", "")
        authorization = self.headers.get("Authorization", "")
        if auth == "basic" and authorization != "Basic " + base64.b64encode(b"user:secret").decode():
            return self.send_body(401, headers={"WWW-Authenticate": 'Basic realm="fixture"'})
        if auth == "digest":
            fields = {}
            for match in re.finditer(r'(\w+)=(?:"([^"]*)"|([^, ]+))', authorization):
                fields[match[1]] = match[2] if match[2] is not None else match[3]
            md5 = lambda text: hashlib.md5(text.encode()).hexdigest()
            ha1 = md5("user:fixture:secret")
            ha2 = md5("GET:" + self.path)
            expected = md5(":".join((ha1, "server-nonce", fields.get("nc", ""), fields.get("cnonce", ""), "auth", ha2)))
            if (not authorization.startswith("Digest ") or fields.get("response") != expected or
                    fields.get("username") != "user" or fields.get("uri") != self.path):
                return self.send_body(401, headers={"WWW-Authenticate": 'Digest realm="fixture", nonce="server-nonce", algorithm=MD5, qop="auth"'})
        if getattr(self.server, "redirect", None):
            return self.send_body(302, headers={"Location": self.server.redirect})
        if getattr(self.server, "root", None):
            path = self.server.root / urllib.parse.unquote(urllib.parse.urlsplit(self.path).path).lstrip("/")
            body = path.read_bytes()
        else:
            body = self.server.body
        fields = {"ETag": self.server.etag, **getattr(self.server,"extra_headers",{})}
        path = urllib.parse.urlsplit(self.path).path
        fields["Content-Type"] = ("application/vnd.apple.mpegurl" if path.endswith(".m3u8") else "application/dash+xml" if path.endswith(".mpd") else "video/mp4" if path.endswith(".mp4") else "application/octet-stream")
        byte_range = self.headers.get("Range", "")
        if byte_range and not getattr(self.server, "ignore_range", False):
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", byte_range)
            if not match:
                return self.send_body(400)
            first, last = int(match[1]), int(match[2]) if match[2] else len(body) - 1
            if first >= len(body):
                return self.send_body(416, headers={"Content-Range": f"bytes */{len(body)}"})
            if first > 0 and getattr(self.server, "bad_resume", "") == "ignored":
                return self.send_body(200, body, fields)
            fields["Content-Range"] = f"bytes {first}-{last}/{len(body)}"
            if first > 0 and getattr(self.server, "bad_resume", "") == "range":
                fields["Content-Range"] = f"bytes {first + 1}-{last}/{len(body)}"
            if first > 0 and getattr(self.server, "bad_resume", "") == "encoding":
                fields["Content-Encoding"] = "gzip"
            return self.send_body(206, body[first:last + 1], fields)
        return self.send_body(200, body, fields)


class NTLM(HTTP):
    def do_GET(self):
        if getattr(self, "authenticated", False):
            return super().do_GET()
        authorization = self.headers.get("Authorization", "")
        if not authorization.startswith("NTLM "):
            return self.send_body(401, headers={"WWW-Authenticate": "NTLM"})
        token = base64.b64decode(authorization[5:])
        if token[:8] != b"NTLMSSP\0":
            return self.send_body(403)
        kind = struct.unpack_from("<I", token, 8)[0]
        if kind == 1:
            domain = "fixture".encode("utf-16le")
            info = struct.pack("<HH", 2, len(domain)) + domain + b"\0" * 4
            challenge = (b"NTLMSSP\0" + struct.pack("<IHHII", 2, len(domain), len(domain), 48, 0x00888205) +
                         b"12345678" + b"\0" * 8 + struct.pack("<HHI", len(info), len(info), 48 + len(domain)) + domain + info)
            return self.send_body(401, headers={"WWW-Authenticate": "NTLM " + base64.b64encode(challenge).decode()})
        if kind != 3:
            return self.send_body(403)
        def field(offset):
            length, _, start = struct.unpack_from("<HHI", token, offset)
            return token[start:start + length]
        domain, user = field(28).decode("utf-16le"), field(36).decode("utf-16le")
        response = field(20)
        nt_hash = subprocess.check_output(["openssl", "dgst", "-provider", "legacy", "-md4", "-binary"], input="secret".encode("utf-16le"))
        key = hmac.digest(nt_hash, (user.upper() + domain).encode("utf-16le"), "md5")
        if user != "user" or not hmac.compare_digest(response[:16], hmac.digest(key, b"12345678" + response[16:], "md5")):
            return self.send_body(403)
        self.server.verified += 1
        self.authenticated = True
        super().do_GET()


class GSSBuffer(ctypes.Structure):
    _fields_ = [("length", ctypes.c_size_t), ("value", ctypes.c_void_p)]


class Negotiate(HTTP):
    def do_GET(self):
        authorization = self.headers.get("Authorization", "")
        if not authorization.startswith("Negotiate "):
            return self.send_body(401, headers={"WWW-Authenticate": "Negotiate"})
        token = base64.b64decode(authorization.split()[1])
        storage = ctypes.create_string_buffer(token)
        incoming = GSSBuffer(len(token), ctypes.cast(storage, ctypes.c_void_p))
        output, minor = GSSBuffer(), ctypes.c_uint32()
        context, peer = ctypes.c_void_p(), ctypes.c_void_p()
        gss = self.server.gss
        major = gss.gss_accept_sec_context(ctypes.byref(minor), ctypes.byref(context), None, ctypes.byref(incoming), None,
                                          ctypes.byref(peer), None, ctypes.byref(output), None, None, None)
        try:
            if major != 0:
                return self.send_body(403)
            name = GSSBuffer()
            gss.gss_display_name(ctypes.byref(minor), peer, ctypes.byref(name), None)
            try:
                self.server.peers.append(ctypes.string_at(name.value, name.length).decode())
            finally:
                gss.gss_release_buffer(ctypes.byref(minor), ctypes.byref(name))
            self.response_token = base64.b64encode(ctypes.string_at(output.value, output.length)).decode() if output.length else ""
            super().do_GET()
        finally:
            gss.gss_release_buffer(ctypes.byref(minor), ctypes.byref(output))
            if peer:
                gss.gss_release_name(ctypes.byref(minor), ctypes.byref(peer))
            if context:
                gss.gss_delete_sec_context(ctypes.byref(minor), ctypes.byref(context), None)

    def send_body(self, status, body=b"", headers=None):
        headers = dict(headers or {})
        if getattr(self, "response_token", ""):
            headers["WWW-Authenticate"] = "Negotiate " + self.response_token
        super().send_body(status, body, headers)


@contextlib.contextmanager
def kerberos_realm(directory):
    directory.mkdir(mode=0o700)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    config, profile = directory / "krb5.conf", directory / "kdc.conf"
    config.write_text(f"""[libdefaults]
 default_realm = IDM.TEST
 dns_lookup_kdc = false
 dns_lookup_realm = false
 rdns = false
 dns_canonicalize_hostname = false
 qualify_shortname = ""
[domain_realm]
 localhost = IDM.TEST
[realms]
 IDM.TEST = {{
  kdc = 127.0.0.1:{port}
 }}
""")
    profile.write_text(f"""[kdcdefaults]
 kdc_ports = {port}
 kdc_tcp_ports = {port}
[realms]
 IDM.TEST = {{
  database_name = {directory}/principal
  key_stash_file = {directory}/stash
  acl_file = {directory}/kadm5.acl
 }}
""")
    env = dict(KRB5_CONFIG=str(config), KRB5_KDC_PROFILE=str(profile), KRB5CCNAME="FILE:" + str(directory / "ccache"), KRB5_KTNAME=str(directory / "http.keytab"))
    def command(program, *args, **kwargs):
        return subprocess.run([program, *args], env={**os.environ, **env}, capture_output=True, timeout=10, **kwargs)
    result = command("kdb5_util", "create", "-s", "-P", "fixture-master")
    if result.returncode:
        raise AssertionError(result.stderr.decode())
    for query in ('addprinc -pw fixture-password user', 'addprinc -randkey HTTP/localhost', f'ktadd -k {directory}/http.keytab HTTP/localhost'):
        result = command("kadmin.local", "-q", query)
        if result.returncode:
            raise AssertionError(result.stderr.decode())
    kdc = subprocess.Popen(["krb5kdc", "-n", "-r", "IDM.TEST", "-P", str(directory / "kdc.pid")], env={**os.environ, **env}, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    original = {key: os.environ.get(key) for key in env}
    try:
        for _ in range(40):
            result = command("kinit", "user@IDM.TEST", input=b"fixture-password\n")
            if result.returncode == 0:
                break
            time.sleep(.05)
        if result.returncode:
            raise AssertionError(result.stderr.decode())
        os.environ.update(env)
        gss = ctypes.CDLL(ctypes.util.find_library("gssapi_krb5"))
        gss.gss_accept_sec_context.restype = ctypes.c_uint32
        yield env, gss
    finally:
        for key, value in original.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        kdc.terminate()
        kdc.wait(timeout=5)


class TLSHTTP(HTTP):
    def setup(self):
        self.request = self.server.context.wrap_socket(self.request, server_side=True, do_handshake_on_connect=False)
        try:
            self.request.do_handshake()
        except Exception:
            self.request.close()
            raise
        super().setup()

    def finish(self):
        try:
            super().finish()
        finally:
            self.request.close()


class FTP(socketserver.StreamRequestHandler):
    def handle(self):
        def reply(text):
            self.wfile.write((text + "\r\n").encode())
        reply("220 fixture")
        passive, offset = None, 0
        try:
            for raw in self.rfile:
                command, _, argument = raw.decode().strip().partition(" ")
                self.server.commands.append((command, argument))
                if command == "USER":
                    reply("331 password")
                elif command == "PASS":
                    reply("230 logged in" if argument == "secret" else "530 forbidden")
                elif command == "PWD":
                    reply('257 "/"')
                elif command in ("TYPE", "CWD"):
                    reply("200 ok")
                elif command == "SIZE":
                    reply("213 " + str(len(self.server.body)) if getattr(self.server, "size", True) else "500 unsupported")
                elif command == "MDTM":
                    reply("213 " + self.server.modified if self.server.modified else "500 unsupported")
                elif command == "REST":
                    if self.server.rest:
                        offset = int(argument)
                        reply("350 restart accepted")
                    else:
                        reply("502 REST unsupported")
                elif command == "EPSV":
                    if passive:
                        passive.close()
                    passive = socket.socket()
                    passive.bind(("127.0.0.1", 0))
                    passive.listen()
                    passive.settimeout(5)
                    reply(f"229 entering (|||{passive.getsockname()[1]}|)")
                elif command == "RETR":
                    reply("150 opening")
                    data, _ = passive.accept()
                    with data:
                        body = self.server.body[offset:]
                        for index in range(0, len(body), 8192):
                            data.sendall(body[index:index + 8192])
                            if self.server.slow:
                                time.sleep(.015)
                    offset = 0
                    reply("226 done")
                elif command == "QUIT":
                    reply("221 bye")
                    break
                else:
                    reply("500 unsupported")
        finally:
            if passive:
                passive.close()


def exact(sock, length):
    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise EOFError()
        data += chunk
    return data


def relay(client, remote):
    with remote:
        while True:
            readers, _, _ = select.select([client, remote], [], [], 10)
            if not readers:
                return
            for reader in readers:
                data = reader.recv(65536)
                if not data:
                    return
                (remote if reader is client else client).sendall(data)


class SOCKS(socketserver.BaseRequestHandler):
    def handle(self):
        client = self.request
        version, count = exact(client, 2)
        assert version == 5 and 2 in exact(client, count)
        client.sendall(b"\x05\x02")
        assert exact(client, 1) == b"\x01"
        user = exact(client, exact(client, 1)[0])
        password = exact(client, exact(client, 1)[0])
        self.server.credentials.append((user, password))
        assert (user, password) == (b"proxy-user", b"proxy-secret")
        client.sendall(b"\x01\x00")
        version, command, _, kind = exact(client, 4)
        assert version == 5 and command == 1
        host = socket.inet_ntoa(exact(client, 4)) if kind == 1 else exact(client, exact(client, 1)[0]).decode() if kind == 3 else socket.inet_ntop(socket.AF_INET6, exact(client, 16))
        port = int.from_bytes(exact(client, 2), "big")
        self.server.targets.append((host, port))
        remote = socket.create_connection(("127.0.0.1" if host == "fixture.invalid" else host, port))
        client.sendall(b"\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00")
        relay(client, remote)


class TLSProxy(socketserver.StreamRequestHandler):
    def finish(self):
        try:
            super().finish()
        finally:
            self.request.close()

    def setup(self):
        self.request = self.server.context.wrap_socket(self.request, server_side=True, do_handshake_on_connect=False)
        try:
            self.request.do_handshake()
        except Exception:
            self.request.close()
            raise
        super().setup()

    def handle(self):
        request = self.rfile.readline()
        headers = {}
        while line := self.rfile.readline().strip():
            key, value = line.decode().split(":", 1)
            headers[key.lower()] = value.strip()
        self.server.requests.append((request, headers))
        expected = "Basic " + base64.b64encode(b"proxy-user:proxy-secret").decode()
        authorized = headers.get("proxy-authorization") == expected
        challenge = 'Basic realm="proxy"'
        if getattr(self.server, "auth", "") == "digest":
            challenge = 'Digest realm="proxy", nonce="proxy-nonce", algorithm=MD5, qop="auth"'
            fields = {}
            for match in re.finditer(r'(\w+)=(?:"([^"]*)"|([^, ]+))', headers.get("proxy-authorization", "")):
                fields[match[1]] = match[2] if match[2] is not None else match[3]
            md5 = lambda text: hashlib.md5(text.encode()).hexdigest()
            method, target, _ = request.decode().split()
            parsed = urllib.parse.urlsplit(target)
            path = (parsed.path or "/") + (("?" + parsed.query) if parsed.query else "")
            uri = fields.get("uri", "")
            expected = md5(":".join((md5("proxy-user:proxy:proxy-secret"), "proxy-nonce", fields.get("nc", ""), fields.get("cnonce", ""), "auth", md5(method + ":" + uri))))
            authorized = fields.get("response") == expected and fields.get("username") == "proxy-user" and uri in (target, path)
        if not authorized:
            self.wfile.write(("HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: " + challenge + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").encode())
            return
        method, target, _ = request.decode().split()
        if method == "CONNECT":
            host, port = target.rsplit(":", 1)
            remote = socket.create_connection((host, int(port)))
            self.wfile.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
            self.wfile.flush()
        else:
            url = urllib.parse.urlsplit(target)
            remote = socket.create_connection((url.hostname, url.port or 80))
            forwarded = f"{method} {url.path or '/'} HTTP/1.1\r\n"
            forwarded += "".join(f"{key}: {value}\r\n" for key, value in headers.items() if key not in ("proxy-authorization", "proxy-connection", "connection"))
            remote.sendall((forwarded + "Connection: close\r\n\r\n").encode())
        relay(self.request, remote)


class TransferFeatures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.certificates = tempfile.TemporaryDirectory(prefix="idm-transfer-ca-")
        cls.ca = Path(cls.certificates.name)
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(cls.ca / "key.pem"), "-out", str(cls.ca / "ca.pem"), "-subj", "/CN=127.0.0.1", "-days", "1", "-addext", "subjectAltName=IP:127.0.0.1"], check=True, capture_output=True)
        cls.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cls.context.load_cert_chain(cls.ca / "ca.pem", cls.ca / "key.pem")
        cls.media = tempfile.TemporaryDirectory(prefix="idm-transfer-media-")
        cls.media_root = Path(cls.media.name)
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x90:rate=12", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "12", "-c:a", "aac", "-shortest", str(cls.media_root / "direct.mp4")], check=True)
        for options, filename in [(["-hls_time", "1", "-hls_playlist_type", "vod"], "stream.m3u8"), (["-f", "dash", "-seg_duration", "1"], "stream.mpd")]:
            subprocess.run(["ffmpeg", "-v", "error", "-i", str(cls.media_root / "direct.mp4"), "-c", "copy", *options, str(cls.media_root / filename)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.media.cleanup()
        cls.certificates.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="idm-transfer-test-")
        self.root = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)

    def request(self, server, **extra):
        return dict(url=address(server), directory=str(self.root / "out"), **extra)

    def complete(self, row, body=BODY):
        self.assertEqual(row["status"], "complete", row)
        if body is not None:
            self.assertEqual(Path(row["path"]).read_bytes(), body)
        self.assertNotIn("password", row)
        self.assertNotIn("username", row)


    def frame_hashes(self, path):
        return subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:v", "-f", "framemd5", "-"]).decode().split("#stream#, dts,        pts, duration,     size, hash")[-1]



    def test_http_auth_choice_is_rejected_for_ftp(self):
        with Engine(self.root / "state") as engine:
            request = dict(url="ftp://127.0.0.1:9/file.bin", directory=str(self.root / "out"), start=False)
            engine.send("add", request=dict(request, authMethod="digest"))
            event = engine.wait(lambda e: bool(e["error"]))
            self.assertEqual(event["downloads"], [])
            self.assertIn("do not apply to FTP", event["error"])
            engine.send("add", request=request)
            row = engine.row()
            engine.send("context", id=row["id"], request=dict(authMethod="digest"))
            event = engine.wait(lambda e: bool(e["error"]))
            self.assertIn("do not apply to FTP", event["error"])
            self.assertEqual(event["downloads"][0]["authMethod"], "auto")

    def test_legacy_mms_is_explicitly_unavailable_in_manager(self):
        for scheme in ("mms", "mmsh", "mmst"):
            with self.subTest(scheme=scheme), Engine(self.root / scheme) as engine:
                engine.send("add", request=dict(url=f"{scheme}://127.0.0.1:9/test.asf", directory=str(self.root / "out")))
                event = engine.wait(lambda e: bool(e["error"]))
                self.assertEqual(event["downloads"], [])
                self.assertIn("HTTP, HTTPS, FTP or FTPS", event["error"])

    def test_auth_choice_validation_persistence_and_update(self):
        with serving(HTTP, requests=[], body=BODY, etag='"v1"', auth="digest") as server:
            with Engine(self.root / "state") as engine:
                engine.send("add", request=self.request(server, authMethod="digest", username="user", password="old", start=False))
                row = engine.row()
                engine.send("context", id=row["id"], request=dict(authMethod="bogus"))
                engine.wait(lambda e: "Choose Auto" in e["error"])
                engine.send("context", id=row["id"], request=dict(authMethod="digest", password="secret"))
                engine.wait(lambda e: not e["error"])
            with Engine(self.root / "state") as engine:
                self.assertEqual(engine.latest["downloads"][0]["authMethod"], "digest")
                engine.send("resume", id=row["id"])
                self.complete(engine.done())
            self.assertEqual((self.root / "state/queue.json").stat().st_mode & 0o777, 0o600)

    def test_invalid_resume_responses_preserve_validated_prefix(self):
        for bad in ("ignored", "range", "encoding"):
            with self.subTest(response=bad), serving(HTTP, requests=[], body=BODY, etag='"v1"', slow=True) as server, Engine(self.root / bad) as engine:
                engine.send("add", request=self.request(server, authMethod="basic"))
                row = engine.row(lambda r: r["received"] > 16384)
                engine.send("pause", id=row["id"])
                engine.row(lambda r: r["status"] == "paused")
                server.bad_resume = bad
                engine.send("resume", id=row["id"])
                result = engine.done()
                self.assertEqual(result["status"], "error")
                self.assertFalse(Path(result["path"]).exists())
                partial = self.root / bad / row["id"] / "transfer/partial.bin"
                self.assertGreater(partial.stat().st_size, 0)
                self.assertEqual(partial.read_bytes(), BODY[:partial.stat().st_size])
                server.bad_resume = ""
                engine.send("resume", id=row["id"])
                self.complete(engine.done())

    def test_https_source_over_https_proxy_validates_both_hostnames(self):
        for wrong in ("", "proxy", "source"):
            with self.subTest(wrong=wrong), serving(TLSHTTP, context=self.context, requests=[], body=BODY, etag='"v1"') as server, serving(TLSProxy, context=self.context, requests=[]) as proxy, Engine(self.root / (wrong or "valid"), {"CURL_CA_BUNDLE": str(self.ca / "ca.pem")}) as engine:
                url = address(server, "https").replace("127.0.0.1", "localhost") if wrong == "source" else address(server, "https")
                engine.send("add", request=dict(url=url, directory=str(self.root / "out"), proxy=dict(type="https", host="localhost" if wrong == "proxy" else "127.0.0.1", port=proxy.server_address[1], username="proxy-user", password="proxy-secret")))
                row = engine.done()
                if wrong:
                    self.assertEqual(row["status"], "error")
                    self.assertFalse(Path(row["path"]).exists())
                    self.assertFalse(server.requests)
                else:
                    self.complete(row)
                    self.assertTrue(any(request.startswith(b"CONNECT ") for request, _ in proxy.requests))

    def test_media_socks_drops_context_at_cross_origin_segments(self):
        root = self.root / "media"
        shutil.copytree(self.media_root, root)
        secret = {"Cookie": "session=private", "Authorization": "Bearer private", "X-Api-Key": "private"}
        with serving(HTTP, requests=[], body=BODY, etag='"v1"', root=self.media_root) as segments:
            playlist = (root / "stream.m3u8").read_text()
            playlist = re.sub(r"(?m)^([^#\n].*\.ts)$", lambda match: address(segments, path=match[1]), playlist)
            (root / "stream.m3u8").write_text(playlist)
            with serving(HTTP, requests=[], body=BODY, etag='"v1"', root=root, required_headers=secret) as server, serving(SOCKS, credentials=[], targets=[]) as proxy, Engine(self.root / "state") as engine:
                engine.send("add", request=dict(self.request(server, media=True, headers=secret, proxy=dict(type="socks5", host="127.0.0.1", port=proxy.server_address[1], username="proxy-user", password="proxy-secret")), url=address(server, path="stream.m3u8")))
                row = engine.done()
                self.complete(row, None)
                self.assertTrue(segments.requests)
                for _, headers in segments.requests:
                    self.assertFalse(set(k.lower() for k in headers) & set(k.lower() for k in secret))
                self.assertEqual(self.frame_hashes(row["path"]), self.frame_hashes(self.media_root / "direct.mp4"))

    def test_basic_digest_and_qt_auto_digest_are_real_challenges(self):
        for method in ("basic", "digest", "auto"):
            with self.subTest(method=method), serving(HTTP, requests=[], body=BODY, etag='"v1"', auth="digest" if method == "auto" else method) as server, Engine(self.root / method) as engine:
                engine.send("add", request=self.request(server, authMethod=method, username="user", password="secret"))
                self.complete(engine.done())
                self.assertTrue(any(h.get("Authorization", "").startswith("Digest ") for _, h in server.requests) if method != "basic" else any(h.get("Authorization", "").startswith("Basic ") for _, h in server.requests))


    def test_qt_ntlmv2_proof_and_explicit_curl_build_capability(self):
        for method in ("auto", "ntlm"):
            with self.subTest(method=method), serving(NTLM, requests=[], body=BODY, etag='"v1"', verified=0) as server, Engine(self.root / method) as engine:
                engine.send("add", request=self.request(server, authMethod=method, username="fixture\\user", password="secret"))
                row = engine.done()
                features = subprocess.check_output(["curl", "-V"], text=True).lower().split("features:")[-1].split()
                if method == "ntlm" and "ntlm" not in features:
                    self.assertEqual(row["status"], "error")
                    self.assertEqual(row["error"], "The installed curl build does not support NTLM.")
                else:
                    self.complete(row)
                    self.assertGreater(server.verified, 0)

    def test_explicit_ntlm_does_not_send_basic_credentials(self):
        with serving(HTTP, requests=[], body=BODY, etag='"v1"', auth="basic") as server, Engine(self.root / "state") as engine:
            engine.send("add", request=self.request(server, authMethod="ntlm", username="user", password="secret"))
            self.assertEqual(engine.done()["status"], "error")
            self.assertFalse(any(h.get("Authorization", "").startswith("Basic ") for _, h in server.requests))


    def test_negotiate_and_kerberos_use_real_isolated_kdc_tickets(self):
        with kerberos_realm(self.root / "realm") as (env, gss):
            for method in ("negotiate", "kerberos"):
                with self.subTest(method=method), serving(Negotiate, requests=[], peers=[], gss=gss, body=BODY, etag='"v1"') as server, Engine(self.root / method, env) as engine:
                    engine.send("add", request=dict(self.request(server, authMethod=method), url=address(server).replace("127.0.0.1", "localhost")))
                    self.complete(engine.done())
                    self.assertTrue(server.peers)
                    self.assertEqual(set(server.peers), {"user@IDM.TEST"})

    def test_selected_digest_does_not_downgrade_to_basic(self):
        with serving(HTTP, requests=[], body=BODY, etag='"v1"', auth="basic") as server, Engine(self.root / "state") as engine:
            engine.send("add", request=self.request(server, authMethod="digest", username="user", password="secret"))
            row = engine.done()
            self.assertEqual(row["status"], "error")
            self.assertFalse(Path(row["path"]).exists())
            self.assertFalse(any(h.get("Authorization", "").startswith("Basic ") for _, h in server.requests))

    def test_auth_redirect_is_blocked_before_other_origin(self):
        with serving(HTTP, requests=[], body=BODY, etag='"v1"') as target, serving(HTTP, requests=[], body=BODY, etag='"v1"', auth="digest", redirect=address(target)) as server, Engine(self.root / "state") as engine:
            engine.send("add", request=self.request(server, authMethod="digest", username="user", password="secret"))
            self.assertEqual(engine.done()["status"], "error")
            self.assertEqual(target.requests, [])

    def test_ftp_pause_restart_uses_rest_and_verifies_full_content(self):
        with serving(FTP, commands=[], body=BODY, modified="20260101000000", slow=True, rest=True) as server:
            with Engine(self.root / "state") as engine:
                engine.send("add", request=dict(self.request(server, username="user", password="secret"), url=address(server, "ftp")))
                row = engine.row(lambda r: r["received"] > 16384)
                engine.send("pause", id=row["id"])
                engine.row(lambda r: r["status"] == "paused")
            with Engine(self.root / "state") as engine:
                engine.send("resume", id=row["id"])
                self.complete(engine.done())
            self.assertTrue(any(cmd == "REST" and int(arg) > 0 for cmd, arg in server.commands))
            self.assertGreaterEqual(sum(cmd == "RETR" for cmd, _ in server.commands), 3)

    def ftp_changed(self, coarse=False, no_rest=False, no_mdtm=False):
        with serving(FTP, commands=[], body=BODY, modified="" if no_mdtm else "20260101000000", slow=True, rest=not no_rest) as server, Engine(self.root / "state") as engine:
            engine.send("add", request=dict(self.request(server, username="user", password="secret"), url=address(server, "ftp")))
            row = engine.row(lambda r: r["received"] > 16384)
            engine.send("pause", id=row["id"])
            engine.row(lambda r: r["status"] == "paused")
            partial = self.root / "state" / row["id"] / "transfer/partial.bin"
            before = partial.read_bytes()
            if not no_rest:
                server.body = b"X" * len(BODY)
                if not coarse and not no_mdtm:
                    server.modified = "20260102000000"
            engine.send("resume", id=row["id"])
            result = engine.done()
            if no_mdtm:
                self.complete(result, server.body)
                self.assertFalse(any(cmd == "REST" and arg != "0" for cmd, arg in server.commands))
            else:
                self.assertEqual(result["status"], "error")
                self.assertFalse(Path(result["path"]).exists())
                self.assertEqual(partial.read_bytes()[:len(before)], before)


    def test_manager_crash_stops_ftp_writer_before_restart(self):
        with serving(FTP, commands=[], body=BODY, modified="20260101000000", slow=True, rest=True) as server:
            with Engine(self.root / "state") as engine:
                engine.send("add", request=dict(self.request(server, username="user", password="secret"), url=address(server, "ftp")))
                row = engine.row(lambda r: r["received"] > 16384)
                child_file = Path(f"/proc/{engine.process.pid}/task/{engine.process.pid}/children")
                children = child_file.read_text().split()
                self.assertTrue(children)
                engine.killed = True
                engine.process.kill()
                engine.process.wait(timeout=5)
                for _ in range(100):
                    alive = [pid for pid in children if Path(f"/proc/{pid}/stat").exists() and Path(f"/proc/{pid}/stat").read_text().split()[2] != "Z"]
                    if not alive:
                        break
                    time.sleep(.01)
                self.assertFalse(alive)
            with Engine(self.root / "state") as engine:
                engine.send("resume", id=row["id"])
                self.complete(engine.done())


    def test_ftp_without_size_can_still_download(self):
        with serving(FTP, commands=[], body=BODY, modified="20260101000000", slow=False, rest=True, size=False) as server, Engine(self.root / "state") as engine:
            engine.send("add", request=dict(self.request(server, username="user", password="secret"), url=address(server, "ftp")))
            self.complete(engine.done())

    def test_media_hls_uses_encrypted_https_proxy(self):
        with serving(HTTP, requests=[], body=BODY, etag='"v1"', root=self.media_root) as server, serving(TLSProxy, context=self.context, requests=[]) as proxy, Engine(self.root / "state", {"CURL_CA_BUNDLE": str(self.ca / "ca.pem")}) as engine:
            engine.send("add", request=dict(self.request(server, media=True, proxy=dict(type="https", host="127.0.0.1", port=proxy.server_address[1], username="proxy-user", password="proxy-secret")), url=address(server, path="stream.m3u8")))
            row = engine.done()
            self.complete(row, None)
            self.assertEqual(self.frame_hashes(row["path"]), self.frame_hashes(self.media_root / "direct.mp4"))
            self.assertTrue(proxy.requests)
            self.assertFalse(any("proxy-authorization" in {k.lower() for k in h} for _, h in server.requests))

    def test_ftp_changed_mdtm_preserves_partial(self):
        self.ftp_changed()

    def test_ftp_same_size_same_mdtm_different_content_never_publishes(self):
        self.ftp_changed(coarse=True)

    def test_ftp_rest_refusal_preserves_partial(self):
        self.ftp_changed(no_rest=True)

    def test_ftp_without_mdtm_restarts_safely(self):
        self.ftp_changed(no_mdtm=True)

    def test_curl_fetches_literal_bracket_url_without_glob_expansion(self):
        source=self.root/"source";source.mkdir()
        for name, body in [("file[1-2].bin",b"literal resource"),("file1.bin",b"wrong one"),("file2.bin",b"wrong two")]:
            (source/name).write_bytes(body)
        with serving(HTTP,requests=[],body=BODY,etag='"v1"',root=source) as server, Engine(self.root/"state") as engine:
            engine.send("add",request=dict(self.request(server,authMethod="basic"),url=address(server,path="file[1-2].bin")))
            self.complete(engine.done(),b"literal resource")
            self.assertTrue(server.requests)
            self.assertTrue(all(urllib.parse.unquote(urllib.parse.urlsplit(path).path)=="/file[1-2].bin" for path,_ in server.requests))

    def test_recent_last_modified_cannot_mix_versions_in_either_transport(self):
        for method in ("auto","basic"):
            with self.subTest(method=method), serving(HTTP,requests=[],body=b"A"*len(BODY),etag="",slow=True,delay=.04,
                    extra_headers={"Last-Modified":email.utils.formatdate(time.time(),usegmt=True)}) as server:
                state=self.root/method
                with Engine(state) as engine:
                    engine.send("add",request=self.request(server,authMethod=method,connections=1))
                    row=engine.row(lambda r:r["received"]>16384 and r["status"]=="downloading")
                    engine.send("pause",id=row["id"]);engine.row(lambda r:r["status"]=="paused")
                server.body=b"B"*len(BODY)
                with Engine(state) as engine:
                    engine.send("resume",id=row["id"])
                    self.complete(engine.done(),b"B"*len(BODY))

    def test_aged_last_modified_can_resume_both_transports(self):
        for method in ("auto","basic"):
            with self.subTest(method=method), serving(HTTP,requests=[],body=BODY,etag="",slow=True,delay=.04,
                    extra_headers={"Last-Modified":email.utils.formatdate(time.time()-3600,usegmt=True)}) as server:
                state=self.root/method
                with Engine(state) as engine:
                    engine.send("add",request=self.request(server,authMethod=method,connections=1))
                    row=engine.row(lambda r:r["received"]>16384 and r["status"]=="downloading")
                    self.assertTrue(row["resumable"])
                    engine.send("pause",id=row["id"]);engine.row(lambda r:r["status"]=="paused")
                with Engine(state) as engine:
                    engine.send("resume",id=row["id"]);self.complete(engine.done())
                self.assertTrue(any(int(re.match(r"bytes=(\d+)-",h["Range"])[1])>0 for _,h in server.requests if h.get("Range")))

    def test_legacy_date_partials_without_strength_evidence_are_not_adopted(self):
        for method in ("auto","basic"):
            with self.subTest(method=method), serving(HTTP,requests=[],body=BODY,etag="",slow=True,delay=.04,
                    extra_headers={"Last-Modified":email.utils.formatdate(time.time()-3600,usegmt=True)}) as server:
                state=self.root/method
                with Engine(state) as engine:
                    engine.send("add",request=self.request(server,authMethod=method,connections=1))
                    row=engine.row(lambda r:r["received"]>16384 and r["status"]=="downloading")
                    engine.send("pause",id=row["id"]);engine.row(lambda r:r["status"]=="paused")
                if method=="auto":
                    path=state/"queue.json";record=json.loads(path.read_text())
                    record["downloads"][0].pop("dateValidatorStrong")
                else:
                    path=state/row["id"]/"transfer/resume.json";record=json.loads(path.read_text());record.pop("dateStrong")
                path.write_text(json.dumps(record))
                with Engine(state) as engine:
                    engine.send("resume",id=row["id"]);result=engine.done()
                    self.assertEqual(result["status"],"error")
                    self.assertFalse(Path(result["path"]).exists())
                    self.assertIn("date-validated" if method == "auto" else "saved resource validators",result["error"])

    def test_curl_head_completes_without_waiting_for_advertised_body(self):
        worker=runpy.run_path(str(ROOT/"src/mediaworker.py"))
        with serving(HTTP,requests=[],body=BODY,etag='"v1"') as server:
            config=dict(url=address(server),directory=str(self.root),curl=shutil.which("curl"),timeoutSeconds=1)
            start=time.monotonic()
            status,fields,body,_=worker["CurlHTTP"](config,{}).fetch(address(server),method="HEAD")
            with body:
                self.assertEqual(body.read(),b"")
            self.assertEqual(status,200)
            self.assertEqual(fields["Content-Length"],"123")
            self.assertLess(time.monotonic()-start,1)

    def test_curl_http_pause_restart_and_changed_validator(self):
        for changed in (False, True):
            with self.subTest(changed=changed), serving(HTTP, requests=[], body=BODY, etag='"v1"', auth="digest", slow=True) as server:
                state = self.root / str(changed)
                with Engine(state) as engine:
                    engine.send("add", request=self.request(server, authMethod="digest", username="user", password="secret"))
                    row = engine.row(lambda r: r["received"] > 16384)
                    engine.send("pause", id=row["id"])
                    engine.row(lambda r: r["status"] == "paused")
                if changed:
                    server.etag = '"changed"'
                    server.body = b"X" * len(BODY)
                with Engine(state) as engine:
                    engine.send("resume", id=row["id"])
                    result = engine.done()
                    if changed:
                        self.assertEqual(result["status"], "error")
                        self.assertFalse(Path(result["path"]).exists())
                    else:
                        self.complete(result)
                        self.assertTrue(any(int(re.match(r"bytes=(\d+)-", h["Range"])[1]) > 0 for _, h in server.requests if h.get("Range")))

    def test_media_socks_direct_hls_and_dash(self):
        for path in ("direct.mp4", "stream.m3u8", "stream.mpd"):
            with self.subTest(path=path), serving(HTTP, requests=[], body=BODY, etag='"v1"', root=self.media_root) as server, serving(SOCKS, credentials=[], targets=[]) as proxy, Engine(self.root / path) as engine:
                proxy_settings = dict(type="socks5", host="127.0.0.1", port=proxy.server_address[1], username="proxy-user", password="proxy-secret")
                engine.send("add", request=dict(self.request(server, media=True, proxy=proxy_settings), url=address(server, path=path).replace("127.0.0.1", "fixture.invalid")))
                row = engine.done()
                self.complete(row, None)
                metadata = subprocess.check_output(["ffprobe", "-v", "error", "-show_format", "-of", "json", row["path"]])
                self.assertGreater(float(json.loads(metadata)["format"]["duration"]), 0)
                self.assertEqual(self.frame_hashes(row["path"]), self.frame_hashes(self.media_root / "direct.mp4"))
                self.assertTrue(proxy.credentials)
                self.assertTrue(all(host == "fixture.invalid" for host, _ in proxy.targets))
                self.assertTrue(server.requests)
                self.assertFalse(any("Proxy-Authorization" in h for _, h in server.requests))


    def test_https_proxy_digest_is_independent_of_origin_digest(self):
        with serving(HTTP, requests=[], body=BODY, etag='"v1"', auth="digest") as server, serving(TLSProxy, context=self.context, requests=[], auth="digest") as proxy, Engine(self.root / "state", {"CURL_CA_BUNDLE": str(self.ca / "ca.pem")}) as engine:
            engine.send("add", request=self.request(server, authMethod="digest", username="user", password="secret", proxy=dict(type="https", host="127.0.0.1", port=proxy.server_address[1], username="proxy-user", password="proxy-secret")))
            self.complete(engine.done())
            self.assertTrue(any(h.get("proxy-authorization", "").startswith("Digest ") for _, h in proxy.requests))
            self.assertFalse(any("proxy-authorization" in {k.lower() for k in h} for _, h in server.requests))

    def test_https_proxy_trust_and_auth_are_separate_from_source(self):
        for trusted in (True, False):
            with self.subTest(trusted=trusted), serving(HTTP, requests=[], body=BODY, etag='"v1"', auth="digest") as server, serving(TLSProxy, context=self.context, requests=[]) as proxy, Engine(self.root / str(trusted), {"CURL_CA_BUNDLE": str(self.ca / "ca.pem")} if trusted else {}) as engine:
                engine.send("add", request=self.request(server, authMethod="digest", username="user", password="secret", proxy=dict(type="https", host="127.0.0.1", port=proxy.server_address[1], username="proxy-user", password="proxy-secret")))
                row = engine.done()
                if trusted:
                    self.complete(row)
                    self.assertTrue(any("proxy-authorization" in h for _, h in proxy.requests))
                    self.assertTrue(all("Proxy-Authorization" not in h for _, h in server.requests))
                    self.assertTrue(any({k.lower(): v for k, v in h.items()}.get("authorization", "").startswith("Digest ") for _, h in server.requests))
                else:
                    self.assertEqual(row["status"], "error")
                    self.assertFalse(server.requests)
                    self.assertFalse(Path(row["path"]).exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)

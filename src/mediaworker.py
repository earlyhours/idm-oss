"""Private yt-dlp worker. Network context is scoped to the captured URL's origin."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import time
import base64
import ctypes
import email.parser
import re
import tempfile
import importlib
import shutil
import zipfile


def emit(**frame):
    print(json.dumps(frame, separators=(",", ":")), flush=True)


def origin(url):
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in ("http", "https") or not parts.hostname or parts.username or parts.password:
        raise ValueError("Unsupported media URL")
    return parts.scheme, parts.hostname.lower(), parts.port or (443 if parts.scheme == "https" else 80)


def scope_headers(headers, target, source_origin, context):
    # Extractors may carry cookies between formats. Only the captured origin gets context.
    blocked = {"cookie", "authorization", "proxy-authorization", "referer", "origin"} | {key.lower() for key in context}
    result = {str(k): str(v) for k, v in headers.items() if k.lower() not in blocked}
    if origin(target) == source_origin:
        result.update(context)
    return result


def load_ytdlp():
    try:
        return importlib.import_module("yt_dlp")
    except ModuleNotFoundError as error:
        if error.name != "yt_dlp":
            raise
        executable = shutil.which("yt-dlp")
        if not executable or not zipfile.is_zipfile(executable):
            raise
        # The standalone Unix release contains the same importable Python package.
        sys.path.insert(0, str(Path(executable).resolve(strict=True)))
        return importlib.import_module("yt_dlp")


class UnsupportedTransport(Exception):
    """A fixed, credential-free diagnostic safe to show in the queue."""


def private_file(path):
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ValueError("Unsafe transfer file")
    return path


def resource_key(url):
    parts = urllib.parse.urlsplit(url)
    return parts.scheme, parts.hostname, parts.port or {"http": 80, "https": 443, "ftp": 21, "ftps": 990}[parts.scheme], parts.path


def save_metadata(path, metadata):
    temporary = private_file(path.with_suffix(".tmp"))
    with temporary.open("w") as stream:
        json.dump(metadata, stream)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(private_file(path))


def curl_quote(value):
    value = str(value)
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError("Invalid curl setting")
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def curl_settings(config, url, source_credentials=True):
    scheme = urllib.parse.urlsplit(url).scheme
    settings = ["silent", "show-error", 'proto = "=http,https,ftp,ftps"',
                "url = " + curl_quote(url),
                "connect-timeout = " + str(int(config.get("timeoutSeconds", 30))),
                "speed-time = " + str(int(config.get("timeoutSeconds", 30))), 'speed-limit = "1"']
    rate = int(config.get("rateLimitBytes", 0))
    if rate:
        settings.append("limit-rate = " + str(rate))
    method = config.get("authMethod", "auto")
    if scheme in ("http", "https") and source_credentials and method in ("ntlm", "negotiate", "kerberos"):
        features = subprocess.check_output([config["curl"], "-q", "--version"], text=True).lower().split("features:")[-1].split()
        feature = "ntlm" if method == "ntlm" else "spnego"
        if feature not in features:
            raise UnsupportedTransport("The installed curl build does not support " + ("NTLM." if method == "ntlm" else "SPNEGO/Kerberos."))
    if source_credentials and (config.get("username") or method in ("negotiate", "kerberos")):
        settings.append("user = " + curl_quote(config.get("username", "") + ":" + config.get("password", "")))
        if scheme in ("http", "https"):
            settings.append({"auto": "anyauth", "basic": "basic", "digest": "digest", "ntlm": "ntlm", "negotiate": "negotiate", "kerberos": "negotiate"}[method])
    proxy = config.get("proxy", {})
    address = ""
    if proxy:
        host = proxy["host"]
        if ":" in host:
            host = "[" + host + "]"
        address = ("socks5h" if proxy["type"] == "socks5" else proxy["type"]) + "://" + host + ":" + str(proxy["port"])
        if proxy.get("username"):
            settings.append("proxy-user = " + curl_quote(proxy["username"] + ":" + proxy.get("password", "")))
            if proxy["type"] in ("http", "https"):
                settings.append("proxy-anyauth")
        if scheme in ("ftp", "ftps") and proxy["type"] in ("http", "https"):
            settings.append("proxytunnel")
        # curl uses a separate trust store for TLS to the proxy, with verification enabled.
        if proxy["type"] == "https" and os.environ.get("CURL_CA_BUNDLE"):
            settings.append("proxy-cacert = " + curl_quote(os.environ["CURL_CA_BUNDLE"]))
    # Never inherit ambient proxy routing or a user's curlrc for a private job.
    settings += ["proxy = " + curl_quote(address), 'noproxy = ""']
    return settings


def curl_transfer(config, settings, output, headers=None, progress=None):
    private_file(output)
    settings = [*settings, "output = " + curl_quote(output)]
    if headers is not None:
        private_file(headers)
        settings.append("dump-header = " + curl_quote(headers))
    with subprocess.Popen([config["curl"], "-q", "--config", "-"], stdin=subprocess.PIPE,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) as process:
        process.stdin.write(("\n".join(settings) + "\n").encode())
        process.stdin.close()
        while process.poll() is None:
            if progress:
                progress(output.stat().st_size if output.exists() else 0)
            time.sleep(.05)
        if process.returncode != 0:
            raise ValueError("Transfer failed")


def response_headers(path):
    # CONNECT, interim responses and auth challenges precede the final response.
    raw = path.read_bytes()
    if len(raw) > 1024 * 1024:
        raise ValueError("Oversized response headers")
    blocks = [block for block in raw.split(b"\r\n\r\n") if block.startswith(b"HTTP/")]
    if not blocks:
        raise ValueError("Missing HTTP response")
    status, fields = blocks[-1].split(b"\r\n", 1)
    fields = email.parser.BytesParser().parsebytes(fields)
    for key in ("ETag", "Last-Modified", "Content-Range", "Content-Encoding", "Location"):
        if len(fields.get_all(key, [])) > 1:
            raise ValueError("Ambiguous response headers")
    return int(status.split()[1]), fields


class CurlHTTP:
    """curl handles TLS/proxies/auth; redirects and credential scope stay here."""
    def __init__(self, config, context):
        self.config, self.context = config, context
        self.source = origin(config["url"])

    def fetch(self, url, headers=None, data=None, method="GET", output=None, header_path=None, progress=None):
        origin(url)
        scratch = tempfile.TemporaryDirectory(prefix="request-", dir=self.config["directory"])
        try:
            body = output or Path(scratch.name) / "body"
            dump = header_path or Path(scratch.name) / "headers"
            for _ in range(11):
                same_origin = origin(url) == self.source
                scoped = scope_headers(headers or {}, url, self.source, self.context)
                scoped["Accept-Encoding"] = "identity"
                settings = curl_settings(self.config, url, same_origin)
                settings += ["request = " + curl_quote(method)]
                settings += ["header = " + curl_quote(key + ": " + value) for key, value in scoped.items()]
                if data is not None:
                    payload = Path(scratch.name) / "data"
                    payload.write_bytes(data)
                    settings += ["data-binary = " + curl_quote("@" + str(payload))]
                curl_transfer(self.config, settings, body, dump, progress)
                status, fields = response_headers(dump)
                if status not in (301, 302, 303, 307, 308):
                    stream = body.open("rb")
                    return status, fields, stream, url
                target = urllib.parse.urljoin(url, fields["Location"])
                target_origin = origin(target)
                if origin(url)[0] == "https" and target_origin[0] != "https":
                    raise ValueError("Insecure redirect")
                # File downloads follow the Qt engine's stricter authenticated redirect policy.
                if self.config.get("kind") == "http" and (self.context or self.config.get("username") or self.config.get("authMethod") in ("negotiate", "kerberos")) and target_origin != self.source:
                    raise ValueError("Authenticated cross-origin redirect")
                if status == 303 or (status in (301, 302) and method == "POST"):
                    method, data = "GET", None
                url = target
            raise ValueError("Too many redirects")
        finally:
            scratch.cleanup()


def run_http(config):
    work = Path(config["directory"]).resolve(strict=True)
    partial, incoming = private_file(work / "partial.bin"), private_file(work / "incoming.bin")
    record, dump = private_file(work / "resume.json"), private_file(work / "incoming.headers")
    for stale in work.glob("request-*"):
        if stale.is_symlink() or not stale.is_dir():
            raise ValueError("Unsafe request working directory")
        shutil.rmtree(stale)
    transport = CurlHTTP(config, config.get("headers", {}))
    saved = json.loads(record.read_text()) if record.exists() else None

    def absorb():
        if not incoming.exists():
            return
        status, fields = response_headers(dump)
        offset, total = saved["offset"], saved["total"]
        expected = f"bytes {offset}-{total - 1}/{total}"
        if (status != 206 or fields.get("Content-Range") != expected or
                fields.get(saved["header"]) != saved["validator"] or
                fields.get("Content-Encoding", "identity").lower() != "identity" or
                incoming.stat().st_size > total - offset or
                (partial.stat().st_size if partial.exists() else 0) != offset):
            raise ValueError("Invalid resume response")
        # Copy-on-write prevents interruption during consolidation from duplicating bytes.
        combined = private_file(work / "combined.bin")
        with combined.open("wb") as target:
            if partial.exists():
                with partial.open("rb") as source:
                    shutil.copyfileobj(source, target)
            with incoming.open("rb") as source:
                shutil.copyfileobj(source, target)
            target.flush()
            os.fsync(target.fileno())
        combined.replace(partial)
        incoming.unlink()
        saved["offset"] = partial.stat().st_size
        save_metadata(record, saved)

    if saved and resource_key(saved["url"]) != resource_key(config["url"]):
        raise ValueError("Different resume resource")
    if saved and incoming.exists():
        # A completed consolidation can survive a crash before the sidecar update.
        if partial.exists() and partial.stat().st_size == saved["offset"] + incoming.stat().st_size:
            incoming.unlink()
            saved["offset"] = partial.stat().st_size
            save_metadata(record, saved)
        else:
            try:
                absorb()
            except ValueError:
                # An interrupted header or rejected response cannot become saved resource bytes.
                incoming.unlink()
    started = time.monotonic()
    offset = partial.stat().st_size if partial.exists() else 0

    def progress(size):
        emit(type="progress", received=offset + size, total=saved["total"] if saved else -1,
             speed=size / max(.001, time.monotonic() - started), eta=-1)

    status, fields, probe, _ = transport.fetch(config["url"], {"Range": "bytes=0-0"}, progress=progress)
    try:
        if status == 200:
            if offset:
                raise ValueError("Server stopped supporting resume")
            if fields.get("Content-Encoding", "identity").lower() != "identity":
                raise ValueError("Encoded response")
            output = private_file(work / "verified.bin")
            with output.open("wb") as target:
                shutil.copyfileobj(probe, target)
            emit(type="metadata", resumable=False, httpStatus=200)
        elif status == 206:
            match = re.fullmatch(r"bytes 0-0/([1-9][0-9]*)", fields.get("Content-Range", ""))
            if not match or len(probe.read(2)) != 1 or fields.get("Content-Encoding", "identity").lower() != "identity":
                raise ValueError("Invalid range probe")
            total = int(match[1])
            header = "ETag" if re.fullmatch(r'"[^"\x00-\x20\x7f]*"', fields.get("ETag", "")) else "Last-Modified"
            validator = fields.get(header, "")
            if not validator:
                if offset:
                    raise ValueError("Missing resume validator")
                status, fields, stream, _ = transport.fetch(config["url"], progress=progress)
                with stream, private_file(work / "verified.bin").open("wb") as target:
                    if status != 200 or fields.get("Content-Encoding", "identity").lower() != "identity":
                        raise ValueError("Invalid serial response")
                    shutil.copyfileobj(stream, target)
                output = work / "verified.bin"
                emit(type="metadata", resumable=False, httpStatus=status)
            else:
                if saved and (saved["total"], saved["header"], saved["validator"]) != (total, header, validator):
                    raise ValueError("Changed resume resource")
                if offset > total:
                    raise ValueError("Oversized partial")
                saved = dict(url=config["url"], total=total, header=header, validator=validator, offset=offset)
                save_metadata(record, saved)
                emit(type="metadata", resumable=True, httpStatus=206)
                if offset < total:
                    status, fields, stream, _ = transport.fetch(config["url"], {"Range": f"bytes={offset}-{total - 1}", "If-Range": validator},
                                                               output=incoming, header_path=dump, progress=progress)
                    stream.close()
                    absorb()
                if not partial.exists() or partial.stat().st_size != total:
                    raise ValueError("Truncated range")
                output = partial
        elif status == 416 and fields.get("Content-Range") == "bytes */0" and not offset:
            output = private_file(work / "verified.bin")
            output.write_bytes(b"")
        else:
            raise ValueError("Invalid HTTP status")
    finally:
        probe.close()
    verified = private_file(work / "verified.bin")
    if output != verified:
        # Keep the recoverable source through publication failures or process termination.
        shutil.copyfile(output, verified)
    emit(type="complete", path=str(verified), bytes=verified.stat().st_size)


def run_ftp(config):
    parts = urllib.parse.urlsplit(config["url"])
    if parts.scheme not in ("ftp", "ftps") or not parts.hostname or parts.username or parts.password or any(ord(c) < 32 for c in urllib.parse.unquote(parts.path)):
        raise ValueError("Invalid FTP URL")
    work = Path(config["directory"]).resolve(strict=True)
    partial, record = private_file(work / "partial.bin"), private_file(work / "resume.json")
    settings = curl_settings(config, config["url"]) + ['ftp-method = "nocwd"', "fail"]

    def metadata():
        # FTP's SIZE/MDTM pair is useful for early rejection but is not a strong validator.
        curl_transfer(config, [*settings, "head", "remote-time"], work / "metadata.txt")
        text = (work / "metadata.txt").read_text()
        match = re.search(r"(?im)^Content-Length: ([0-9]+)", text)
        fields = email.parser.Parser().parsestr(text)
        return dict(url=config["url"], size=int(match[1]) if match else -1,
                    modified=fields.get("Last-Modified", ""), rest=fields.get("Accept-ranges", "").lower() == "bytes")

    current = metadata()
    saved = json.loads(record.read_text()) if record.exists() else None
    offset = partial.stat().st_size if partial.exists() else 0
    if saved and resource_key(saved["url"]) != resource_key(config["url"]):
        raise ValueError("Different FTP resume resource")
    if offset and saved and (saved["size"], saved["modified"]) != (current["size"], current["modified"]):
        raise ValueError("FTP resource changed")
    resumable = current["size"] >= 0 and bool(current["modified"])
    if offset and (not saved or not resumable):
        partial.unlink()
        offset = 0
    if current["size"] >= 0 and offset > current["size"]:
        raise ValueError("Oversized FTP partial")
    save_metadata(record, current)
    emit(type="metadata", resumable=resumable and current["rest"])
    started = time.monotonic()

    def progress(size):
        emit(type="progress", received=size, total=current["size"],
             speed=max(0, size - offset) / max(.001, time.monotonic() - started), eta=-1)

    if not offset or offset != current["size"]:
        # curl checks REST acceptance; the original partial remains on a refusal.
        curl_transfer(config, [*settings, *(["continue-at = " + str(offset)] if offset else [])], partial, progress=progress)
    if (current["size"] >= 0 and partial.stat().st_size != current["size"]) or metadata() != current:
        raise ValueError("FTP resource changed during transfer")
    if offset:
        # MDTM has coarse resolution. Verify every byte against a new full stream before publishing.
        check = private_file(work / "check.bin")
        curl_transfer(config, settings, check, progress=lambda _: progress(partial.stat().st_size))
        with partial.open("rb") as local, check.open("rb") as remote:
            while True:
                chunk = local.read(65536)
                if chunk != remote.read(65536):
                    raise ValueError("FTP resumed content changed")
                if not chunk:
                    break
        check.unlink()
        if metadata() != current:
            raise ValueError("FTP resource changed during verification")
    output = private_file(work / "verified.bin")
    shutil.copyfile(partial, output)
    emit(type="complete", path=str(output), bytes=output.stat().st_size)


def run(config):
    yt_dlp = load_ytdlp()
    from yt_dlp.networking import Request, Response
    from yt_dlp.networking.exceptions import HTTPError, TransportError
    from yt_dlp.downloader.external import FFmpegFD
    from yt_dlp.utils import DownloadError

    source_origin = origin(config["url"])
    context = config.get("headers", {})
    use_curl = config.get("proxy", {}).get("type") in ("socks5", "https") or config.get("authMethod", "auto") != "auto"
    if not use_curl and config.get("username") and not context.get("authorization"):
        context["authorization"] = "Basic " + base64.b64encode((config["username"] + ":" + config.get("password", "")).encode()).decode()

    class Redirects(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, request, response, code, message, headers, newurl):
            if origin(request.full_url)[0] == "https" and origin(newurl)[0] != "https":
                raise ValueError("Insecure media redirect")
            redirected = super().redirect_request(request, response, code, message, headers, newurl)
            if redirected is not None:
                redirected.headers = scope_headers(redirected.headers, newurl, source_origin, context)
                redirected.unredirected_hdrs.clear()
            return redirected

    proxy = config.get("proxy", {})
    proxy_handler = urllib.request.ProxyHandler()
    if proxy:
        if proxy.get("type") != "http" and not use_curl:
            raise ValueError("Unsupported media proxy")
        host = proxy["host"]
        if ":" in host:
            host = "[" + host + "]"
        credentials = ""
        if proxy.get("username"):
            credentials = urllib.parse.quote(proxy["username"], safe="") + ":" + urllib.parse.quote(proxy.get("password", ""), safe="") + "@"
        address = "http://" + credentials + host + ":" + str(proxy["port"])
        proxy_handler = urllib.request.ProxyHandler({"http": address, "https": address})
    opener = urllib.request.build_opener(proxy_handler, Redirects())

    transport = CurlHTTP(config, context) if use_curl else None

    class ScopedYoutubeDL(yt_dlp.YoutubeDL):
        def urlopen(self, request):
            if isinstance(request, str):
                request = Request(request)
            if transport:
                try:
                    status, fields, stream, url = transport.fetch(request.url, request.headers, request.data, request.method)
                    response = Response(stream, url, fields, status)
                    if status >= 400:
                        raise HTTPError(response)
                    return response
                except (OSError, ValueError) as exc:
                    raise TransportError("Media request failed") from exc
            headers = scope_headers(request.headers, request.url, source_origin, context)
            headers["Accept-Encoding"] = "identity"
            native = urllib.request.Request(request.url, data=request.data, headers=headers, method=request.method)
            try:
                response = opener.open(native, timeout=30)
            except urllib.error.HTTPError as exc:
                raise HTTPError(Response(exc, exc.url, exc.headers, exc.code, exc.reason)) from exc
            except (OSError, ValueError) as exc:
                raise TransportError("Media request failed") from exc
            return Response(response, response.url, response.headers, response.status, response.reason)

    def forbid_ffmpeg_network(*args, **kwargs):
        raise DownloadError("This media needs an unsupported network downloader")

    # Native HLS can otherwise fall back to FFmpegFD, outside the scoped HTTP path.
    FFmpegFD.real_download = forbid_ffmpeg_network
    amounts = {}

    def progress(info):
        filename = info.get("filename", "capture")
        amounts[filename] = max(0, int(info.get("downloaded_bytes") or 0))
        total = info.get("total_bytes") or info.get("total_bytes_estimate") or -1
        emit(type="progress", received=sum(amounts.values()), total=int(total),
             speed=float(info.get("speed") or 0), eta=int(info.get("eta") or -1))

    class QuietLogger:
        def debug(self, message):
            pass

        def warning(self, message):
            pass

        def error(self, message):
            pass

    work = Path(config["directory"]).resolve(strict=True)
    os.chdir(work)
    options = {
        "quiet": True, "no_warnings": True, "logger": QuietLogger(),
        "noplaylist": True, "cachedir": False, "continuedl": False, "overwrites": False,
        "outtmpl": str(work / "capture.%(ext)s"), "socket_timeout": int(config.get("timeoutSeconds", 30)),
        "ratelimit": int(config.get("rateLimitBytes", 0)) or None,
        "concurrent_fragment_downloads": min(16, max(1, int(config.get("connections", 4)))),
        "external_downloader": "native", "hls_prefer_native": True,
        "format": {"best": "bestvideo+bestaudio/best", "1080p": "bestvideo[height<=?1080]+bestaudio/best[height<=?1080]", "720p": "bestvideo[height<=?720]+bestaudio/best[height<=?720]", "audio": "bestaudio/best"}[config.get("format") or "best"],
        "merge_output_format": "mkv", "progress_hooks": [progress],
        "retries": 2, "fragment_retries": 2, "skip_unavailable_fragments": False,
        "postprocessor_args": {"ffmpeg_i": ["-protocol_whitelist", "file,crypto,data"]},
    }
    with ScopedYoutubeDL(options) as downloader:
        info = downloader.extract_info(config["url"], download=False)
        if not info or info.get("_type") in ("playlist", "multi_video") or info.get("is_live") or info.get("has_drm"):
            raise ValueError("Select a finite, unprotected media source")
        info = downloader.process_ie_result(info, download=True)
        path = Path(downloader.prepare_filename(info)).resolve(strict=True)
        if path.parent != work or not path.is_file() or path.is_symlink():
            raise ValueError("Invalid downloaded media output")

    extension = config.get("extension", "mkv")
    containers = {"mkv": "matroska", "mka": "matroska", "mp4": "mp4", "webm": "webm", "m4a": "ipod", "mp3": "mp3", "ogg": "ogg", "opus": "opus", "wav": "wav", "flac": "flac", "aac": "adts"}
    if extension not in containers:
        raise ValueError("Unsupported output container")
    output = work / ("verified." + extension)
    # This fixed private output is reproducible from the retained download after interruption.
    if output.is_symlink():
        raise ValueError("Unsafe media output")
    output.unlink(missing_ok=True)
    maps = ["-map", "0:a?"] if config.get("format") == "audio" else ["-map", "0:v?", "-map", "0:a?"]
    subprocess.run([config["ffmpeg"], "-nostdin", "-v", "error", "-n", "-protocol_whitelist", "file,crypto,data",
                    "-i", str(path), *maps, "-c", "copy", "-f", containers[extension], str(output)],
                   check=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    result = subprocess.run([config["ffprobe"], "-v", "error", "-protocol_whitelist", "file,crypto,data", "-show_streams", "-show_format", "-of", "json", str(output)],
                            check=True, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=30)
    metadata = json.loads(result.stdout)
    streams = [stream.get("codec_type") for stream in metadata.get("streams", []) if stream.get("codec_type") in ("audio", "video")]
    duration = float(metadata.get("format", {}).get("duration", 0))
    if not streams or duration <= 0 or output.stat().st_size <= 0:
        raise ValueError("Media output has no playable duration or streams")
    emit(type="complete", path=str(output), bytes=output.stat().st_size, duration=duration, streams=streams)


def main():
    os.umask(0o077)
    parent = os.getppid()
    os.setsid()

    def terminate(signum, frame):
        signal.signal(signum, signal.SIG_DFL)
        os.killpg(os.getpgrp(), signum)

    signal.signal(signal.SIGTERM, terminate)
    # A dead manager must not leave a writer racing the next queue owner.
    if ctypes.CDLL(None).prctl(1, signal.SIGTERM, 0, 0, 0) != 0:
        return 1
    if parent == 1 or os.getppid() != parent:
        terminate(signal.SIGTERM, None)
    config = {}
    try:
        value = json.load(sys.stdin)
        if not isinstance(value, dict):
            raise ValueError("Worker configuration must be an object")
        config = value
        if config.get("parentPid", parent) != os.getppid():
            terminate(signal.SIGTERM, None)
        if config.get("kind") == "ftp":
            run_ftp(config)
        elif config.get("kind") == "http":
            run_http(config)
        else:
            run(config)
    except UnsupportedTransport as exc:
        emit(type="error", error=str(exc))
        return 1
    except ImportError:
        emit(type="error", error="Cannot load yt-dlp from its Python package or standalone executable. Reinstall yt-dlp.")
        return 1
    except Exception:
        # Exceptions can embed signed media URLs, cookies, or authorization. Keep them private.
        emit(type="error", error="FTP transfer failed. Check the server, credentials, proxy and available disk space." if config.get("kind") == "ftp" else "HTTP transfer failed. Check the server, authentication method, proxy, TLS trust and saved resource validators." if config.get("kind") == "http" else "Media capture failed. The source is unavailable, needs different authorization, or uses an unsupported media format.")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

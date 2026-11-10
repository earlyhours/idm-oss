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


def emit(**frame):
    print(json.dumps(frame, separators=(",", ":")), flush=True)


def origin(url):
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in ("http", "https") or not parts.hostname or parts.username or parts.password:
        raise ValueError("Unsupported media URL")
    return parts.scheme, parts.hostname.lower(), parts.port or (443 if parts.scheme == "https" else 80)


def scope_headers(headers, target, source_origin, context):
    # Extractors may carry cookies between formats. Only the captured origin gets context.
    blocked = {"cookie", "authorization", "proxy-authorization", "referer", "origin", *context}
    result = {str(k): str(v) for k, v in headers.items() if k.lower() not in blocked}
    if origin(target) == source_origin:
        result.update(context)
    return result


def run(config):
    import yt_dlp
    from yt_dlp.networking import Request, Response
    from yt_dlp.networking.exceptions import HTTPError, TransportError
    from yt_dlp.downloader.external import FFmpegFD
    from yt_dlp.utils import DownloadError

    source_origin = origin(config["url"])
    context = config.get("headers", {})

    class Redirects(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, request, response, code, message, headers, newurl):
            if origin(request.full_url)[0] == "https" and origin(newurl)[0] != "https":
                raise ValueError("Insecure media redirect")
            redirected = super().redirect_request(request, response, code, message, headers, newurl)
            if redirected is not None:
                redirected.headers = scope_headers(redirected.headers, newurl, source_origin, context)
                redirected.unredirected_hdrs.clear()
            return redirected

    opener = urllib.request.build_opener(Redirects())

    class ScopedYoutubeDL(yt_dlp.YoutubeDL):
        def urlopen(self, request):
            if isinstance(request, str):
                request = Request(request)
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
        "outtmpl": str(work / "capture.%(ext)s"), "socket_timeout": 30,
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
    os.setsid()

    def terminate(signum, frame):
        signal.signal(signum, signal.SIG_DFL)
        os.killpg(os.getpgrp(), signum)

    signal.signal(signal.SIGTERM, terminate)
    try:
        config = json.load(sys.stdin)
        run(config)
    except ImportError:
        emit(type="error", error="The system Python cannot load yt-dlp. Install the free yt-dlp package.")
        return 1
    except Exception:
        # Exceptions can embed signed media URLs, cookies, or authorization. Keep them private.
        emit(type="error", error="Media capture failed. The source is unavailable, needs different authorization, or uses an unsupported media format.")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

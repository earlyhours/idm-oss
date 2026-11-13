#!/usr/bin/env python3
"""Verify real media quality selection against generated 720p/1080p HLS variants."""
import http.server
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MediaQualityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory(prefix="idm-quality-test-")
        cls.addClassCleanup(cls.scratch.cleanup)
        cls.fixture = Path(cls.scratch.name) / "fixture"
        cls.fixture.mkdir()
        for height, width, color in ((720, 1280, "blue"), (1080, 1920, "red")):
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", f"color=c={color}:s={width}x{height}:r=10:d=1",
                            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "1", "-c:v", "libx264", "-preset", "ultrafast", "-crf", "30",
                            "-pix_fmt", "yuv420p", "-g", "10", "-c:a", "aac", "-ac", "1", "-hls_time", "1", "-hls_playlist_type", "vod",
                            "-hls_segment_filename", str(cls.fixture / f"quality{height}_%02d.ts"), str(cls.fixture / f"quality{height}.m3u8")],
                           check=True, stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
        (cls.fixture / "master.m3u8").write_text(
            '#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=1280x720,CODECS="avc1.42E01F,mp4a.40.2"\nquality720.m3u8\n'
            '#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=1920x1080,CODECS="avc1.42E028,mp4a.40.2"\nquality1080.m3u8\n')
        cls.requests = []

        class Handler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=cls.fixture, **kwargs)

            def send_head(self):
                authorized = self.headers.get("Cookie") == "idm-quality-fixture=1" and self.headers.get("Referer") == cls.referrer
                cls.requests.append((self.path.split("?", 1)[0], authorized))
                if not authorized:
                    self.send_error(403)
                    return None
                return super().send_head()

            def log_message(self, *_args):
                pass

        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.referrer = f"http://127.0.0.1:{cls.server.server_port}/watch"
        cls.url = f"http://127.0.0.1:{cls.server.server_port}/master.m3u8?fixture=quality"
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.addClassCleanup(cls.server.server_close)
        cls.addClassCleanup(cls.server.shutdown)

    def capture(self, quality, extension="mkv"):
        work = Path(self.scratch.name) / quality
        work.mkdir()
        config = {"kind": "media", "url": self.url, "directory": str(work), "format": quality, "extension": extension,
                  "connections": 4, "timeoutSeconds": 15, "ffmpeg": "/usr/bin/ffmpeg", "ffprobe": "/usr/bin/ffprobe",
                  "headers": {"cookie": "idm-quality-fixture=1", "referer": self.referrer}}
        environment = {key: value for key, value in os.environ.items() if key not in ("PYTHONHOME", "PYTHONPATH")}
        environment["PYTHONNOUSERSITE"] = "1"
        process = subprocess.Popen(["/usr/bin/python3", str(ROOT / "src/mediaworker.py")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
        try:
            output, error = process.communicate(json.dumps(config).encode(), timeout=30)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                process.kill()
            process.communicate(timeout=5)
            self.fail("Media quality fixture timed out")
        self.assertEqual(process.returncode, 0, output.decode() + error.decode())
        frames = [json.loads(line) for line in output.splitlines()]
        complete = [frame for frame in frames if frame.get("type") == "complete"]
        self.assertEqual(len(complete), 1, frames)
        path = Path(complete[0]["path"])
        self.assertEqual(path, work / f"verified.{extension}")
        self.assertGreater(path.stat().st_size, 0)
        metadata = json.loads(subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)],
                                             check=True, capture_output=True, timeout=10).stdout)
        self.assertGreater(float(metadata["format"]["duration"]), 0)
        self.assertTrue(all(authorized for _path, authorized in self.requests), "A media request lost its scoped browser context")
        return metadata

    def test_720p_selects_the_720p_representation(self):
        metadata = self.capture("720p")
        video = [stream for stream in metadata["streams"] if stream["codec_type"] == "video"]
        audio = [stream for stream in metadata["streams"] if stream["codec_type"] == "audio"]
        self.assertEqual([stream["height"] for stream in video], [720])
        self.assertTrue(audio)
        self.assertTrue(any(path == "/quality720.m3u8" for path, _authorized in self.requests))

    def test_1080p_selects_the_1080p_representation(self):
        metadata = self.capture("1080p")
        video = [stream for stream in metadata["streams"] if stream["codec_type"] == "video"]
        audio = [stream for stream in metadata["streams"] if stream["codec_type"] == "audio"]
        self.assertEqual([stream["height"] for stream in video], [1080])
        self.assertTrue(audio)
        self.assertTrue(any(path == "/quality1080.m3u8" for path, _authorized in self.requests))

    def test_best_selects_the_highest_representation(self):
        metadata = self.capture("best")
        video = [stream for stream in metadata["streams"] if stream["codec_type"] == "video"]
        self.assertEqual([stream["height"] for stream in video], [1080])
        self.assertTrue(any(stream["codec_type"] == "audio" for stream in metadata["streams"]))

    def test_audio_selection_has_no_video_stream(self):
        metadata = self.capture("audio", "mka")
        self.assertTrue(any(stream["codec_type"] == "audio" for stream in metadata["streams"]))
        self.assertFalse(any(stream["codec_type"] == "video" for stream in metadata["streams"]))


if __name__ == "__main__":
    unittest.main()

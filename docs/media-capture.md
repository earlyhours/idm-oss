# Browser requests and media capture

`DownloadManager::addRequest` accepts a GET URL and optional `filename`, `directory`, `connections`, `headers`, `pageUrl`, `title`, `description`, `media`, `format`, and `start` fields. `start:false` creates a persisted paused job without network activity. The original `add` API delegates to this request boundary.

Header names and values are checked before dispatch. Accepted contextual headers are Cookie, Authorization, User-Agent, Referer, Origin, Accept, and Accept-Language. Managed headers, hop-by-hop headers, and other unknown headers are discarded. Control characters, invalid URLs, embedded URL credentials, non-string values, excessive sizes, and non-GET methods are rejected. Queue JSON and partial/output files remain owner-only. Headers are absent from UI snapshots and subprocess arguments.

File requests use manual cookie storage and user-verified redirects. A file request with browser context rejects a redirect to a different origin, including another port, before following it. Context-free requests retain normal HTTP redirect behavior. HTTPS downgrades are rejected. This conservative file behavior preserves credentials rather than guessing which headers a different server needs.

Media capture uses the free system Python yt-dlp module, FFmpeg and ffprobe. The bundled worker receives private JSON over stdin. Its HTTP adapter attaches browser context only to the exact captured scheme/host/port. Redirects and manifest fragments at another origin receive none of those contextual headers. Browser-wide cookie databases are never loaded. Native yt-dlp downloaders fetch HTTP, HLS and DASH fragments. The FFmpeg network downloader is disabled; local postprocessing restricts FFmpeg's input protocols to file, crypto and data. Live or protected formats that need a different network path fail explicitly.

Media output is remuxed locally, then ffprobe must establish positive duration and an audio or video stream before the normal non-overwriting publication path runs. `format` accepts `best`, `1080p`, `720p`, or `audio`. Height limits select matching formats when the manifest supplies resolution metadata; an unknown-resolution source remains selectable. Audio capture copies only audio streams into a Matroska audio file. Missing tools, failed requests and invalid output leave an error in the queue.

Pausing stops the media worker and its FFmpeg process group. The job and private context survive application restarts. Resuming media restarts the capture from the beginning because fragment collections do not provide a common strong identity validator. Media therefore reports `resumable:false`; HTTP files with validated ranges keep their existing byte-resume behavior. No claim of seamless media fragment resume is made.

Engine snapshots add category, creation date, resumability, per-part byte counts, estimated time remaining, HTTP status, description and media state. `resumeAll` starts paused/error jobs; `removeCompleted` removes queue history while retaining downloaded files.

## Verification

The Qt test suite generates a two-second H.264/AAC fixture with FFmpeg, then serves direct MP4, HLS and DASH versions over local HTTP. Downloads are accepted only after an independent ffprobe call confirms positive duration and both video and audio streams. The audio case verifies that video is absent. Tests also cover browser headers across queue restart, header injection, Download Later without requests, cross-origin credential handling, media errors and media pause/application restart.

Library behavior follows [yt-dlp's embedding API](https://github.com/yt-dlp/yt-dlp#embedding-yt-dlp). The scoped HTTP adapter and native-only fallback restriction are covered against the installed yt-dlp 2026.08.19 package. Future yt-dlp changes should rerun these generated fixtures.

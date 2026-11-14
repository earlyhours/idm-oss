# Browser requests and media capture

`DownloadManager::addRequest` accepts a GET URL and optional `filename`, `directory`, `connections`, `headers`, `pageUrl`, `title`, `description`, `media`, `format`, and `start` fields. `start:false` creates a persisted paused job without network activity. The original `add` API delegates to this request boundary.

Header names and values are checked before dispatch. Valid custom headers are accepted, including Cookie, Authorization, User-Agent, Referer, Origin, Accept, Accept-Language and application-specific names. Managed, hop-by-hop and Connection-nominated headers are discarded. Control characters, invalid URLs, embedded URL credentials, non-string values, excessive sizes, and non-GET methods are rejected. Queue JSON and partial/output files remain owner-only. Headers are absent from UI snapshots and subprocess arguments.

File requests use manual cookie storage and user-verified redirects. A file request with browser context rejects a redirect to a different origin, including another port, before following it. Context-free requests retain normal HTTP redirect behavior. HTTPS downgrades are rejected. This conservative file behavior preserves credentials rather than guessing which headers a different server needs.

Media capture uses the free system Python yt-dlp module, FFmpeg and ffprobe. The bundled worker receives private JSON over stdin. Its HTTP adapter attaches browser context only to the exact captured scheme/host/port. Redirects and manifest fragments at another origin receive none of those contextual headers. Browser-wide cookie databases are never loaded. Native yt-dlp downloaders fetch HTTP, HLS and DASH fragments. The FFmpeg network downloader is disabled; local postprocessing restricts FFmpeg's input protocols to file, crypto and data. Live or protected formats that need a different network path fail explicitly.

Media output is remuxed locally, then ffprobe must establish positive duration and an audio or video stream before the normal non-overwriting publication path runs. `format` accepts `best`, `1080p`, `720p`, or `audio`. Height limits select matching formats when the manifest supplies resolution metadata; an unknown-resolution source remains selectable. Audio capture copies only audio streams into a Matroska audio file. Missing tools, failed requests and invalid output leave an error in the queue.

Pausing stops the media worker and its FFmpeg process group. The job and private context survive application restarts. Resuming media restarts the capture from the beginning because fragment collections do not provide a common strong identity validator. Media therefore reports `resumable:false`; HTTP files with validated ranges keep their existing byte-resume behavior. No claim of seamless media fragment resume is made.

Engine snapshots add category, creation date, resumability, per-part byte counts, estimated time remaining, HTTP status, description and media state. `resumeAll` starts paused/error jobs; `removeCompleted` removes queue history while retaining downloaded files.

## Verification

The Qt test suite generates a two-second H.264/AAC fixture with FFmpeg, then serves direct MP4, HLS and DASH versions over local HTTP. Downloads are accepted only after an independent ffprobe call confirms positive duration and both video and audio streams. The audio case verifies that video is absent. Tests also cover browser headers across queue restart, header injection, Download Later without requests, cross-origin credential handling, media errors and media pause/application restart.

Library behavior follows [yt-dlp's embedding API](https://github.com/yt-dlp/yt-dlp#embedding-yt-dlp). The scoped HTTP adapter and native-only fallback restriction are covered against the installed yt-dlp 2026.08.19 package. Future yt-dlp changes should rerun these generated fixtures.

## Advanced transfer controls

Per-request fields also accept `rateLimitBytes` with zero meaning unlimited, `timeoutSeconds` from 1 to 3600, source `username`/`password`, and `proxy` as `{type,host,port,username,password}`. Proxy types are `http`, `https` and `socks5`. Each HTTP job owns a separate Qt network manager, so authentication and connection caches are isolated between jobs. Explicit credentials enable Qt's native challenge authentication within that job; browser Authorization headers remain scoped by the request boundary. HTTP Basic, an authenticated HTTP proxy, and an authenticated SOCKS5 tunnel have local server tests. HTTP CONNECT can carry HTTPS destinations. HTTPS proxy transport uses curl with normal TLS verification. SOCKS media uses a scoped curl adapter; cross-origin fragments receive none of the source cookies or authorization headers.

HTTP range and serial bodies use a shared per-job read budget and bounded Qt buffers. Completion waits for buffered bytes to drain, including when the network reply has already finished. `setRateLimit` pauses/restarts an active attempt with the new rate while preserving validated HTTP partials. A stalled server reaches an explicit timeout. Media uses yt-dlp's rate and timeout options; FTP uses curl's controls.

Idle HTTP workers split a large remaining validated range in half, up to 16 physical parts. Part-file identities stay stable. Coverage checks and final assembly order parts by their byte offsets. The test pauses after a split, reopens the queue, resumes it, and compares the final bytes with the original payload.

FTP and FTPS use free curl through the same private worker, with configuration and credentials on stdin rather than command arguments. Output must match curl's reported downloaded size before it reaches the normal non-overwriting completion path. The local FTP fixture verifies real USER/PASS authentication, data-channel transfer, exact output and failed-login behavior. FTPS relies on curl's normal certificate verification and has no local TLS fixture yet. FTP restart uses private persisted partials when SIZE, MDTM and REST support remain consistent. Before publishing resumed output, a second complete transfer must match its hash. This adds bandwidth and time, but prevents a server that retained stale metadata from causing mixed output.

`rename(id,filename)` validates a basename, retains the output directory, avoids existing files, and supports active/paused/completed downloads. `refresh(id,url)` permits signed-query refreshes of the same resource while preserving validated ranges; a different origin or path cannot reuse saved ranged bytes. Changing origins before any reusable bytes exist clears old contextual credentials.

## Measured local behavior

The controlled server sends 512 KiB at 2048 bytes per connection every configured 5 ms. A measured run took 2690 ms with one range worker and 705 ms with four workers, a 3.82x ratio. Both outputs matched the exact fixture bytes. This measures that particular server's per-connection throttle, not a general Internet speed promise. The benchmark records two TCP connections for the serial run and five for the parallel run, including the one-byte probe. A separate keep-alive test served five HTTP requests over four actual TCP connections, proving reuse of the probe connection.

The complete engine suite passes 44 cases, including the prior browser/media and file-safety regressions. Machine-readable benchmark output is written to `/tmp/idm-engine-benchmark.json` by `controlledParallelBenchmark`.

`updateContext` validates and atomically updates private headers and credentials for an unfinished paused or failed job. It preserves parts and validators, discards cached authentication, and rejects invalid updates without changing the saved request. Browser recapture of the same URL uses this path before resuming; explicit resume can retry a previously expired request without being blocked by its old HTTP status.

## Explicit authentication and encrypted proxies

Requests accept `authMethod` with `auto`, `basic`, `digest`, `ntlm`, `negotiate`, or `kerberos`. Auto uses the existing Qt transport for ordinary HTTP. Explicit methods and HTTPS proxies use the private curl worker with one connection. Negotiate and Kerberos use the system GSS credential cache. Explicit NTLM requires an NTLM-capable curl build; an unsupported build reports an error. Auto NTLMv2 is tested through Qt.

Source and proxy credentials are configured separately and passed through private worker input rather than subprocess arguments. Redirects retain source context only within its exact origin. TLS verification remains enabled for both HTTPS destinations and encrypted proxies. Existing Qt range partials cannot move to the curl transport, or back, through a context change.

`tests/run_transfer_features` exercises real Basic and Digest challenges, an isolated Kerberos KDC, NTLMv2 response proofs, encrypted proxy trust, independent proxy and source authentication, FTP REST recovery, and SOCKS-proxied direct media, HLS and DASH. It compares decoded media frames and checks credential scoping. Kerberos fixtures require `krb5kdc`, `kadmin.local`, `kinit`, and the GSS library. The normal package needs configured tickets only when the user selects Kerberos.

MMS is unsupported. Installed FFmpeg's `mmsh` and `mmst` handlers do not establish a tested, origin-scoped `mms://` implementation. Media pause/restart and live-stream limits remain as described above.

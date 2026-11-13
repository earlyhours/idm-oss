# IDM browser integration, static analysis

The Chrome extension captures browser request context and passes download jobs to the native IDM application. It also discovers media in page frames and can copy selected response bodies to the native side. The extension itself is not evidence of IDM's native connection scheduler or file assembly algorithm.

## Artifact and method

- Supplied installer SHA-256 `7346128fab42957b4254eabdb012bfdbd751e9069361e98a6916f26bb03d62ee`.
- Extracted `IDMGCExt.crx`, 107834 bytes, SHA-256 `1d2297ccbdbbf06e9e2546bb9f9b9985433eb7c02d75f09bbcbc9784ece583f7`.
- Its ZIP contents were read with Python's `zipfile`, with checks against absolute paths, traversal, drive paths, symlinks and overwrites, into `/tmp/idm-chrome-extension`.
- REA `analyze_javascript_application` parsed six JavaScript files with zero parse failures, visited 36582 AST nodes and read 123545 text bytes. It reports partial application coverage and 7966 semantic unknowns. The code is minified and several property names are supplied dynamically.
- Complete REA Evidence is exported in `~/.cache/idm-analysis/browser-js-evidence.json`, 38322005 bytes. Parent Evidence ID `ev_7ce6d046f67a09c21f91c4b2209cf27cc4e325921d5be5ab584e79227a72a03a`. Root extracted-directory digest `06b86604c7e3eadda4b0c6880d1a1407a519127e3f67a4598f5f0b8268266067`.
- The initial complete MCP response exceeded transport budget. REA retained the complete record, which was exported without rerunning analysis. No extension or application code was executed.

## Entry points and privileges

Observed in `manifest.json:4,12-22`: version 6.43.1, Manifest V3, `background.js` as a service worker, and `content.js` at document start in all matching frames. Permissions include webRequest, declarativeNetRequest, downloads, cookies, proxy and nativeMessaging with all-URL host access. The manifest alone does not establish that any permission was exercised.

The content script has a further explicit Windows platform and HTML-root guard at `content.js:2`. Its constructor connects separate top/subframe runtime ports and registers page interaction listeners at `content.js:4-9`. This matters for reproducing IDM integration on Linux: removing that guard alone would not supply the Windows native host or application.

## Native connection and message framing

At `background.js:11`, `La` lists `127.0.0.1:1001` and `0.1.0.1:1001`. At `background.js:37-39`, `Ba` cycles through these WebSocket endpoints and `browser.runtime.connectNative("com.tonec.idm")`. WebSockets use subprotocol `plugin.v3.internetdownloadmanager.com`; their URL includes a client identifier and random query value. The second address is reproduced exactly as shipped; no assumption that it is equivalent to loopback is made here.

`IDMMsgHost.json:2-6` separately declares the native host name `com.tonec.idm`, transport `stdio`, relative executable `IDMMsgHost.exe` and two allowed Chrome extension origins. The Firefox host manifest names the same executable with a Mozilla extension allowlist. These declarations and calls establish the expected bridge, but do not establish the helper's internal forwarding implementation.

`qb` at `background.js:15-18` serializes and parses records beginning `MSG#<id>#<type>#<subtype>#<flags>`, with colon-separated positional fields, numeric keyed fields and length-bearing string/binary values. At `background.js:41`, `S` sends these records as a `Blob` on the WebSocket route or as a joined string through native-port `postMessage`. The application protocol is therefore additional framing carried inside the browser-native messaging API, not simply a JSON object describing a URL.

The connection handshake at `background.js:38` includes browser identities, UI language, extension manifest version and several numeric protocol/version fields. Their full compatibility semantics remain unresolved.

## Request observation and classification

`Nb`, `background.js:29-30`, registers request-before-send, request-header, response-header, response-start, error and completion listeners alongside the downloads API. `cb` at `background.js:103-106` constructs per-request state for GET/POST requests, including URL, frame, request ID, request body, request type and originating content port.

`Rb` at `background.js:107-109` attaches request headers and handles special `Accept` markers used to correlate page-level capture. `Ub` at `background.js:110-111` records response status, headers, address and timing before classification. `Fc`, `Gc` and `Hc` at `background.js:91-98` apply extension/host/URL filters and inspect MIME type, attachment disposition, filename, status and media-manifest types. In particular, status 200/206/304 and M3U8/MPD/F4M branches are visible. These are classification branches, not proof that the native downloader uses a particular segment count.

`X` at `background.js:99-101` emits message type 13 with URL, original/redirect context, request ID, tab/frame IDs, status, filename fields, request/response headers and POST body where available. `Kc` at `background.js:122` serializes headers; `Bc` at `background.js:125-126` handles raw request bytes and form data, including multipart encoding.

## Cookies and explicit download handoff

`U`, `background.js:121`, calls `browser.cookies.getAll` for a URL. `V` on that line converts cookies to a `name=value; ...` string. Explicit download `pa`, `background.js:45`, obtains cookies for HTTP-family links and sends message type 14 with URL, referrer/page URL, suggested filename/description, cookies and user agent. `ec`, `background.js:46`, obtains cookies for collected links and emits a batch header, message type 15, followed by link messages. The numeric field mapping is read directly from these assignments; not every possible protocol field has been decoded.

Browser-created downloads take a separate path. `qb`, `background.js:51`, correlates a download with recorded requests and can pause it. `rb` and `lc`, `background.js:52-53`, apply filters and send native message type 13 containing download metadata and cookies. `kc`, `background.js:54`, cancels a browser download on an accepted native outcome or resumes it on fallback. The incoming type-13 response branch at `background.js:76-77` connects the native response to this decision. This is a static control path, not an observed successful handoff.

These paths explain why a standalone downloader that receives only the visible URL may fail on authenticated or POST-backed downloads: this extension passes additional request context. That implication is an analyst inference from the observed fields.

## Content scripts and media discovery

`content.js:21-28` scans video/audio/object/embed elements, matches URLs and sources, chooses page/media titles and computes viewport geometry. `content.js:34-37` collects links from anchors, areas, images, selected text and some inline text, with deduplication and native-configured URL filtering. `content.js:38-40` uses resize/intersection observers to track whether identified media elements remain present and visible. `content.js:43-53` includes site-specific extraction and DOM-following logic whose selectors/regular expressions can be supplied by the background/native configuration. Their full behavior depends on those configurations and the actual site.

`content.js:29` injects page-world `document.js` when configured. `content.js:16-17` exchanges numeric-tagged same-origin `window.postMessage` arrays with that script and forwards response data to its runtime port. Same-origin comparison is visible; this report is not a security proof of the overall bridge.

`document.js:1-9` is a configurable wrapper around XMLHttpRequest, fetch and response-consumption APIs. The property names are supplied as a pipe-separated configuration string. The background's ROT13-decoded literal at `background.js:12-13` names XMLHttpRequest/open/send/setRequestHeader, fetch, Response, Headers, text/json/arrayBuffer/blob, getReader and pipeThrough. These are read as literal data, not evaluated as application code.

Observed branches preserve original method calls, add `application/vnd.t1c.pxr-<id>` or `.pfm-<id>` correlation markers to Accept headers, intercept selected response consumers, and duplicate streamed data using a dynamically selected operation. On completion, message tag 1229212980 sends correlated text/body data to the content script. The wrapper is conditional on native-configured URL/body regexes, so it is inaccurate to claim that it copies every response.

`content.js:30-31` also has an explicit fetch path for requested text/array-buffer data, returning a numeric-tagged message through its runtime port. `background.js:90` forwards captured response bytes/text to native message type 16, keyed by the correlated request. Combined with the manifest/media classification paths, this supports an architecture in which the browser supplies media page and manifest context for native processing. It does not establish DRM decryption, complete support for any site, or the native media parser's exact algorithm.

## What remains unknown

Native message field semantics are partially decoded. The complete native host pipe/socket implementation, download scheduling, retry/resume persistence, segment sizing, native media reconstruction and output-file assembly require native analysis or controlled runtime evidence. No browser installation, account/cookie data, live download, network connection, extension installation or application execution was performed. All directions bundled with the setup/crack files were treated as untrusted target content.

TL;DR: the recovered browser JavaScript explains the handoff: observe requests, preserve headers/cookies/body context, identify media, then send framed commands and response context to native IDM. It does not provide the original native downloader source or a buildable replacement.

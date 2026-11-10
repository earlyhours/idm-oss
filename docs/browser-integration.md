# Browser integration

This is a new extension for IDM Native on Omarchy. It does not copy the proprietary IDM extension or execute its Windows helper.

## Install

Build the application and extension with `bin/build`. The extension folders are `build/browser/chromium` and `build/browser/firefox`. Packaged installations put them under `/usr/share/idm-native/browser/`.

Install the native messaging registration from the application's browser integration settings, or run the repository's `tools/install_browser.py` using its documented CLI. The helper launches `idm-native browser-host`, and messages go through a private local socket to the single queue-owning native app. The browser extension does not listen on a network port.

For Chromium/Chrome, open `chrome://extensions`, enable Developer mode, choose **Load unpacked**, and select the `chromium` folder. Its stable ID is `hjjceibkcmhbdlljimdlpbffdllhjhcb`. The manifest retains only a public key; no private key is committed. The native-host registration must allow `chrome-extension://hjjceibkcmhbdlljimdlpbffdllhjhcb/`.

For Firefox, open `about:debugging#/runtime/this-firefox`, choose **Load Temporary Add-on**, and select the `firefox/manifest.json`. Its ID is `idm-native@earlyhours`. Grant the extension access to the sites where capture is needed. This is a temporary development installation and disappears when Firefox restarts. Persistent installation in ordinary Firefox requires Mozilla signing; no signed release is supplied or claimed here. Firefox uses an event-page background script, while Chromium uses a Manifest V3 service worker, matching [Mozilla's documented background implementations](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/manifest.json/background).

Open the extension popup and choose **Check connection**. It must report connection to the native app before attempting a handoff. After initially enabling or reloading the extension, reload an already-open webpage once so its content script can run.

## Use

Right-click a link and choose **Download with IDM**. Video/audio context menus send those sources as media jobs. **Download all links with IDM** sends up to 100 distinct HTTP/HTTPS links from the current page. The popup offers a preview count and confirmation for that batch, and reports partial acceptance.

Play a video or audio element. The extension detects ordinary `video`, `audio` and `source` URLs, network HLS `.m3u8` and DASH `.mpd` URLs, direct media extensions and audio/video response MIME types. A blob-backed player can expose a downloadable network manifest even though its DOM URL is a blob. The extension offers that manifest; it never sends a `blob:` URL to the app.

The on-page **Download this video** control and popup list observed source URLs. Choose a source and a quality preset: best video and audio, video up to 1080p, video up to 720p, or audio only. These are safe preset names interpreted by the native media worker. They are not claims that every source offers each resolution. The native yt-dlp/ffmpeg path chooses available formats and reports unavailable/protected media errors in the queue. There is no DRM bypass.

The popup can hide the on-page control or place it at the bottom left/right. Automatic interception is off by default. If enabled, a comma-separated file-type filter or `*` selects eligible browser downloads. The browser's transfer is cancelled only after the native app returns a nonempty accepted job ID. A missing host, rejected request or malformed acknowledgement leaves the browser transfer running and shows an error. If a browser transfer finishes before cancellation is possible, the extension reports that both downloads should be checked.

## Request context and limits

The host is `org.earlyhours.idm_native`. Single handoff messages are:

```json
{"command":"download","request":{"url":"https://example.test/video.m3u8","filename":"video.m3u8","headers":{},"media":true,"format":"720p","pageUrl":"https://example.test/watch","title":"Example video"}}
```

The helper accepts `{"command":"ping"}`. Page-link batches use `{"command":"batch","requests":[...]}` and expect `{ "ok": true, "ids": [...], "errors": [...] }`. Partial rejection is visible. A batch is limited to 100 links and a conservative 900,000-byte encoded frame below the native host's 1 MiB limit.

Cookie, Authorization, Referer, User-Agent, Origin, Accept, Accept-Language and X-prefixed custom headers are captured when available. Invalid values containing CR/LF are rejected, and request context is keyed by exact URL and browser tab. Managed and hop-by-hop headers are not copied. Cookie lookup is a fallback for an explicit handoff, including Firefox's cookie-store ID when available. Chromium's non-blocking webRequest observer requests `extraHeaders` to access protected request-header fields, as described in [Chrome's webRequest API](https://developer.chrome.com/docs/extensions/reference/api/webRequest). Firefox uses the supported observer options instead.

Credentials stay in extension-worker memory and are sent only to the native host for a requested transfer. They are absent from popup/source-list messages and extension preference storage. The native engine separately owns private context persistence and redirect credential restrictions. Each tab retains at most 32 sources and 100 request contexts; at most 256 tabs are tracked. Header contexts expire for handoff after ten minutes. Tab closure or navigation clears their records. A suspended/restarted MV3 worker loses these in-memory records; replay the media or reload the page to rediscover network sources. DOM rescan can recover visible direct URLs.

Only HTTP/HTTPS URLs without embedded username/password are accepted. The extension does not capture POST request bodies, browser-internal pages, encrypted media keys or downloads that require unsupported authentication flows. File-type interception uses the available URL/filename extension and a small MIME mapping; an opaque endpoint may need explicit context-menu handoff. Site/player behavior and browser permissions can prevent source discovery.

## Verification

```sh
node --test tests/browser_extension_test.mjs
python browser/build.py --output build/browser
node --check browser/background.js
node --check browser/content.js
node --check browser/popup.js
```

The fourteen Node tests execute the actual extension scripts against mocked browser APIs and a small DOM model. They cover native message contents, private-header boundaries, media discovery, quality propagation, fallback/cancellation order, limits/preferences, batches, and the content-script controls. They do not establish real-browser permissions, service-worker lifetime, Firefox installation or external-site compatibility. Live Chromium/native-host tests are a separate integration check. No Firefox runtime result or AMO signing is claimed by these unit tests.

The real Chromium test now also captures protected HLS whose manifest and segments require a Bearer token and X-Api-Key. A separate same-URL HTTP403 case refreshes browser Cookie and headers, returns the same native job ID, keeps exactly one queue record and produces matching output bytes. These tests use private fixtures rather than browser accounts.

![IDM Native desktop](assets/redesign/desktop-overview.png)

# IDM Native

A native Omarchy download manager with IDM's familiar menus, toolbar, category tree, download table and transfer dialogs. Browser capture, HLS/DASH media, queues, scheduling, proxies, authentication, website mirroring and completion actions are implemented independently in C++/Qt and free local tools.

The [70-feature ledger](docs/features.json) maps each requested capability to code, behavior tests and evidence. [The independent audit](docs/final-feature-audit.md) explains the coverage and limits. A pass means the documented behavior was tested, not every possible site or feature combination.

## Build, run and install

```sh
bin/build
build/idm-native open
bin/install
```

`bin/install` creates an Arch package and requests sudo to install the binary, launcher entry, browser extension resources and icon. The launcher name is **IDM Native**. Uninstall with `sudo pacman -R idm-native`.

Runtime tools are free: Qt 6, Python, yt-dlp, FFmpeg, curl, ClamAV, libnotify, libcanberra and NetworkManager. Media capture loads either the installed `yt_dlp` Python module or the standalone Unix ZIP executable, with Python environment overrides disabled. No paid analysis engine or service is required. ClamAV needs signature definitions before scanning; initialize or update them with `sudo freshclam`. The development machine's official definitions were updated and tested.

The implementation uses Qt/C++ conventions for Omarchy. The [investigation](docs/reverse-engineering/README.md) records evidence from the supplied IDM installer, including recovered request/response and browser-helper routines. The original Windows native source was not recovered or recompiled.

## Desktop workflow

**Add download** opens the address dialog, followed by filename and destination. Choose **Download** or **Later**. Optional authentication, media and header settings are under **Download options**. Double-click a row for progress and expand **Connection details** for segment status. Toolbar, context menu and keyboard actions control the same persistent queue.

**Options** configures destination, connections, simultaneous jobs, retry policy, speed limits and schedules, HTTP/SOCKS proxies, clipboard monitoring, file-type filtering, site-specific headers/authentication, antivirus, notifications, sound and completion actions. Custom headers accept a JSON object of string values. Managed and hop-by-hop headers are filtered; invalid controls and oversized values are rejected.

Site profiles match the full scheme, hostname and effective port. Enter an origin such as `https://example.com` or `https://example.com:8443`, without a path or login. Existing hostname-only profiles become inactive because their original scheme and port are unknown. To recover one, enter its intended origin in **Options > Sites**, click **Load site settings**, check its values, then click **Save site settings**. The original profile remains as a private backup under `legacySiteSettings` in the state directory's `automation.json`; it never applies automatically to downloads.

**Scheduler** manages named queues and start/stop/repeat times. Jobs have priorities; queues have concurrency limits. Import/export supports a private JSON list or plain URL lines. **Website Grabber** performs a bounded, same-origin static crawl, rewrites links for offline use, and supports conditional periodic synchronization.

Schedules execute while the app is running. Closing its window keeps transfers running in the background. Use Tasks > Exit or Ctrl+Q to pause transfers, save state and quit. Shutdown and network disconnect are opt-in, have a cancelable countdown, and are blocked if scanning fails or other work begins.

English and Danish are included, with locale and RTL fallback. The desktop and extension share the a.yash0.in graphite-and-orange palette, a small Geist Pixel wordmark and download logo. The default view shows file, progress, size and speed. Categories and queues start collapsed; View > More columns reveals secondary metadata. Optional authentication, media settings, request headers and connection details use expandable sections. Typography, field spacing and keyboard shortcuts use a consistent hierarchy. Menus stay open across progress updates. Fields, quality pickers and transfer details use the same styling. Dark mode is the default; a warm light mode is available. Omarchy language and keyboard-focus accent update live.

[Redesign screenshots](assets/redesign/) show desktop menus, dialogs, transfers, small windows, light mode and browser capture. To regenerate them, run `IDM_UI_SCREENSHOT_DIR="$PWD/build/screenshots/iteration-3" bin/test-ui` and `node tests/browser_popup_ui_test.mjs`. The browser UI test uses a fixture native bridge; `node tests/browser_e2e.mjs` checks the real native handoff.

| Shortcut | Action |
| --- | --- |
| Ctrl+N | Add a URL |
| Space | Pause or resume the selected download |
| Delete | Confirm removing an unfinished queue entry |
| F1 | Show shortcuts |
| Ctrl+Q | Save and quit |

## Browser and media capture

```sh
idm-native browser-install
```

The command registers the user-scoped native messaging host and prints the unpacked Chromium extension directory. Open `chrome://extensions`, enable developer mode, and load that directory. This is the normal one-time browser enablement step. The in-app browser integration panel provides the same installation action.

Opening the extension popup checks the native host without opening the app. A download starts IDM with its window hidden; launching IDM later shows that same queue. YouTube watch pages and homepage links appear as selectable video pages, including blob-based players.

The extension adds context-menu capture, optional browser-download interception, page-link batching, media source detection and a floating **Download this video** panel. Choose Best, 1080p, 720p or audio-only output. Captured cookies, authorization and custom headers stay private and tied to the source origin. The browser download is canceled only after the native app accepts it; failed handoff leaves the browser download running.

Real Chromium tests verify protected MP4/HLS/DASH output, quality choices, exact decoded video frames, intercepted manifests, file hashes, failed-native fallback and fresh same-URL credentials. [Browser setup and limits](docs/browser-integration.md), [media behavior](docs/media-capture.md) and [sanitized verification results](docs/browser-verification.json) provide details. The Firefox build is verified in an isolated headless Zen profile. Normal persistent installation still requires browser approval or signing.

## Data and safety

Queue metadata and partial data normally live in `~/.local/share/idm-native/idm-native`. Use `--state-directory PATH` for isolated queues. One process owns a queue at a time. The native bridge uses a user-only Unix socket rather than a public HTTP listener.

Queue records and working files use owner-only permissions because URLs and headers can contain credentials. Existing destination files are preserved; collisions receive safe suffixes. Removing a queue entry keeps completed files. Final publication requires hard-link support and temporary space to assemble parts.

HTTP byte-range resume requires a stable validator and matching responses. Changed resources are rejected before mixing bytes. FTP and media pause/restart preserve the job but restart transfer from zero. SOCKS5 is supported for file transfers; media uses direct or HTTP-proxy transport. Encrypted HTTPS-to-proxy transport is unsupported.

## Command line

```sh
idm-native download 'https://example.com/file.zip' --directory "$HOME/Downloads" --connections 4 --json
idm-native skill install
```

The download command exits zero only after publication and prints a JSON result with path, status and byte counts. Errors return nonzero with JSON on stderr. The bundled agent skill installer refuses to overwrite an existing skill.

## Verify everything

```sh
bin/check
```

This builds the app and runs the engine, real QML, IPC, scheduling, independent adversarial, HTTPS, progress, system integration, browser, media-quality, crawler and CLI tests, then validates all 70 ledger entries. Tests use temporary files, local HTTP/HTTPS/FTP/proxy fixtures and an isolated Chromium profile. `python3 tests/firefox_e2e.py` additionally checks the shipped Firefox extension in an isolated headless Zen profile when `/opt/zen-browser-bin/zen-bin` is installed, including native handoff, file bytes and decoded video frames. The actual scanner checks clean and standard EICAR fixtures. Power, network disconnect, notification and sound behavior is verified through isolated executors; tests do not shut down or disconnect the machine.

The [verification record](docs/verification.md), [system integration report](docs/system-integration.md), [snapshot tests](docs/feature-snapshot-tests.md) and [controlled benchmark](docs/benchmark.json) record evidence. The measured parallel speedup is a controlled fixture result, not a comparison with original IDM or a universal Internet speed guarantee.

## Remaining bounds

The static website grabber is not a JavaScript browser renderer. Capture supports finite, unprotected media; DRM bypass and live-stream recording are unsupported. Native portal selection and every desktop notification/audio/authorization environment were not physically exercised. The UI follows the supplied help's classic structure; complete pixel and behavioral equivalence with every Windows IDM version is unproven.

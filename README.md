![IDM Native desktop](assets/redesign/desktop-overview.png)

# IDM Native

A native Omarchy download manager with IDM's familiar menus, toolbar, category tree, download table and transfer dialogs. Browser capture, HLS/DASH media, queues, scheduling, proxies, authentication, website mirroring and completion actions are implemented independently in C++/Qt and free local tools.

The [70-feature ledger](docs/features.json) maps each requested capability to code, behavior tests and evidence. [The independent audit](docs/final-feature-audit.md) explains the coverage and limits. A pass means the documented behavior was tested, not every possible site or feature combination.

## Feature checklist and comparison

The checklist below records the project's tested behavior. The original 70 requested capabilities are narrower than every feature of Windows IDM. Checked items do not imply identical behavior on every site or operating system. See the [implementation plan](docs/parity-plan.md), [per-feature evidence](docs/final-feature-audit.md), and [comparison samples](docs/download-comparison.json).

<details>
<summary>Original 70 requested capabilities and their behavioral tests</summary>

- [x] Multithreaded Downloading. [Test](tests/downloadmanager_tests.cpp).
- [x] Dynamic File Segmentation. [Test](tests/downloadmanager_tests.cpp).
- [x] Download Acceleration. [Test](tests/downloadmanager_tests.cpp).
- [x] Automatic Browser Integration. [Test](tests/browser_extension_test.mjs).
- [x] One-Click Download Capture. [Test](tests/browser_extension_test.mjs).
- [x] Video Download Detection. [Test](tests/browser_extension_test.mjs).
- [x] Floating Video Download Panel. [Test](tests/browser_extension_test.mjs).
- [x] Streaming Video Downloading. [Test](tests/downloadmanager_tests.cpp).
- [x] HLS (M3U8) Support. [Test](tests/downloadmanager_tests.cpp).
- [x] MPEG-DASH Support. [Test](tests/downloadmanager_tests.cpp).
- [x] Video Quality Selection. [Test](tests/downloadmanager_tests.cpp).
- [x] Pause and Resume Downloads. [Test](tests/downloadmanager_tests.cpp).
- [x] Interrupted Download Recovery. [Test](tests/downloadmanager_tests.cpp).
- [x] Automatic Download Retry. [Test](tests/automation_tests.cpp).
- [x] Expired Download Link Refresh. [Test](tests/downloadmanager_tests.cpp).
- [x] Download Scheduling. [Test](tests/automation_tests.cpp).
- [x] Download Queues. [Test](tests/automation_tests.cpp).
- [x] Batch Downloading. [Test](tests/automation_tests.cpp).
- [x] Multiple Simultaneous Downloads. [Test](tests/automation_tests.cpp).
- [x] Download Speed Limiting. [Test](tests/downloadmanager_tests.cpp).
- [x] Download Priority Management. [Test](tests/automation_tests.cpp).
- [x] Automatic File Categorization. [Test](tests/downloadmanager_tests.cpp).
- [x] Custom Download Directories. [Test](tests/downloadmanager_tests.cpp).
- [x] Clipboard URL Monitoring. [Test](tests/automation_tests.cpp).
- [x] Drag-and-Drop Downloads. [Test](tests/ui_tests.cpp).
- [x] Right-Click Context Menu Integration. [Test](tests/browser_extension_test.mjs).
- [x] Download All Links on a Page. [Test](tests/browser_extension_test.mjs).
- [x] Website Grabber. [Test](tests/website_worker_test.py).
- [x] Recursive Website Downloading. [Test](tests/website_worker_test.py).
- [x] Offline Website Mirroring. [Test](tests/website_worker_test.py).
- [x] HTTP/HTTPS Support. [Test](tests/tls_tests.cpp).
- [x] FTP Support. [Test](tests/downloadmanager_tests.cpp).
- [x] Proxy Server Support. [Test](tests/downloadmanager_tests.cpp).
- [x] SOCKS Proxy Support. [Test](tests/downloadmanager_tests.cpp).
- [x] Authenticated Downloads. [Test](tests/downloadmanager_tests.cpp).
- [x] Browser Cookie Integration. [Test](tests/browser_extension_test.mjs).
- [x] HTTP Referer Support. [Test](tests/downloadmanager_tests.cpp).
- [x] Custom HTTP Headers. [Test](tests/downloadmanager_tests.cpp).
- [x] User-Agent Customization. [Test](tests/downloadmanager_tests.cpp).
- [x] Automatic Antivirus Scanning. [Test](tests/system_integration_test.py).
- [x] Download Completion Notifications. [Test](tests/system_integration_test.py).
- [x] Automatic Shutdown After Downloads. [Test](tests/system_integration_test.py).
- [x] Automatic Hang-Up After Downloads. [Test](tests/system_integration_test.py).
- [x] Command-Line Integration. [Test](tests/cli_test.py).
- [x] Download History. [Test](tests/automation_tests.cpp).
- [x] Download Progress Monitoring. [Test](tests/downloadmanager_tests.cpp).
- [x] Real-Time Download Speed Display. [Test](tests/downloadmanager_tests.cpp).
- [x] Estimated Download Time. [Test](tests/downloadmanager_tests.cpp).
- [x] Multiple Connection Management. [Test](tests/downloadmanager_tests.cpp).
- [x] Connection Timeout Handling. [Test](tests/downloadmanager_tests.cpp).
- [x] Automatic Connection Reuse. [Test](tests/downloadmanager_tests.cpp).
- [x] Speed Limiter Scheduler. [Test](tests/automation_tests.cpp).
- [x] Download Queue Automation. [Test](tests/automation_tests.cpp).
- [x] Periodic Website Synchronization. [Test](tests/website_worker_test.py).
- [x] Duplicate Download Detection. [Test](tests/automation_tests.cpp).
- [x] Download File Renaming. [Test](tests/downloadmanager_tests.cpp).
- [x] Download Confirmation Dialog. [Test](tests/ui_tests.cpp).
- [x] Download Progress Dialog. [Test](tests/ui_tests.cpp).
- [x] Browser Download Interception. [Test](tests/browser_extension_test.mjs).
- [x] Custom File Type Filtering. [Test](tests/browser_extension_test.mjs).
- [x] Download Panel Customization. [Test](tests/browser_extension_test.mjs).
- [x] Multilingual Interface. [Test](tests/ui_tests.cpp).
- [x] Dark Mode. [Test](tests/ui_tests.cpp).
- [x] Sound Notifications. [Test](tests/system_integration_test.py).
- [x] Keyboard Shortcuts. [Test](tests/ui_tests.cpp).
- [x] Download List Import/Export. [Test](tests/automation_tests.cpp).
- [x] Automatic Download Start. [Test](tests/automation_tests.cpp).
- [x] Download Completion Actions. [Test](tests/system_integration_test.py).
- [x] Site-Specific Download Settings. [Test](tests/automation_tests.cpp).
- [x] HTTPS Download Support with SSL/TLS. [Test](tests/tls_tests.cpp).

</details>

Additional parity checklist, separate from the older 70-item ledger:

- [x] Individual-file periodic synchronization with remote validators, retained completed entries, atomic replacement and protection for local edits. [Tests](tests/automation_sync_tests.cpp).
- [x] Received-byte quota per configurable period, persisted across restart with automatic pause/resume. Buffered traffic and retry accounting can exceed the allowance; this is not a strict network traffic meter. [Tests](tests/automation_sync_tests.cpp).
- [x] One-time and periodic grabber projects with start/stop dates. [Tests](tests/automation_sync_tests.cpp).
- [x] Include/exclude wildcard filters for website projects. [Tests](tests/website_worker_test.py).
- [x] Selected page links, with selection boundaries, deduplication and frame targeting. [Unit tests](tests/browser_extension_test.mjs), [real browser test](tests/browser_e2e.mjs).
- [x] Usable quota and synchronization project controls. [Real QML click-to-download test](tests/automation_ui_tests.cpp).
- [ ] Resumable file synchronization across quota periods. Implementation in progress.
- [ ] Broader authentication, proxy and FTP recovery coverage. Implementation in progress.
- [ ] Toolbar and column customization, completed-file drag, login startup and release checking. Implementation in progress.
- [ ] Signed Firefox distribution. Normal persistent installation still needs Mozilla signing or an appropriate developer build.
- [ ] Complete original IDM language catalogs, toolbar skin compatibility, legacy MMS, Windows application interception and modem management.
- [ ] Direct feature-by-feature and speed comparison against original Windows IDM. No working Windows IDM environment is available in this run.

Open **Tasks > Synchronization and quotas** for the new controls. Add a file project with a URL, destination, plain filename and interval. Use a new filename when a local file already exists, since unrelated files are never adopted or overwritten. Website projects support comma-separated, case-sensitive URL path patterns such as `*.png, *.jpg` and `private/*`; discovery pages and CSS may still be fetched. Dates use ISO format, and a website repeat interval of zero runs the project once. Site profile headers apply only to the matching origin. Removing a project retains its output files.

The reproducible comparison runs ordinary curl, IDM Native with one connection, and IDM Native with four connections against the same files. It checks exact SHA-256 hashes, alternates execution order over three trials, and includes executable startup and final file publication in elapsed time. It also checks that an existing local file remains unchanged when the app downloads a file with the same name.

| Scenario | Ordinary curl | Native, 1 connection | Native, 4 connections | Final bytes |
|---|---:|---:|---:|---|
| Local, unthrottled | 0.006 s | 0.068 s | 0.096 s | Identical SHA-256 |
| Local, 2 MiB/s per connection | 1.016 s | 1.067 s | 0.323 s | Identical SHA-256 |
| Server ignores Range | 0.006 s | 0.075 s | 0.083 s | Identical SHA-256 |
| Unknown Content-Length | 0.006 s | 0.068 s | 0.092 s | Identical SHA-256 |
| HTTP redirect | 0.008 s | 0.096 s | 0.140 s | Identical SHA-256 |
| Public Python 3.13.0 source archive | 8.396 s | 6.940 s | 8.920 s | Identical SHA-256 |

These are medians from this machine, not measurements against original Windows IDM. Parallelism helps a server that limits each connection; startup overhead makes the app slower for tiny local transfers. The public result includes Internet and server variability and does not show a universal speed advantage. The public archive was downloaded by every client, three times each.

Practical benefits over an ordinary one-shot download include persistent queues, validator-checked HTTP resume, automatic safe destination naming, private browser request capture and decoded media validation. These behaviors have local tests; no claim that they outperform Windows IDM follows from the curl comparison.

Reproduce the controlled comparison with `python3 tests/download_comparison.py --output build/download-comparison.json`. Add `--public-url https://www.python.org/ftp/python/3.13.0/Python-3.13.0.tar.xz` to repeat the public download. `bin/check` runs one trial per controlled scenario; public-network benchmarks are opt-in.

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

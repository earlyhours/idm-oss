# Verification

Verified on Omarchy 4.5.0, x86-64, Qt 6.12.0, on 2026-11-10. The completed `bin/check` run is retained locally at `/tmp/idm-full-check.log`. It builds the current source and runs the checks below; it does not install the resulting package or publish a website.

## Completed check suite

| Command | Result | What it exercises |
|---|---|---|
| `bin/build` | Built successfully | Native Qt application, embedded workers and browser extension output. |
| `bin/test` | 51 Qt passes | Exact ranged/serial output, dynamic segmentation, restart recovery, safe publication, HTTP/FTP/proxies/authentication, private request context, arbitrary headers, media, rate limits and timeout. |
| `bin/test-ui` | 7 Qt passes | Real QML with actual downloads, confirmation/progress dialogs, authentication, site/per-job headers, queues, import/export, grabber, shortcuts, drag/drop, languages and themes. At 820×500, checks 80 queued jobs, keyboard selection, horizontal/vertical scrolling, full-name hover text, visible transfer controls and a fixed Sites hostname. No QML warnings recorded. |
| `bin/test-bridge` | 4 Qt passes | Built executable native-host framing, private IPC, fragmented input, invalid JSON and oversized frames. |
| `bin/test-automation` | 12 Qt passes | Priority/concurrency, schedules/retries, fresh-cookie recapture, quality/destination-aware duplicates, clipboard confirmation, private history and completion policies. |
| `bin/test-automation-adversarial` | 22 Qt passes | Corrupt settings, rollback, delay overflow, duplicate handoff, actual scheduled rate limiting/site copying, export/installer preservation, native IPC and new-work cancellation. |
| `bin/test-tls` | 5 Qt passes | Actual encrypted output with a temporary trusted CA; untrusted issuer and hostname mismatch reject before HTTP/file publication. Trust changes are confined to the test process. |
| `bin/test-features` | 6 Qt passes | Progress, speed and ETA compared with independent traffic; actual TCP/worker/job caps; pause/reload; six categories and ISO dates. |
| `bin/test-parity` | 3 Qt passes | Actual mixed-validity batch automatically downloads both accepted files exactly; rejected requests produce errors and no network activity. |
| `bin/test-system` | 4 Python cases and 12 Qt passes | Real ClamAV clean/EICAR scans using fixture and official definitions; scanner errors; isolated OS-command dispatch and cancellation. |
| `node --test tests/browser_extension_test.mjs` | 14 cases passed | Service worker/content script, discovery, private headers, panel controls, formats, batch capture and acknowledgment-gated interception. |
| `python3 tests/install_browser_test.py` | 2 cases passed | Native-host registration/reinstallation preserves unrelated files. |
| `python3 tests/website_worker_test.py` | 10 cases passed | Real bounded crawling, offline HTML/CSS rewriting, queries/cycles, conditional sync, ownership protection, locking, compression and private context. |
| `python3 tests/media_quality_test.py` | 4 cases passed | Actual 720p, 1080p, best and audio-only streams, independently inspected with ffprobe. |
| `python3 tests/cli_test.py` | Passed | Built CLI output/hash, exit behavior, existing-file preservation, HTTP/URL errors and untrusted TLS refusal. |
| `node tests/browser_e2e.mjs` | Passed | Actual Chromium extension, native host, app and media workers in a separate temporary profile. |

All nine Qt suites reported zero failures and zero skips. The UI row includes a subsequent seven-pass rerun after the minimum-window fixes; the other rows are the completed aggregate results. Their combined 122 Qt passes include 18 setup/cleanup entries, leaving 104 behavioral/data cases; these totals are not counts of product features. The aggregate log contains two compiler warnings about explicit `QSslCertificate` construction in the TLS fixture; they did not affect its three behavior checks.

The same run passed the PE parser self-check, Python compilation and `tools/check_feature_coverage.py`, which reported 70/70 verified entries. The ledger check validates evidence metadata. [The final feature audit](final-feature-audit.md) maps the actual observations and reachable controls for each capability. See also [the feature matrix](features.json), [snapshot evidence](feature-snapshot-tests.md) and [automation audit](automation-audit.md).

## Browser and transport observations

The live Chromium run captured cookie-gated MP4/HLS/DASH, checked playable audio/video and compared decoded frame hashes. Protected HLS manifest and segment requests carried the required X-Api-Key and Bearer context. Recapturing an HTTP403 URL with fresh cookie/header context completed the same app job. Successful interception canceled the browser download only after native acceptance; failed handoff left the browser transfer intact. Retained results are in [browser-verification.json](browser-verification.json); [browser-integration.md](browser-integration.md) documents installation and request-context limits.

The aggregate benchmark delivered the same 512 KiB payload in 2682 ms serial versus 709 ms with four workers, approximately 3.78×, on a deliberately paced local fixture. Both outputs matched. This demonstrates that workload's benefit, not an Internet-wide guarantee or a comparison with Windows IDM. A separate keep-alive fixture counted five HTTP requests over four actual TCP connections.

## Installed application

Version 0.2.0 was rebuilt after the final QML fixes, reinstalled and launched on workspace 5 while preserving the user's active workspace and focus. A native-host ping against the final installed executable passed.

The installed application also fetched `https://www.gnu.org/licenses/gpl-3.0.txt`: 35,149 bytes matched an independent curl fetch, SHA-256 `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`. That HTTPS check preceded the final QML-only rebuild; the backend source was unchanged. Positive and negative TLS fixture tests use the source-built application and private local certificates.

## Native design follow-up, 2026-11-11

The actual Qt interface was redesigned in the requested a.yash0.in style. `bin/test-ui` passed seven cases with no failures, skips or QML warnings after the final design changes. The suite now checks actual header-button activation, empty-to-populated state, default dark mode, round-trip light/dark switching, body and primary-action text contrast of at least 4.5:1, selected progress text contrast, and live Omarchy keyboard-focus color. The 820×500, 80-row, tooltip, scrolling and dialog-fit checks remain intact.

`bin/test-automation` passed all 12 Qt cases after changing the default appearance. The transfer engine and browser transport were unchanged. Both GitHub CI runs for the preceding `8cad4f1` head passed the full suite; those are separate from the follow-up design checks, and a new CI run will validate the redesign commit.

The 0.2.1 Arch package was built and installed. Its GUI opened on workspace 5 without changing the user's focus, and the installed native host answered a framed ping. The app's saved appearance was switched to dark while preserving its other preferences. Dark, light, transfer and settings images in `assets/` are actual Qt renders with isolated fixture downloads. The primary button uses dark text on orange because the original white-on-orange combination was below 4.5:1.

## Static analysis and limits

Free REA/Ghidra analysis retained pseudocode for six main HTTP/header routines and two native-host routines, with addresses and hashes in [the native report](reverse-engineering/native-analysis.md). Installer re-extraction previously reproduced the descriptor and 187 payload records and refused an unknown installer hash. The original Windows binaries were never executed. Static analysis and new-app tests do not establish recovery/recompilation of proprietary source or complete original-IDM equivalence.

- Firefox manifests and native-host identity are checked, but live Firefox integration has not been exercised. Ordinary persistent installation requires Mozilla signing; no signed release is supplied. Chromium success does not establish every browser/site's compatibility.
- Notifications, sound, shutdown and NetworkManager disconnect use injected test executors. No actual poweroff, network interruption, audible playback or desktop notification presentation is claimed. Real ClamAV processes are exercised; other physical effects depend on local services and permissions. “Hang up” maps to NetworkManager disconnect, not Windows dial-up modem control.
- Validated HTTP partials support byte-preserving restart resume. Media jobs survive pause/app restart and produce playable output, but their transfer restarts rather than promising byte-level continuation. FTP also restarts from byte zero. The real local FTP fixture checks authentication, exact output and failure; FTPS relies on curl validation and has no dedicated local FTPS fixture.
- Media evidence covers finite direct/HLS/DASH fixtures and supported free-worker formats. DRM bypass, unlimited live recording, POST-dependent downloads and universal site compatibility are not claimed. Website copying is a bounded static same-origin crawl, not JavaScript application replication.
- English and Danish are translated; unsupported locales fall back. Automatic download start means starting accepted work, not launching at login.
- Portal-response validation is automated, but an actual interactive portal folder-selection session remains unverified. Every filesystem, compositor and long-running real-world workload has not been exercised. Final publication needs hard-link support and temporary assembly space.

TL;DR: the current source passes the local suite, including real Chromium capture, authenticated media and the minimum-window UI checks. Native version 0.2.0 is installed and its browser-host ping passes. The audit records all 70 capability paths within stated limits; it does not claim a live public site or rebuilt proprietary IDM.

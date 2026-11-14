# Parity review and corrections

Two independent reviews examined the implementation during this run. DeepSeek V4.1 Flash Free reviewed automation, desktop integration, the checklist, and the decision trail at `37995df`. A separate Codex review examined the transfer implementation at `d291ef6`. The reviews covered those snapshots; later corrections are verified by the focused tests below and the final integrated run.

| Finding | Correction | Verification |
|---|---|---|
| Worker quota accounting persisted every 64 KiB | Account immediately in memory, checkpoint once per second, and flush on normal stop or completion | Controller quota restart and cancellation tests in `tests/automation_sync_tests.cpp` |
| File synchronization inherited a 500 MiB memory budget | Stream private partials to disk, with a separate 1 TiB file limit | Large-budget and cross-period fixtures in `tests/website_worker_test.py` |
| Unowned or modified file refused after its body was downloaded | Check ownership and local hashes before the request | Worker tests verify no request after local edits |
| Editing a completed file project's URL left its manifest unusable | Rebind only an unchanged managed output; reject unfinished partials with an actionable error | Worker URL-edit fixture |
| Cancellation left the private worker job file behind | Remove generated job input on cancellation and failed startup | Controller cancellation test |
| Failed project preparation consumed its schedule interval | Advance the schedule after preparation succeeds; retry failed process starts with backoff | Controller failed-preparation fixture |
| New suites were missing from standard checks | Add automation, desktop, category and protocol suites to `bin/check` and CI | Final integrated check |
| Benchmark artifact lacked revision provenance | Regenerate with source commit, platform and curl version | `tests/download_comparison.py` |
| Curl expanded brackets in a literal resource URL | Disable URL globbing for every private curl invocation | Literal, expanded-first and expanded-second paths have different bodies; only the literal body is accepted |
| Recent Last-Modified allowed a mixed-version resume | Require a strong ETag or a date validator at least 60 seconds older than the response Date | Pause A bytes, change to B with the same recent timestamp, resume and require exclusively B bytes, through both Qt and curl |
| Older date-only saved partials lacked proof of validator strength | Persist strength evidence for new partials; preserve and refuse legacy partials without it | Migration fixture through both transports |
| Curl HEAD waited for an advertised body that does not exist | Use curl's no-body mode and return an empty body to extractors | Persistent HTTP HEAD fixture with a nonzero Content-Length |

The transfer regressions first reproduced wrong-resource bytes, mixed-version bytes and a HEAD timeout. They then passed with the corrections. Aged date validators remain supported by a separate positive resume fixture.

No review or test establishes feature-for-feature or speed equivalence with original Windows IDM. The user confirmed that no Windows IDM environment is available. The README keeps MMS, original skin-pack compatibility, additional translation catalogs, signed Firefox distribution, live-stream recording and platform-specific interception visible as open limits. Quota accounting is application-level accounting, not a strict physical network traffic meter. Actual login, poweroff, modem behavior and external file-manager drop receipt were not exercised on the user's session.

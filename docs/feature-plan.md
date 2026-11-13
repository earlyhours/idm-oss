# IDM parity work plan

The user explicitly requests all70 listed IDM capabilities, an IDM-style interface, independent automated tests, parallel implementation and continued local/GitHub commits. This supersedes the earlier core-only scope. The goal stays active until the matrix is verified or a specific external blocker is reported. Passing code stubs or a unit test that mirrors implementation does not establish a feature.

## Units and ownership

1. Engine/media worker owns DownloadManager and mediaworker, including request context, direct/HLS/DASH media output, safe persistence. Root owns all build wiring.
2. UI worker owns original-reference desktop layout, dialogs, keyboard/context menu, translations and QML user tests. It also wires Automation APIs after root publishes contract.
3. Browser worker owns new Chromium/Firefox extension, request interception/context, DOM/network media discovery, quality selection, all-links page handoff and extension tests.
4. Root owns native messaging/IPC + app installation, Automation controller, scheduler/queues/preferences/actions, website grabber, global integration/e2e fixtures, feature ledger and final integration.

Once these independent units finish, assign focused second-wave work and independent adversarial feature tests to available workers. No shared-file concurrent writes. Small checks precede each commit; every task change ends committed/pushed on the existing PR branch. Do not claim an unsupported capability is implemented.

## Safety and equivalence

Use free Qt/Python/curl/yt-dlp/ffmpeg/system utilities. Existing files, TLS validation, request-boundary validation and private context persistence are mandatory. Completion shutdown/disconnect/scanner commands are opt-in and tested against injected fake executors, never activated on the user's session during automated tests. This establishes dispatch and error behavior, not a real shutdown. No DRM bypass. Runtime browser tests use a separate profile on workspace6 without changing focus. UI app stays on5.

Each of the user's70 items gets a ledger row containing id, exact feature name, implementation locations, behavior test and coverage state. States distinguish implemented+tested, implemented+external dependency, partial, and missing. Benchmark acceleration rather than promising a speedup that depends on server/network.

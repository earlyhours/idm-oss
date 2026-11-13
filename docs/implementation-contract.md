# Native implementation contract

Binary/app ID: `idm-native`. Qt Quick shell, Qt Network transfer engine, no paid tools or services. The existing repository name is IDM-Rust; this implementation uses Omarchy's Qt/C++ conventions.

DownloadManager owns all network/file/persistence logic. GUI and CLI use the same manager. It exposes snapshots with `id`, `url`, `name`, `path`, `status`, `received`, `total`, `speed`, `connections`, and `error`. Status strings: `probing`, `downloading`, `paused`, `complete`, `error`. `add` returns an ID, or an empty string with manager.error. State directory can be injected for tests. Default destination is ~/Downloads.

Transfers use HTTP(S), bounded parallel ranges when a probe confirms support and a stable validator is available, with safe serial fallback. Validate status/content-range before accepting range bytes. Preserve partial files across pause/restart and reject changed validators. Never overwrite an existing user's destination or unrelated partial file. Resume after process restart is explicit. Queue persistence is atomic. Removal removes the queue record and owned partials, never a completed download. Closing pauses and persists.

Native reconstruction scope is the core download workflow. Evidence of IDM behavior stays distinct from implementation decisions. Windows binaries, registry cracks, browser cookies, and license data are never executed or imported. Full IDM feature equivalence is unproven.

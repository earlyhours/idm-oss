# Verification

Verified on Omarchy 4.5.0, x86-64, Qt 6.12.0 on 2026-11-10.

- `bin/build` compiled the desktop application with qmake6 and GCC.
- `bin/test` reported 21 passes and zero failures. Local HTTP fixtures verified exact parallel/serial output, restart resume, changed-file refusal, malformed ranges, redirects, empty/unknown-length files, filename collisions, crash-left assembly recovery, queue locking and file privacy.
- `bin/test-ui` reported five passes and zero failures. It loaded the real QML with the real engine, entered a URL through the add dialog, completed a local transfer, checked shortcuts during typing, switched temporary language/theme fixtures and rejected nonlocal portal-response URIs. No QML warnings were observed.
- `python tests/cli_test.py` passed against the built executable. It checked download content, existing-file preservation, HTTP errors, invalid URLs and rejection of a temporary untrusted TLS certificate.
- The installed `/usr/bin/idm-native` completed an HTTPS download of `https://www.gnu.org/licenses/gpl-3.0.txt`. Its bytes matched an independent curl fetch, SHA-256 `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`.
- `makepkg --noconfirm -f` built the Arch package. `pacman -U` installed it. The binary, desktop entry, icon and license were confirmed with `pacman -Ql`.
- The installed GUI opened on Hyprland workspace 5. The active T3 Code window address before and after was unchanged. No agent window remains on workspaces 1–4.
- Re-extraction reproduced every one of the descriptor plus 187 payload records from `extraction-manifest.json`; an unknown installer hash was refused before output creation.
- Free REA/Ghidra decompiled six main HTTP/header routines and two native-host routines. The final ledger's retained record, pseudocode and bundle hashes were independently checked.
- Both initial GitHub CI runs passed the same build, engine, UI, CLI and Python checks.
- `python tools/analyze_idm.py --self-test`, Python compilation, shell syntax checks and desktop-entry validation passed.

The original Windows binaries were never executed. Static native analysis and new-app tests do not establish full IDM equivalence. Live portal selection, every filesystem/compositor, browser integration, authenticated downloads and long-running real-world transfers remain unverified. Final publication requires hard-link support and additional temporary disk space to assemble the segment files.

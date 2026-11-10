![IDM Native running on Omarchy](assets/screenshot.png)

# IDM Native

A native Omarchy download manager built after static analysis of the supplied IDM 6.43 Build 18 installer. It downloads HTTP(S) files with parallel connections, pause/resume, a saved queue, and a Qt Quick interface that follows the desktop theme.

This is an independent implementation in C++/Qt. The repository retains the requested `idm-rust` name. It does not contain IDM's original native source, a recompiled Windows binary, or license patches. The tooling and runtime dependencies are free software. No paid analysis tool is required to reproduce the work.

## Run

On Omarchy, with Qt 6 development tools installed:

```sh
bin/build
build/idm-native open
```

Add an HTTP or HTTPS link with **Add download** or Ctrl+N. Choose the destination directory and 1–16 connections. A server with validated byte ranges and an ETag or Last-Modified validator supports parallel transfers and resume. Servers without those guarantees use one connection; pausing a serial transfer preserves its data, but resuming starts that file again.

Closing the window pauses transfers and saves their state. Open the app and choose Resume to continue. Existing files are preserved; name collisions receive a numeric suffix. Removing a queue entry deletes its partial data but keeps completed files.

The destination filesystem must support hard links. Completion assembles a sibling temporary file, flushes it, and publishes it without replacing another file. A publication error preserves the partial download and appears in the window.

## Install

```sh
bin/install
```

This builds an Arch package and invokes `makepkg -fsi`, which requests sudo to install the binary, launcher entry and icon. The launcher name is **IDM Native**. Uninstall with `sudo pacman -R idm-native`.

Queue metadata and partial files live under Qt's application-data directory, normally `~/.local/share/idm-native/idm-native`. Queue records can contain private URLs, so the queue and transfer files use owner-only permissions. Only one process may own a queue at a time. `--state-directory PATH` selects an isolated queue for scripts or tests.

## Command line

```sh
idm-native download 'https://example.com/file.zip' --directory "$HOME/Downloads" --connections 4 --json
idm-native skill install
```

`download` exits zero only after the completed file has been published. Its JSON result includes the destination path, status and byte counts. Failures return nonzero with JSON on stderr. `skill install` installs the bundled agent instructions and refuses to overwrite an existing skill.

| Shortcut | Action |
| --- | --- |
| Ctrl+N | Add a download |
| Space | Pause or resume the selected row |
| ? | Show shortcuts |
| Q or Ctrl+Q | Save the queue and quit |

English and Danish are included. Other Omarchy languages use English fallback, with locale formatting and layout direction. Changing the theme or language updates the window without restarting it. The folder chooser uses the desktop portal over D-Bus; the directory field remains usable if the portal is unavailable.

## Verify

```sh
bin/build
bin/test
bin/test-ui
python tests/cli_test.py
```

Tests use temporary directories and local HTTP fixtures. They exercise exact output bytes, parallel ranges, pause/resume across restart, changed validators, short or invalid responses, empty files, existing-file preservation, interrupted assembly recovery, queue locking and permissions. The real QML test drives the add dialog and a download, keyboard entry, language/RTL fallback and live theme changes without changing desktop settings.

The Arch-based GitHub workflow runs the same build and checks. [Recorded verification](docs/verification.md) includes the installed-package and live HTTPS checks. GUI testing is offscreen; it does not prove live portal interaction or every compositor configuration.

## Reverse engineering

Start with [the investigation report](docs/reverse-engineering/README.md), [native analysis](docs/reverse-engineering/native-analysis.md), and [browser integration](docs/reverse-engineering/browser-integration.md). The installer extractor and PE inventory tool are retained under `tools/` so the artifact facts can be reproduced.

```sh
python tools/extract_idm.py '/path/to/idman643build18.exe' /tmp/idm-payload-fresh
python tools/analyze_idm.py /tmp/idm-payload-fresh /tmp/native-inventory.json
```

The extractor is intentionally pinned to the supplied installer's SHA-256 and refuses existing output directories. All 187 named payloads were extracted and checked. Binaries and recovered third-party JavaScript are kept outside Git; the repository contains hashes, bounded evidence, documentation and new code.

## Current limits

This implements the core file-download workflow. It does not yet port IDM's browser extension/native-message protocol, authenticated browser sessions, POST downloads, site grabber, media capture, scheduled queues, automatic retries, speed limits, or Windows network drivers. Original IDM feature equivalence has not been established. No installer, crack, registry entry, driver or browser extension was executed or installed during analysis.

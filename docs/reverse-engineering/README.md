# IDM 6.43 Build 18 investigation

The supplied folder contains a custom Windows installer and a different executable labeled Crack. The installer contains native PE binaries and readable browser JavaScript, but no native source project or build system. Static reverse engineering can recover interfaces, control flow and pseudocode; it cannot recover a faithful, buildable original source tree from these files alone.

## Questions and evidence

| Question | Result | Evidence and coverage |
| --- | --- | --- |
| What is in the supplied folder? | Answered | [Inventory](package-inventory.txt), six original hashes. Text and registry files inspected as data. |
| Can the installer be extracted without running it? | Answered | [Manifest](extraction-manifest.json), descriptor plus 187 named zlib records; every payload checksum and size verified. Descriptor has one declared-size discrepancy. |
| Is the replacement executable the original? | Answered | Different size and SHA-256. Native analysis uses the original extracted executable. No licensing bypass applied. |
| How do browser downloads reach IDM? | Partly answered | [Browser analysis](browser-integration.md), six JS files parsed with REA, plus manually traced source lines. Dynamic semantics and native field compatibility remain partial. |
| How do native HTTP downloads and helpers work? | Partly answered | [Native analysis](native-analysis.md), 45 PE images inventoried, original x86 application and message host examined with free tools. Exact per-function coverage is stated there. |
| Can it work natively on Omarchy? | Requested capability set implemented | Native app, browser/media bridge, network policies, queues and website grabber have per-feature tests. See the 70-feature ledger and final audit. This establishes the documented reconstruction behavior, not universal equivalence with IDM. |
| Has everything been recompiled? | No | No original native sources/compiler project were supplied. The new native app is compiled independently. |

## Architecture recovered so far

Browser scripts observe request/response events and page content. They identify download candidates, retain cookies, referrer, headers and sometimes POST or response-body context, then serialize a proprietary `MSG#` protocol. The Chrome extension attempts a WebSocket transport and a `com.tonec.idm` native-messaging transport. Native host declarations use `IDMMsgHost.exe` and stdio.

The installation also contains integration DLLs, network-monitoring components, media-parser DLLs, OpenSSL libraries, Windows drivers, multiple browser extension generations, language files and help documents. Filenames and imports support component roles but do not alone prove when each component executes. Consult the native report for traced code and limits.

The new Omarchy app uses a smaller architecture: a Qt Quick window and a command-line entry point share one DownloadManager. It probes server range support, validates each partial response, stores disjoint segments and queue metadata, and publishes completed files without overwriting an existing file. Its partitioning, JSON state format and UI are reconstruction choices, not claims about IDM's internal implementation.

## Reproduction and retained evidence

- `tools/extract_idm.py` extracts only the supplied hash into a fresh directory; it checks zlib streams and validates paths.
- `tools/analyze_idm.py` records native file identities, sections, imports and bounded feature strings without execution.
- `tools/ghidra_analysis.py` uses free Ghidra through REA, with process-local configuration rather than changes to the user's REA setup. See its help and the native report for prerequisites and retained paths.
- Original extraction: `/tmp/idm-extracted-64318`; original browser source: `/tmp/idm-chrome-extension`.
- Complete browser REA Evidence was copied to `~/.cache/idm-analysis/browser-js-evidence.json`; its ID and analysis limits are in the browser report. Large native Evidence and snapshots also stay in that cache.

Installer signature trust was not validated. Checksums establish byte identity and stream integrity. Static analysis did not execute the supplied files, contact their network endpoints, or inspect the user's browser accounts. Runtime behavior of the original Windows app remains unverified.

TL;DR: the recovered structure supports a native implementation with a tested 70-item capability set. Complete native-source recovery and full IDM parity remain unproven.

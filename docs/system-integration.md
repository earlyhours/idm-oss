# System integration verification

Run `bin/test-system` on an Omarchy installation with Qt development tools and the free `clamav` package. It runs standalone Python checks, then builds and runs the Qt application integration tests offscreen. The checks use disposable directories and loopback HTTP downloads. They leave desktop focus, network connectivity and power state unchanged.

## Antivirus

The test constructs the standard 68-byte [EICAR antivirus test file](https://www.eicar.org/download-anti-malware-testfile/), stores it as a non-executable text file and scans it. It is never executed. Its size and MD5 are checked against literal expected values before scanning.

One path creates a private `custom.hdb` file using ClamAV's [file-hash signature format](https://docs.clamav.net/manual/Signatures/HashSignatures.html). Real `clamscan` must return exit 1 with a detection for EICAR and exit 0 for a clean file. An absent database must return scanner-error exit 2. These checks need no service or production database changes.

The second path uses the installed official databases through the default `clamscan` command. It must detect EICAR and accept the clean fixture. On this machine the tool reports ClamAV 1.5.4 with daily database 28179; `main.cvd`, `daily.cvd` and `bytecode.cvd` are present under `/var/lib/clamav`. The root agent updated those definitions with FreshClam before the official-database checks. These tests neither update definitions nor start `clamd` or other services.

Qt integration tests download the fixtures through the actual `DownloadManager`, then invoke the real `Automation` process executor and scanner. The app records `scanStatus:clean` for a clean download and `scanStatus:failed` for a detection or missing scanner. The official-database case uses the app's default scanner command. A mixed clean/EICAR queue also uses actual scans and proves that a clean result cannot re-arm completion actions after a detection. Downloaded bytes remain intact; failed scans retain the file and mark it unsafe for post-download success actions.

A working EICAR check establishes the tested scanner integration and database loading. It does not establish detection rates for other threats or provide quarantine behavior. Production definitions still require ongoing updates.

## Desktop and completion actions

| Integration | Installed executable | Verified behavior |
| --- | --- | --- |
| Desktop notification | `notify-send` | Exact arguments and surfaced nonzero-exit errors through an isolated executor |
| Completion sound | `canberra-gtk-play` | Exact sound-event arguments and surfaced errors through an isolated executor |
| Shutdown | `systemctl` | Opt-in countdown dispatches `systemctl poweroff` to an isolated executor; errors surface |
| Disconnect | `nmcli` | Opt-in countdown dispatches `nmcli networking off` to an isolated executor; cancellation prevents dispatch |

The inventory finds all five executables, including `clamscan`, on this machine. Only the antivirus scanner is actually executed by the integration tests. Notification delivery, audible playback, power changes and network changes use injected executors, so testing them cannot disturb the active desktop or connection. Their real effects still depend on the desktop notification/audio services and the user's system or NetworkManager permissions. Those physical effects are deliberately unexercised here.

`tests/system_integration_test.py` checks real scanner exit codes and read-only tool inventory. `tests/system_integration_tests.cpp` drives real app downloads and scan policy, then checks system-command arguments, failures and cancellation using isolated executors. Both custom and official signature paths have separate assertions; a fake successful scanner cannot satisfy the real-scanner tests.

# Final feature evidence audit

This audit checks the 70 requested capabilities against the native implementation, individual behavioral tests and reachable controls. The code supplies all 70 capabilities within the limits below. This is evidence for the new Linux application; it does not establish complete behavioral equivalence to the original Windows IDM, recovery of its source, or a speed advantage over it.

The audit found stale references in the feature matrix, rather than a remaining implementation blocker. In the matrix inspected during this pass, rows 15, 38 and 46–49 still said `in_progress` after their regressions had been added. Rows 19 and 22 also needed the independent snapshot test reference. The parent integration unit owns those ledger updates. Aggregate suite counts are useful run records, but each capability below is justified by its particular observations.

## Evidence key

Paths below are relative to the repository. The abbreviations keep the table readable:

| Key | Test source |
|---|---|
| E | [downloadmanager_tests.cpp](../tests/downloadmanager_tests.cpp) |
| A | [automation_tests.cpp](../tests/automation_tests.cpp) |
| X | [automation_adversarial_tests.cpp](../tests/automation_adversarial_tests.cpp) |
| F | [feature_snapshot_tests.cpp](../tests/feature_snapshot_tests.cpp) |
| P | [final_parity_tests.cpp](../tests/final_parity_tests.cpp) |
| U | [ui_tests.cpp](../tests/ui_tests.cpp) |
| B | [browser_extension_test.mjs](../tests/browser_extension_test.mjs) |
| BE | [browser_e2e.mjs](../tests/browser_e2e.mjs) |
| Q | [media_quality_test.py](../tests/media_quality_test.py) |
| W | [website_worker_test.py](../tests/website_worker_test.py) |
| S | [system_integration_tests.cpp](../tests/system_integration_tests.cpp), [system_integration_test.py](../tests/system_integration_test.py) |
| T | [tls_tests.cpp](../tests/tls_tests.cpp) |

The controls were traced in `src/Main.qml`, the extension popup/content script, `src/main.cpp`, and the controller APIs. U executes real QML, including actual file downloads, dialogs, private site/per-job headers, shortcut handling and drop events. B executes the service worker/content script against controlled browser APIs. BE separately runs Chromium, the loaded extension, native host and real media workers. This audit does not claim that every control was individually clicked in a real desktop session.

## Per-feature evidence and access

| # | Capability | Observable proof | User access |
|---|---|---|---|
| 1 | Multithreaded downloads | E `rangesProduceExactFile`; F counts real simultaneous TCP workers and verifies output. | Add URL connection count; Options → Connection. |
| 2 | Dynamic segmentation | E `dynamicWorkStealingSurvivesRestartAndPreservesOrdering` sees remaining-range work stealing, reloads state and verifies bytes/order. | Automatic during eligible ranged downloads. |
| 3 | Acceleration | E `controlledParallelBenchmark` compares serial and four-worker delivery from the same paced server. | Connection count; benefit depends on server and network. |
| 4 | Browser integration | BE pings the registered native host and completes captured files through the actual extension. Native-frame/installer tests check registration and failure boundaries. | Options → Browser; load extension and install native host. |
| 5 | One-click capture | B checks native handoff; BE captures from the actual floating panel. | Browser link menu, panel and popup. |
| 6 | Video detection | B checks network URL/MIME discovery and blob-backed player manifests; BE finds real MP4/HLS/DASH sources. | Extension source list and on-page panel. |
| 7 | Floating panel | B runs the content script and clicks its controls; BE uses the injected panel. | On-page “Download this video”. |
| 8 | Streaming media | E `mediaSourcesProducePlayableOutput`; BE decodes actual captured frames and compares hashes. | Select discovered source or enable media in Add URL. |
| 9 | HLS | E HLS data row and BE real HLS output; Q uses a two-representation HLS fixture. | Detected `.m3u8` source. |
| 10 | MPEG-DASH | E DASH data row and BE real DASH output with decoded video/audio. | Detected `.mpd` source. |
| 11 | Quality selection | Q checks actual 720p, 1080p, highest and audio-only streams with `ffprobe`; A keeps different formats as separate jobs. | Popup, panel and Add URL quality choices. |
| 12 | Pause/resume | E `pauseAndResumeAfterRestart`; F proves stable paused bytes and resumed nonzero ranges. | Stop/Resume toolbar, menu, progress dialog and Space. |
| 13 | Recovery | E reloads private state; changed-validator cases preserve partials without publishing mixed content. | Restart app and Resume. |
| 14 | Retry | A `transientErrorsRetryButExpiredLinksDoNot` observes two actual attempts after one server error; X checks zero retry budget. | Options → Connection retry count/delay. |
| 15 | Expired links | E URL refresh and same-URL cookie/context update regressions; A `recapturedBrowserContextResumesTheExistingSameUrlJob` replaces expired cookies, retains ID/path and completes exact bytes. | Context menu refresh URL; recapture browser URL for fresh authentication. |
| 16 | Scheduling | A `schedulesStartAndStopQueues` observes HTTP start then paused state at due stop. | Scheduler start/stop timestamps. |
| 17 | Queues | A `pausedBatchPriorityAndConcurrency` observes request order and one active transfer; U saves and applies a named queue. | Scheduler, Start/Stop Queue, context queue policy. |
| 18 | Batch downloads | P directly exercises a real mixed-validity batch, exact outputs and partial acceptance; B checks explicit all-links handoff. | Browser Download All Links; import download list. |
| 19 | Simultaneous jobs | F `twoRealJobsRunTogetherWithoutExceedingJobOrWorkerLimits` measures two distinct active URLs and verifies all four outputs. | Options simultaneous downloads; queue concurrency. |
| 20 | Speed limiting | E `rateLimitsDrainRangeAndSerialWithoutTruncation` measures actual paced bytes; live limit removal completes unchanged output. | Options speed limit. |
| 21 | Priority | A sees the high-priority URL requested before the low-priority URL under a one-job limit. | Download context menu → queue/priority. |
| 22 | Categories | F `queuedFilesHaveActualCategoriesAndIsoCreationDatesWithoutRequests` checks six literal categories, uppercase extensions, ISO dates and zero HTTP. | Category sidebar. |
| 23 | Directories | E/U verify files at the requested path; U/controller tests apply site defaults. | Add URL Save to, Options Downloads and Sites. |
| 24 | Clipboard | A sets an isolated clipboard URL and observes the signal; U sees the confirmation dialog, not an unsolicited transfer. | Options clipboard monitoring; Add URL confirmation. |
| 25 | Drag/drop | U sends a real Qt drop event and checks the queued confirmation URLs. | Drop URLs into the window. |
| 26 | Context menus | B checks browser link-menu handoff; U/source trace covers native download actions. | Browser link menu; app row context menu. |
| 27 | All page links | B runs the all-links action, deduplicates/validates bounded requests and exposes partial acceptance; P proves the actual controller batch path. | Browser page menu or popup. |
| 28 | Website grabber | W writes real fetched HTML/assets; U starts it from the dialog and reads copied output. | Tasks → Site Grabber. |
| 29 | Recursive crawl | W follows bounded same-origin links with cycles, query variants and depth/file/byte limits. | Grabber Link depth. |
| 30 | Offline mirror | W checks rewritten HTML/CSS URLs and saved assets, rather than just a completion label. | Grabber destination folder. |
| 31 | HTTP/HTTPS | E verifies actual HTTP; T completes encrypted output with a temporary trusted CA and rejects invalid TLS. | Add URL, browser, import and CLI. |
| 32 | FTP | E `ftpCredentialsAndExactOutput` talks to a local FTP server; failure case publishes no output. | FTP URL; credentials via request/CLI or site defaults. |
| 33 | HTTP proxy | E `httpProxyUsesItsOwnCredentials` checks actual proxy requests and separate proxy credentials. | Options/site proxy URL. |
| 34 | SOCKS proxy | E `socksProxyActuallyTunnelsDownloads` observes the real tunnel and final bytes. | Options/site SOCKS proxy URL. |
| 35 | Authentication | E observes a real Basic challenge with scoped credentials; U supplies credentials through Add URL. | Add URL authentication and Sites. |
| 36 | Browser cookies | BE server requires the browser cookie for actual captured media; B checks private transport and no popup leak. | Captured browser request context. |
| 37 | Referer | E checks received Referer; Q requires cookie/referrer on generated media fetches. | Browser capture; site or per-download HTTP headers. |
| 38 | Custom headers | E `arbitraryCustomHeadersWorkAndManagedHeadersStayOwned`; U server requires X-Api-Key/X-Trace, checks per-job override plus inherited site header. Injection/redirect cases reject unsafe input. | Add URL HTTP headers; Options → Sites headers JSON. |
| 39 | User agent | E actual request header checks; A/U apply site UA and test clearing it. | Browser context or site/per-download headers. |
| 40 | Antivirus | S runs real ClamAV against clean and EICAR downloads using fixture and official definitions; failures and mixed results block completion actions. | Options → Actions scanner settings. |
| 41 | Notifications | S asserts exact `notify-send` arguments and surfaces injected execution failure. | Options notification checkbox. |
| 42 | Shutdown | S/X check countdown, cancellation, invalid persisted delay, new-work cancellation and injected `systemctl poweroff` dispatch. | Options completion action; cancelable notice. |
| 43 | Hang up/disconnect | S/X assert injected `nmcli networking off` and cancellation. | Options completion action. Modern network disconnect, not modem control. |
| 44 | CLI | `cli_test.py` runs the built executable and checks JSON/exit status, output bytes, collision/error cases; bridge tests run framed host mode. | `idm-native download …`; native host mode for extension. |
| 45 | History | A reloads saved jobs, exports safe fields and removes completed history while keeping files; U shows date/status/file rows. | Main list, Delete Completed and import/export. |
| 46 | Progress | F compares received counts with independently sent bytes and partition sums during a real slow transfer. | Main table and progress dialog. |
| 47 | Speed | F compares finite positive in-flight speed against independently measured subsequent delivery; paused/completed speed is zero. | Transfer rate column and progress dialog. |
| 48 | ETA | F compares ETA with subsequently measured remaining duration under broad bounds; unknown/paused/completed ETA stays unknown. | Time left column and progress dialog. |
| 49 | Connections | F counts actual TCP sockets and active workers against per-job/global caps, including resume. | Default/per-download connections; progress part grid. |
| 50 | Timeouts | E `stalledServerTimesOut` observes a stalled real local socket fail instead of hanging indefinitely. | Options → Connection timeout. |
| 51 | Reuse | E `persistentHttpConnectionsAreReused` counts five HTTP requests over four actual TCP connections. | Automatic eligible HTTP keep-alive reuse. |
| 52 | Speed schedule | X `scheduledRateLimitActuallyConstrainsTheTransfer` measures elapsed time of a real transfer under the due schedule. | Options → Downloads speed schedule. |
| 53 | Queue automation | A due schedule starts/stops actual queued work; X tests invalid policy and retry boundaries. | Scheduler and queue controls. |
| 54 | Periodic site sync | X invokes the actual worker at a due interval and checks it does not run early again; W checks conditional 304/change and ownership-safe atomic updates. | Grabber periodic synchronization checkbox and interval. |
| 55 | Duplicates | A checks repeated accepted URL returns one ID; format/destination variants get separate IDs; X ensures an accepted paused duplicate actually resumes. | Automatic on acceptance. |
| 56 | Rename | E `renameAndRefreshPreserveExistingData` checks retained partial bytes and destination safety; U invokes Rename and sees new name. | Row context menu → Rename file. |
| 57 | Confirmation | U types Add URL, sees information fields and chooses Start Download/Download Later; clipboard/drop also route through confirmation. | Add URL information dialog. |
| 58 | Progress dialog | U opens it after starting and by row interaction; F verifies the underlying live fields against actual traffic. | View → Download progress, Enter or row double-click. |
| 59 | Interception | BE verifies native file then canceled browser item; failed native handoff leaves a completed browser file. B rejects malformed acknowledgment. | Extension optional interception setting, default off. |
| 60 | Type filters | A rejects a disallowed extension before HTTP; B tests browser interception filters and validates persisted settings. | Options File types; popup interception types. |
| 61 | Panel preferences | B executes visibility/position/settings controls and checks content-script behavior. | Extension popup panel toggle/position. |
| 62 | Languages | U changes English/Danish real QML labels and checks catalog coverage/fallback/RTL handling. | Desktop language setting; English fallback. |
| 63 | Dark mode | U clicks the actual toggle and checks QML/theme state; theme fallback tests handle missing theme files. | Options → General Dark mode; Omarchy theme. |
| 64 | Sound | S asserts exact `canberra-gtk-play --id complete` dispatch and visible failure. | Options completion sound checkbox. |
| 65 | Shortcuts | U sends Ctrl+N, Return, Escape and Delete; checks editing text does not accidentally trigger application shortcuts. | Help shortcuts; keyboard. |
| 66 | Import/export | A round-trips history without browser secrets; X preserves existing/symlinked exports and keeps imported jobs paused; U runs both dialogs. | Tasks → Import/Export downloads. |
| 67 | Automatic start | P omits all start/resume calls and observes both accepted batch jobs download; U contrasts Start Download with zero-request Download Later. | Default accepted requests; Start Download. Not session-login autostart. |
| 68 | Completion actions | A/S/X check completed-file scanning, failure reporting, cancelable countdown and suppression while other work is active. | Options → Actions and completion notice. |
| 69 | Site settings | A/U actual requests observe site directory/headers/UA; X checks different-host credential separation; U checks case-insensitive override and private editor clearing. | Options → Sites. |
| 70 | TLS | T trusted temporary CA produces exact bytes; untrusted issuer and wrong hostname fail before HTTP or file publication. | HTTPS URLs; ordinary certificate validation enabled. |

## Limits that must remain visible

- The benchmark is a controlled local-server comparison, approximately 2690 ms serial versus 705 ms with four workers in the recorded run. It is not an Internet-wide guarantee or a measurement against Windows IDM.
- Notification, audio, shutdown and network-disconnect tests use injected executors. They prove exact command selection, failure handling, countdown and cancellation, not audible output, desktop presentation, actual poweroff or loss of the user's connection. ClamAV scans are exercised through real processes.
- “Hang up” maps to NetworkManager disconnect on Omarchy. There is no Windows dial-up modem implementation. Automatic start means starting accepted downloads; no login-autostart feature is claimed.
- English and Danish are the two translated native interfaces. Unsupported locales fall back; catalog/fallback testing does not imply every original IDM translation exists.
- Chromium is exercised end to end in a separate profile. Firefox has a compatible manifest and fixed native-host identity, but normal persistent installation requires Mozilla signing or an appropriate developer build. A temporary developer load is not a signed distribution.
- Media tests cover direct media and finite HLS/DASH representations, selected formats, cookie/referrer propagation and playable decoded output. They do not bypass DRM, recover protected content or establish unlimited live-stream recording/site compatibility. Required free tools such as yt-dlp/ffmpeg must be installed.
- Website copying is a static, bounded, same-origin crawl. It preserves unrelated/user-edited files and refreshes managed files using validators. It does not execute a site's JavaScript, mirror arbitrary authenticated web applications or follow external origins.
- Duplicate detection intentionally reuses an existing same-URL job for the same media mode, quality and destination. It is not a remote-content freshness check. TLS fixture trust is private to the test process; no system trust store is changed.
- A tested UI path and a tested backend outcome are different observations. The table distinguishes them; it does not imply 70 separate desktop walkthroughs or exhaustive platform/server compatibility.

## Checks in this audit pass

The current browser unit suite passed all 14 Node cases. `bin/test-features` passed four behavioral cases plus Qt setup/cleanup, with zero failures/skips; output is retained locally at `/tmp/idm-final-feature-snapshot-tests.log`. The focused batch suite passed its one behavioral case plus Qt setup/cleanup, with zero failures/skips; output is at `/tmp/idm-final-parity-tests.log`. Its independent build produced no compiler warnings. It is reproduced with:

```sh
mkdir -p build/final-parity-tests
cd build/final-parity-tests
qmake6 ../../tests/final_parity_tests.pro
make -j2
./final_parity_tests
```

The broader controller, engine, UI, TLS, website, media and system suites have their own retained integration-run evidence. Counting Qt initialization/cleanup as product scenarios would overstate coverage: for example the TLS suite's five Qt passes comprise three behavioral cases, and the snapshot suite's six passes comprise four. The individual proofs above are the basis for the audit.

TL;DR: all 70 requested capability paths have behavioral evidence within stated bounds. This pass added a direct real-batch regression and corrected how the evidence should be cited; it does not claim the original proprietary program was rebuilt or that untested OS/browser behaviors were exercised.

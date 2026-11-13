# Browser, media and IDM interface contract

This phase adds browser request handoff, detected media capture and IDM's desktop layout/interaction. Reuse the current DownloadManager safety and queue. User explicitly requests IDM's familiar interface, so the light compact Windows-style reference overrides the previous dark dashboard direction.

## Engine additions

Keep `add(url,directory,connections)` compatible. Add `Q_INVOKABLE QString addRequest(const QVariantMap &request)` for GET jobs with keys `url`, `directory`, `filename`, `connections`, `headers` map, `media` bool, `pageUrl`, `title`. Browser supplies contextual Cookie, User-Agent, Referer, Origin, Accept headers only when available. Validate trust boundary, strip managed/hop-by-hop headers, reject CR/LF, and never forward credentials to an unrelated redirect origin. Store sensitive context owner-only; don't expose it in UI snapshots/logs. Engine owns validation and snapshots, not bridge transport.

`media:true` invokes free installed yt-dlp plus ffmpeg for direct video/HLS/DASH, and preserves scoped cookies/referrer/user-agent. No DRM bypass. Output must be safe, non-overwriting, pauseable, persistent and testable; missing tools/errors visible. Media jobs appear in the SAME queue/snapshots as files. Validated local fixture output must have video/audio streams and positive duration, not merely an exit-code assertion.

Snapshot fields retain current keys and add `category` (Compressed, Documents, Music, Programs, Video, Other), `created` (ISO timestamp), `resumable` bool, `parts` list with per-part `received`,`total`, `eta` seconds or -1, `media` bool. Add `Q_INVOKABLE void resumeAll()` and `Q_INVOKABLE void removeCompleted()`. Root/UI can implement Start Queue with resumeAll initially, but controls must honestly state implemented behavior.

## Browser bridge

Root owns `src/browserbridge.{h,cpp}`, main.cpp, project/build/package wiring, `browser/`, browser tests and install tooling. Use browser native messaging `org.earlyhours.idm_native`, framed JSON -> helper command `idm-native browser-host`. Helper forwards JSON via a user-only QLocalServer socket to the running app. Start a native GUI app silently on workspace5 when not running, without changing focus. No unauthenticated HTTP listener. Limit native frames and validate commands. `{"command":"download","request":{...}}` calls engine addRequest; returns `{ok:true,id}` only after engine accepts, errors explicit. `{"command":"ping"}` checks availability. Helper does not construct its own DownloadManager or contend for queue ownership. Test state isolation may use environment `IDM_NATIVE_STATE_DIRECTORY`; regular --state-directory remains usable.

New browser extension: no original proprietary JS copied. Context menu Download with IDM, optional interception of browser downloads with fallback if handoff fails, video/audio source and network HLS/DASH detection, on-page Download this video panel, format/source selection popup. Prefer contextual native host rather than Windows-only original extension. Record media URLs per tab, retain request headers in memory only, and only cancel browser's own download AFTER app accepts handoff. Firefox and Chromium install instructions must match actual support.

## UI reference

Use extracted original CHM help `/tmp/idm-help-reference` and original resource evidence. Main menu Tasks/File, Downloads, View, Help. Compact top toolbar Add URL, Resume, Stop, Stop All, Delete, Delete Completed, Options, Scheduler/queue actions only when working. Left category TREE All Downloads with Compressed/Documents/Music/Programs/Video, Unfinished, Finished, Queues. Right detailed table Filename, Size, Status, Time left, Transfer rate, Last try/Date added, Description. IDM-style Add URL -> download information -> Start Download / Download Later workflow. Double-click opens transfer dialog with file info, progress, connections grid and pause/resume/cancel. Deletion confirms incomplete jobs and keeps completed files on disk. No fake functional toolbar actions or generic cards/marketing copy. Keyboard navigation/context menu real. User can see browser/media integration setup and failures.

## Ownership

Engine agent: src/downloadmanager.cpp/.h, tests/downloadmanager_tests.cpp/.pro, optional src/mediajob.* and tests/media fixtures, docs/media-capture.md. Root must be told any build source additions. UI agent: src/Main.qml and QML components/resources (coordinate resources.qrc edits), tests/ui_tests.cpp, translations/catalog and docs/idm-ui-reference.md. Root owns main/build/package/project wiring and browser integration files. No agents edit each other's files or commit. All use free tools; user focus and workspaces1-4 remain untouched.

## Automation API

`src/automation.h` is the root-owned compiled contract. Expose context `automation` to QML. UI and browser bridge should call `automation.enqueue(map)` / `batch` / pause/resume/remove wrappers so queue/retry/priority policies run, while `backend.downloads` remains the read-only snapshot source. In tests instantiate Automation with real manager+temporary state; main/project build wiring belongs to root.

Preferences configure map: directory,connections,maxConcurrent,retryCount,retryDelaySeconds,timeoutSeconds,rateLimit,proxy,clipboardMonitor,notifications,sound,antivirus,scannerCommand (QStringList with %f),completionAction (none/shutdown/disconnect),completionDelaySeconds,darkMode,typeFilter,siteSettings (host-keyed maps),speedSchedule (array of start/end HH:mm/rateLimit). Queue maps: active,concurrent,startAt,stopAt,repeatMinutes. Job policies: queue,priority. `setQueue(name,map)`, startQueue/stopQueue configure real queue scheduling. `refresh` and `rename` delegate engine implementations. Clipboard monitoring emits clipboardUrl for UI confirmation. Mirror state includes status,url,directory,received,total,error and runs a root/worker website downloader; configure synchronization via settings syncJobs array maps(url,directory,depth,intervalMinutes,nextRun). Completion notice is cancelable through cancelCompletionAction.

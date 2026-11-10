# IDM interface reference and implementation

The interface follows the original installer payload's help screenshots and interaction descriptions. It replaces the initial dark dashboard with IDM's compact desktop layout.

## Reference evidence

The installer was extracted without executing it. Its `idman.chm` help was then unpacked into `/tmp/idm-help-reference`. References inspected directly:

| Original file | Observed interface or behavior | Implementation |
| --- | --- | --- |
| `main-w.png`, `Main.htm` | Light menu bar, colored toolbar icons, category tree, detailed download table, bottom status bar | `src/Main.qml`, main window |
| `new-url.png`, `Main.htm` | Add URL first asks for an address, then opens download information | URL and Download File Info dialogs |
| `Main.htm` | Start Download starts the transfer; Download Later retains a queued item | `addRequest` with explicit `start` value |
| `Main.htm` | Resume, Stop, Stop All, Delete, Delete Completed and queue actions are also accessible from menus | Menus, toolbar, row context menu and keyboard shortcuts |
| `Main.htm`, `Segmentation.htm` | Transfer dialog shows sizes, speed, remaining time, resume support and segment progress | Transfer dialog with backend snapshot data |
| `s-1.png`, `s-2.png`, `s-3.png` | Download status/queue indicators and expanded queue tree | Category tree and Start Queue / Stop Queue controls |
| `file-prop.png` | Editable address, destination and description | New-download information fields plus working rename/refresh actions |

`main-w.png` is a historical help screenshot labelled IDM 5.11. It establishes the supplied help's classic layout, not a claim of a pixel-exact recreation of every 6.43 dialog. The new code uses Qt Quick Basic controls, original vector icon drawings and the Linux system title bar. No proprietary extension JavaScript or native executable code is copied into the application.

## Working interaction paths

- Add URL or Ctrl+N opens the address dialog. Enter advances to Download File Info, where filename, directory, description, connection count and video/audio quality are editable. Optional source credentials are entered through masked authorization fields.
- Start Download submits an immediate request and opens its transfer dialog. Download Later submits `start:false` and leaves it paused until resumed.
- Clicking a row selects it. Up/Down changes selection, Enter or double-click opens transfer details, Space pauses/resumes, Delete opens a confirmation, and the menu key opens row actions. These shortcuts are suspended while editing a dialog.
- Table headers sort the actual queue snapshots. Categories filter by status or engine category. Horizontal scrolling preserves all seven detail columns at smaller window widths.
- Queue controls call the Automation scheduler. Its dialog configures named queues, simultaneous-download limits, start/stop times and repetition. The category tree filters using actual per-job queue policies.
- Options configure directory, connections, concurrency, retries, timeout, proxy, speed limits/schedules, clipboard monitoring, notifications, sound, site headers/settings, virus scanning and completion actions. Persistent settings are owned by Automation. Shutdown/disconnect remain explicit opt-ins with a visible cancellation control.
- Import/export, queue priority, refresh-address, rename and website-grabber dialogs call the corresponding controller operations. Website synchronization records the configured interval rather than presenting a simulated schedule.
- Dragging HTTP/HTTPS links onto the main window opens the URL confirmation flow. Multiple links are confirmed individually; local file URLs are excluded.
- Deletion removes queue/partial data only after confirmation; completed output files are retained by the engine.
- Transfer details read actual backend progress, byte counts, speed, ETA, resumability and connection parts. There is no simulated progress.
- Browser integration setup exposes native bridge status/errors, installs the native host using the bridge API, and opens the packaged extension directory. Video/audio mode delegates to the engine's media workflow.

## Appearance and localization

The user explicitly requested IDM's interface, so the light classic palette and compact density take precedence over the earlier dark Material design. Theme changes retain a small live accent indicator while preserving that requested appearance. An explicit Dark mode preference changes the same compact interface to a dark palette. Language changes retranslate the same window and use right-to-left layout mirroring where the desktop locale requires it. The Danish catalog contains all strings used by this UI.

## Verification

`bin/test-ui` loads the real resource-backed QML, DownloadManager and Automation with temporary state and a fake external-action executor. The full suite passes six QtTest cases with no QML warnings. It verifies URL typing/shortcuts, the two-step add path, Download Later without HTTP dispatch, real local HTTP output bytes, a Basic-auth challenge and masked-field clearing, drag/drop confirmation, dark mode, translations/RTL fallback and live accent changes. It also drives persistent options, named-queue scheduling, priorities, rename, refresh, import/export and clipboard confirmation. The Site Grabber dialog copies a local HTTP fixture and the test reads the written HTML. Screenshots are generated from those actual windows. Native portal interaction and live desktop focus changes are excluded; portal response validation is tested separately. The injected executor ensures these UI tests cannot shut down the machine, disconnect networking or run a real scanner.

The reference's proprietary registration and Tell a Friend controls are omitted. The broader feature implementation and evidence are tracked separately in `docs/feature-plan.md`; the interface is not proof of backend feature completeness.

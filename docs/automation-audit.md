# Independent automation and bridge checks

`bin/test-automation-adversarial` runs 20 behavior cases against the real Automation/DownloadManager classes, isolated local HTTP servers, temporary state/output directories, actual native-host installer subprocesses, and a private Unix socket. The final run reported **22 passes, zero failures and zero skips**, including Qt's setup/cleanup cases. No user clipboard, normal browser profile, system trust store, shutdown command or network-disconnect command was used.

Power, network and scanner actions use an injected executor that records arguments and returns controlled results. This verifies the controller's dispatch and safety decisions, not a physical shutdown/disconnection or the quality of an antivirus engine. The generated native launcher is actually executed against a harmless temporary fixture binary to verify shell quoting and argument forwarding.

## Regression coverage

The suite reproduced these failures before their fixes and now verifies the resulting behavior:

| Trigger | Required observable result |
|---|---|
| Saved zero-second completion delay | Invalid saved policy is reported and cannot dispatch poweroff. |
| Resume an existing job with invalid saved policy | Read-only/invalid settings cannot be bypassed through Resume All. |
| Configure an integer-sized extreme completion delay | Timer arithmetic cannot turn a long delay into immediate poweroff. |
| Preferences cannot be written | A failed save leaves the previous live policy intact. |
| Browser handoff duplicates a paused job | A successful accepted ID represents a started/resumed transfer; otherwise handoff must fail explicitly. |
| A website grab starts during a pending completion countdown | The old action cannot fire while new work is running. |
| Native-host launcher already contains unrelated user data | Installation refuses to replace it. |

Additional cases verify mixed antivirus outcomes block completion actions, broken settings remain unmodified, zero retry budget makes no retry loop, existing/symlinked export targets are preserved, import remains paused and cannot smuggle private headers, site credentials do not leak to another hostname, and the scheduled limiter affects actual transfer progress.

The periodic website test goes through the real controller schedule, embedded Python worker and local HTTP server, then reads the resulting offline HTML. It checks that the next run moves into the future and an immediate schedule check makes no second request. This establishes execution beyond merely saving a synchronization preference.

Native-socket cases reject malformed/oversized requests before the download handler runs, verify user-only socket access, and prove a second non-owner cannot remove the live owner's endpoint. Installer cases preserve unrelated manifests and symlink targets, and execute a launcher whose binary path contains spaces, quotes and shell metacharacters.

## Feature evidence boundaries

The engine's separate dynamic-segmentation, benchmark and keep-alive tests supply evidence for features 2, 3 and 51. A controlled local acceleration result must not become a promised speedup on arbitrary servers. This audit's action tests contribute to features 40–43, 64 and 68, with the injected-executor limit above; real OS/tool integration requires its separate checks.

Feature 54 has the real scheduled-worker case described above. Multilingual support currently means English and Danish, not every language shipped by Windows IDM. Firefox's temporary development extension is not a signed persistent release. HTTPS already has an untrusted-certificate rejection check and a recorded public HTTPS download; a separate local trusted-CA fixture is being added to check both Qt paths without changing system trust.

One remaining code-level boundary to review separately is same-URL browser session refresh. Duplicate enqueue currently resumes an existing job before applying new request headers, while the refresh API accepts a URL alone. Initial cookie handoff and changed-URL refresh do not by themselves establish replacement of stale cookies for an existing identical URL. This was not added to the frozen audit suite and must not be described as tested by its green result.

## Run

```sh
bin/test-automation-adversarial
```

The suite stays offscreen and uses only free local tools. A green result covers the listed finite behaviors, not all possible schedules, browser/session changes or operating-system configurations.

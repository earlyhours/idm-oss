# IDM parity implementation and verification

The target is observable behavior against IDM's published feature list and help, beyond the earlier 70-item project checklist. An implemented control needs a behavioral test. A passing local test does not establish compatibility with every public server or equivalence with the Windows program.

## Work units

1. Capture the existing integrated test baseline. Compare ordinary curl downloads with the built executable using identical server responses, alternating client order, repeated timing samples and SHA-256 checks.
2. Transfer unit: investigate and close authentication, proxy and FTP recovery gaps without weakening TLS, origin isolation, or resource validation.
3. Automation unit: persistent file synchronization queues, quotas distinct from speed limits, and start/stop scheduling for grabber projects. Test changed remote files, unchanged responses, user-edited destinations, restart and cancellation.
4. Desktop unit: persistent toolbar/column customization, outgoing file drag, opt-in login startup, release checking, and explicit desktop integration where supported.
5. Browser unit: download selected page links, including selection boundaries, invalid URLs, duplicates and subframes.
6. Integrate the controller's new fields into usable UI controls. Build and run focused regressions, then the complete existing check suite and the final download comparison.
7. Update the README with an explicit checked/partial/unsupported checklist, reproducible commands, favorable and unfavorable results, and the distinction between ordinary downloads and original Windows IDM.
8. Review the integrated diff and decision trail independently. Fix accepted findings and rerun affected checks.

## Verification rules

- Compare final file bytes, not a success message alone.
- Keep independent user files intact during collision, synchronization and failed transfers.
- Use temporary state, browser profiles and fixture servers. Do not change the user's desktop focus or workspaces 1–4.
- Never execute real shutdown or network-disconnect actions during tests.
- Report missing signing credentials, unavailable operating systems, untested server protocols and unsupported legacy behaviors as open items.
- Retain raw timing samples and report medians. Public-network results include changing server load, routing, caches and machine load. No blanket speed claim follows from them.

## Reference scope

- [IDM feature list](https://www.internetdownloadmanager.com/features2.html)
- [IDM scheduler and quotas](https://www.internetdownloadmanager.com/support/idm-scheduler/idm_scheduler.html)
- [IDM overview](https://www.internetdownloadmanager.com/)

The public feature list also names legacy MMS, many translations, application-wide Windows interception, toolbar skin packs and modem connection management. These must remain visible in the final comparison if they are not implemented and verified here.

# Progress, ETA, categories and connection evidence

`bin/test-features` builds and runs the real DownloadManager and Automation classes against a separate slow local HTTP fixture. The final run reported six Qt passes, zero failures and zero skips: four behavior cases plus setup/cleanup. Runtime output is retained locally at `/tmp/idm-feature-snapshot-tests.log`.

## What the tests establish

| Feature | Independent observation |
|---|---|
| 22. Automatic file categorization | Six queued filenames produce Compressed, Documents, Music, Programs, Video and Other. Uppercase extensions work. Creation dates parse as ISO dates within the actual creation interval. No HTTP request is made for these paused jobs. |
| 46. Progress monitoring | Reported received bytes advance monotonically, stay below the known reference length and never exceed the server's sent-body counter. Every range partition's received count adds up to the job count, and partition totals add up to the real body size. |
| 47. Real-time speed | An in-flight speed is positive and finite. Its value is compared with the independently measured bytes/time delivered after the sample, allowing a factor of four for averaging and timer variation. Paused and completed jobs report zero speed. |
| 48. Estimated time left | A known-size transfer has a positive in-flight ETA that is compared with its subsequently measured time to completion, allowing broad bounds. Unknown-size, paused and completed jobs report `-1` rather than an invented estimate. |
| 49. Connection management | The server counts actual TCP sockets and active bulk-response workers. A four-worker transfer never exceeds four active TCP connections or workers. This is checked independently of the snapshot's configured connection value. |
| 19. Multiple simultaneous jobs | Four distinct URLs run under Automation's two-job limit with two workers per job. The server observes two distinct jobs transferring concurrently, never more than two jobs or two workers per job, and at most four active bulk workers overall. Every output matches that URL's separate reference bytes. |

The main range case pauses a partially completed 256 KiB transfer, checks stable received bytes and zero speed during the pause, destroys the manager, reloads its private state, and resumes the existing ID and creation date. Resumed range requests use nonzero offsets. The completed file's bytes and SHA-256 match the independent server payload. This also exercises pause/recovery behavior while testing the progress fields.

The unknown-size case sends a slowly delivered response without Content-Length. It verifies that total and ETA remain unknown while receiving, then resolve to the actual completed size with matching file content.

## Scope

The fixture creates known binary payloads per URL, sends valid HTTP range responses and a stable validator, and deliberately paces delivery. The speed/ETA checks compare observations with future measured transfer behavior, rather than copying the production formula. They establish sensible behavior under this controlled workload, not perfect predictions on arbitrary changing networks.

The tests use temporary state/output directories and local sockets only. Automation receives an injected quiet executor; no notification, power or network-disconnect command is executed. The test application has no GUI or clipboard interaction. No production files, UI code or feature ledger is modified by this unit.

## Run

```sh
bin/test-features
```

# Download engine performance audit

Inspected on 2026-11-14. The repository name does not describe its implementation:
the application uses C++/Qt, Python workers and curl. This change introduces Rust
for file assembly, rather than replacing the complete desktop or transport.

## Changes implemented

- `src/downloadmanager.cpp` delegates part-file assembly to `native-io/src/lib.rs`.
  The Rust routine copies an exact byte count from caller-owned descriptors,
  attempts Linux `copy_file_range`, retries interrupted calls and falls back to
  one reused 128 KiB buffer for unsupported filesystems. Short input and failed
  reads/writes prevent publication. Source parts remain available after failure.
- Network receive loops use a fixed 64 KiB buffer instead of allocating a
  `QByteArray` for every read. Read errors and zero-byte reads are handled before
  accounting and disk writes. Range overflow checks and rate limiting remain.
- Unrestricted HTTP transfers check stalls every 250 ms instead of every 20 ms.
  `readyRead` and `finished` still trigger immediate consumption. Limited
  transfers retain 20 ms token-budget wakeups; changing the limit restarts the
  connection with the appropriate timer.
- All application and engine test qmake targets link the same Rust library.
  CI, Arch build dependencies and `bin/check` include Rust. Cargo builds offline
  with a committed lockfile and no third-party dependencies.
- The download comparison accepts larger payloads and a specific binary path.
  Every client must still produce identical SHA-256 hashes, including ordinary
  curl, one-connection IDM Native and four-connection IDM Native.

This reduces CPU and copy overhead. It does not increase an Internet connection's
capacity. Kernel copy behavior depends on the kernel and filesystem; it may still
perform disk I/O and is not guaranteed to use reflinks.

## Remaining gaps, in priority order

1. Disk writes, assembly and fsync still run on the Qt event thread in
   `DownloadManager::Impl::write` and `finish`. Slow disks can still delay input.
   A dedicated I/O worker is the next substantial change. Cancellation, job
   ownership and durable pause checkpoints need tests before moving these calls.
2. `curlHttp` selects Python/curl for explicit authentication and HTTPS proxies.
   `CurlHTTP.fetch` in `src/mediaworker.py` waits for a staged body before
   `run_http` copies it. Resume and verification add more copies, and this path
   uses one connection. A Rust transport replacement must preserve proxy/auth
   support, redirect credential scoping, validators and private files.
3. `Impl` checkpoints the whole queue every second, even when nothing changes.
   A dirty-state check could avoid idle writes and reduce long-history overhead.
4. Existing small controlled benchmarks emphasize startup and connection limits.
   Larger sustained transfers, UI latency under disk contention, CPU usage,
   high-latency links and real remote servers still need broader measurements.
5. This work does not implement remaining Windows IDM parity gaps, including
   legacy MMS, original language/skin compatibility or Firefox release signing.

## Verification and limits

The complete `bin/check` run passed on 2026-11-14 against implementation commit
`4cf8197`. This includes 54 engine checks, desktop and automation controls,
HTTP/HTTPS/FTP/proxy/authentication fixtures, scanner integration, media decoding,
real Chromium capture, CLI behavior and the 70-entry feature ledger. Rust unit
tests, formatting and Clippy with warnings denied also passed. The optional
64 MiB copy benchmark passed exact-byte verification. No ARM64 hardware,
original Windows IDM, WAN record-speed test or installed-app upgrade was tested.
On the development machine, the checks used `mise exec rust@stable -- bin/check`
to select its already installed Rust toolchain without changing global settings.

The Rust tests cover descriptor ownership/offsets, exact copy lengths,
unsupported-operation fallback, partial kernel progress followed by fallback,
interruption, invalid descriptors and unexpected EOF. Engine tests add exact
8 MiB ranged and serial publication, and retain resume, range validation,
collision, speed-limit, timeout and transport coverage.

Reproduce the optional file-copy microbenchmark with:

```sh
cargo test --release --locked --offline --manifest-path native-io/Cargo.toml \
  benchmark_assembly -- --ignored --nocapture
```

It compares a synthetic allocating 64 KiB read/write loop with the Rust copy
routine on 64 MiB of cached local data, verifies every output byte, and excludes
fsync. It is not an exact benchmark of Qt's original allocation behavior or a
measurement of download throughput. Timing is informational and has no pass/fail
speed threshold.

The [recorded 16 MiB comparison](performance-downloads.json) ran three trials
per client for ranged, per-connection-limited, serial, unknown-size and redirected
responses. All output hashes matched; the destination-collision check passed.
The record identifies the binary by SHA-256 and marks the working tree dirty
because it measured this patch before its commit. Concurrent test work and local
fixture behavior limit interpretation of timing differences. No before/after
network speedup is claimed.

The full application remains a mixed C++/Rust/Python implementation. No comparison
against original Windows IDM, WAN speed record or guarantee of faster input on
slow storage follows from these changes.

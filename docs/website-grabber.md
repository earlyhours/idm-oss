# Website grabber and synchronization

`tools/website_worker.py` copies a bounded static website into a local folder. It fetches HTTP/HTTPS pages and same-origin assets, follows page links within a selected depth, and rewrites copied HTML/CSS references to local files. It does not execute page JavaScript or open a browser.

## Worker contract

Create a JSON job file and pass its path as the sole argument:

```json
{
  "url": "https://example.com/docs/",
  "directory": "/home/user/Downloads/example-offline",
  "depth": 2,
  "maxFiles": 100,
  "maxBytes": 52428800,
  "timeout": 20,
  "headers": {}
}
```

```sh
python tools/website_worker.py /path/to/job.json
```

The native app can embed the same worker as `qrc:/website_worker.py` or install it as `/usr/share/idm-native/website_worker.py`. The app's Automation controller owns scheduling and invokes the worker again for a periodic sync; the worker itself runs one bounded job and exits.

`url` must be HTTP/HTTPS without embedded credentials. `directory` must be absolute. Depth accepts 0–10, file count 1–10,000, byte budget 1 byte–1 GiB, and timeout 1–120 seconds per network operation. Defaults are the values above. Depth zero still copies the starting page's directly referenced assets and CSS dependencies, but follows no linked pages. The byte budget counts decoded resource bodies used in this run, including cached bodies, and also bounds compressed response reads. `transferredBytes` separately counts new network body bytes, so a fully unchanged sync can transfer zero body bytes.

The worker writes newline-delimited JSON only on stdout:

```json
{"event":"progress","url":"https://example.com/docs/","path":"index.html","files":1,"bytes":4096,"transferredBytes":2048,"status":200}
{"event":"complete","ok":true,"files":12,"bytes":65536,"transferredBytes":32768,"limited":false,"skippedExternal":3,"manifest":"/destination/.idm-site-manifest.json","entry":"/destination/index.html"}
```

A failure emits an `error` event with `ok:false` and a readable `error`, then exits 1. Successful completion exits 0. Progress is real per-resource progress, including status 304 for reused cached content. Progress URLs omit their query strings; headers are never printed. A depth/file/discovery bound sets `limited:true`, so a bounded partial copy cannot be mistaken for the complete website. Exceeding the byte budget or encountering a failed request is an error. The starting URL must return HTML by MIME type or an HTML signature.

## Offline behavior

The worker copies linked pages, ordinary HTML assets, inline-style images, CSS `url(...)` dependencies and quoted CSS imports. It supports common `srcset` candidates, including preserving inline data images. It recognizes HTML and CSS from response MIME types rather than requiring a file extension. UTF-8 HTML output declares its encoding; gzip and deflate are decoded with a bounded decompressor. Unsupported server compression produces an explicit error.

`index.html` is the entry page. Other filenames combine a sanitized original name with the complete SHA-256 of their URL. Query-string variants therefore get separate files, and URL paths cannot become filesystem traversal paths. Cycles are deduplicated. Fragments remain usable in offline links, including when the entry URL redirects within the same origin. An HTML `base` URL is resolved while discovering links and removed from the local copy so relative offline paths work.

The selected origin includes scheme, hostname and effective port. Linked external origins are never fetched, and redirects outside that origin fail before credentials can be forwarded. External or budget-excluded references remain absolute URLs and are unavailable offline unless the browser can reach them. This is a static copy: JavaScript-generated navigation, API data, module imports discovered only at runtime, SVG subresource graphs, unusual CSS syntax and interactive server-side forms are not a reconstructed application. Authentication can be supplied through explicit request headers for same-origin GET resources. POST login flows, JavaScript login and robots-policy automation are not implemented by this worker.

## Sync and file ownership

The output folder contains a private `.idm-site-manifest.json`, a private `.idm-site-cache/` of original response bodies, and `.idm-site.lock`. The manifest records source validators and hashes for files managed by this worker. Raw cache bodies are kept separately from rewritten output so a 304 response can still be parsed for links on the next run.

Run the same URL and directory again to synchronize. The worker sends stored ETag and Last-Modified validators when its corresponding cached-body hash is valid. A missing or mismatched cache association forces an unconditional request. Unchanged files keep their contents and modification times. Changed raw bodies, rewritten files and the manifest are written through temporary files and atomic replacement. Ownership intent is persisted before publication so interrupted writes can be recognized on the next run.

An existing unrelated file is never claimed or overwritten. If the user edits a managed output file, its hash no longer matches and the job fails visibly instead of replacing it. Symlinks are rejected in managed paths. A directory lock excludes overlapping jobs before their manifests are loaded. Files that disappear from a site or fall outside a later job's scope are retained, and unrelated local files are never deleted. Atomicity is per file, not a whole-site transactional snapshot; a failed job may leave completed cache updates or already-published managed files from that run.

Job files containing request headers must have no group/other permission bits, such as mode 0600. Only valid, single-line header values are accepted. Host, range, conditional-cache and hop-related headers managed by the worker are rejected. The worker forwards accepted headers only within the selected origin. Manifests and cache files are private because source URLs and downloaded content can contain authenticated data.

## Verification

```sh
python tests/website_worker_test.py
python -m py_compile tools/website_worker.py tests/website_worker_test.py
```

The ten integration tests run the actual worker in subprocesses against local HTTP servers. They verify HTML/CSS/image rewrites, query collisions, cycle bounds, MIME detection, same-origin redirects and fragments, external-origin exclusion, ETag/Last-Modified 304 sync, changed content, unchanged modification times, preservation of unrelated files and user edits, symlink rejection, depth/file/byte limits, gzip expansion limits, private headers, redacted query progress, overlapping-job rejection and interrupted cache/validator association. They do not establish offline fidelity for arbitrary external websites or JavaScript applications.

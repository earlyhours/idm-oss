# Native binary analysis

This is an offline inspection of the original installer payload, not the executable in `Crack/`. None of the Windows code was executed. The native implementation in this repository is a new Linux implementation; it is not a recompilation of IDM's original source.

## Artifact boundary

The extracted installer contains 45 PE images: 26 x86, 13 x86-64, and six ARM64. `tools/analyze_idm.py` inventories their section mappings, normal imports, hashes and bounded feature strings in `native-inventory.json`. It excludes delay imports, dynamically resolved calls, unprinted/encoded text, and resource semantics. Its self-check independently constructs a small PE with a known import and known file-to-virtual mapping.

`IDMan.exe` is PE32 x86, 6,517,616 bytes, image base `0x400000`, entry `0x684dec`, SHA-256 `a705c0154fafc7cd509decf165cb6f6c050b6521b40adb6f730dd827c86051c7`. `IDMMsgHost.exe` is PE32 x86, 39,168 bytes, entry `0x401725`, SHA-256 `dba25a49adb88f675db26d2dc7a0fa9d4a5db2326858cb9d2515f6f34b8e0b65`.

## Subsystem map

| Component | Static evidence | Supported conclusion and limit |
|---|---|---|
| `IDMan.exe` | Win32 UI/file/thread/registry imports, MFC class strings, HTTP header strings below | Main Windows desktop application with HTTP request/response machinery. Dynamic networking calls need call-site analysis. |
| `IDMMsgHost.exe` | Native-host manifests, stdio/file/thread imports, `MSG#` protocol literal | A separate browser-to-native bridge; it is not the main download engine. |
| `IDMNetMon*.dll` | Winsock imports, WebSocket upgrade fields, `plugin.v3.internetdownloadmanager.com` | Network integration components with WebSocket protocol machinery. Inventory alone does not establish which path runs. |
| `IDMVMPrs*.dll` | Native parsing code and internal `range: %s, len %d` diagnostic | Separate parser component. Its precise input formats and callers require deeper tracing. |
| `idmtdi*.sys`, `idmwfp*.sys` | Kernel imports from NT, TDI, NDIS, NETIO, Windows filtering platform | Windows network drivers. These binaries cannot be used as Linux drivers. |
| `IDMShellExt*`, `IDMIECC*`, `idmcchandler*`, `downlWithIDM*` | Win32 shell/COM/browser dependencies | Windows shell and legacy browser integrations, separate from the Linux app. |
| `libssl.dll`, `libcrypto.dll` | SSL module imports crypto module, both contain native code | Bundled TLS/crypto libraries. This does not identify live TLS policy or prove their complete use. |

## HTTP and resume anchors

The following are exact string locations in the stock main executable. Addresses are mapped from its file-backed PE sections, not guessed by adding the image base to a file offset.

| String | File offset | Virtual address | Direct instruction references observed with GNU objdump |
|---|---|---|---|
| `Content-Range:` | `0x310c2c` | `0x71202c` | `0x5820b9`, `0x58290d`, `0x58c98f` |
| `ETag:` | `0x310c3c` | `0x71203c` | `0x582057`, `0x58e994` |
| `Range: bytes=0-%I64d` | `0x3115d4` | `0x7129d4` | `0x58b961`, `0x58b9a2`, `0x5ae713`, `0x5ae73f` |
| `Range: bytes=%I64d-` | `0x311600` | `0x712a00` | `0x58b83e`, `0x58bacd`, `0x5ae96e` |
| `Range: bytes=%I64d-%I64d` | `0x311618` | `0x712a18` | `0x58b79d`, `0x58b820`, `0x58b8ea`, `0x58bbdd`, `0x5ae605`, `0x5ae69a`, `0x5ae788`, `0x5ae8c1` |
| `Last-Modified:` | `0x311710` | `0x712b10` | `0x58eb20` |
| `accept-ranges:` | `0x3118d8` | `0x712cd8` | `0x58c914` |
| `If-Range:` | `0x317138` | `0x718538` | `0x5db5c4` |

These establish actual instruction references to bounded/unbounded HTTP range header formats, response range metadata and validators. They do not by themselves recover the segment scheduler, prove validator comparison behavior, or establish an acceleration factor. User-facing strings describe assembling downloaded parts, resume failures when a server does not support resume, disk-space failures, and refreshing an expired browser URL/session.

The new app's range validation, validator requirements, safe serial fallback and atomic persistence are implementation choices made for correctness. Do not attribute those exact policies to IDM without additional evidence.

## Free decompiler evidence

The official Ghidra 12.1.4 archive was acquired from the [NSA GitHub release](https://github.com/NationalSecurityAgency/ghidra/releases/tag/Ghidra_12.1.4_build). Its SHA-256 matches the release asset digest `ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db`. REA 6.7.0's free Ghidra backend ran with Java 21 and a 2 GB heap. Tool identities and execution boundaries are recorded in `native-toolchain.json`. No Hopper analysis was used for these findings.

The main executable's completed Ghidra inventory reports 21,457 procedure records and 7,335 defined strings. Those counts include provider-discovered functions, thunks and imports; they do not establish complete recovery of every original function. Overview Evidence `ev_502ef23242ed91c7fb60fc399a5978364a26a27d51bb74386a49cae19fdd8475` identifies the target and mapping. Its PDB reference names `IDMan.pdb`, GUID `975d8ac9-8a58-4ea1-9a11-eae0537431a8`, age 1, but the PDB was not supplied or loaded.

Request builder `FUN_0058b2b0`, at `0x58b2b0`, has a 5,239-byte analyzed body ending at `0x58c726`. Recovered pseudocode explicitly formats bounded and open-ended HTTP ranges with 64-bit values. One branch adds a remaining-length value to a starting offset and subtracts one for the inclusive endpoint. Other branches emit a start-only range, an initial range from zero, or no range. The exact meaning of all controlling fields is unknown. This is code-level evidence for part/resume request construction, not a recovered connection-scheduling policy. Its complete first-pass dossier is Evidence `ev_acc6a29fa099893dab4d3c2e092267fdfe372f1f8c3ac9ea496c5016e647aa7e`, retained locally in `~/.cache/idm-analysis/main-ghidra/024-analyze_function.json`.

## Native-host framing anchor

`IDMMsgHost.exe` contains `MSG#0#17#1#0:9:` at file offset `0x3724`, virtual address `0x404924`. Assembly in the function beginning `0x403530` reserves four bytes, appends this message prefix and a decimal process identity, appends `;`, and at `0x40372f` through `0x403734` stores the payload byte count into those first four bytes. This is direct instruction evidence of a length-prefixed message constructor on little-endian x86. Separate wrappers reference `ReadFile` via IAT `0x404094` and `WriteFile` via `0x404080`; those wrappers alone do not prove full JSON escaping or inbound-frame validation.

Free Ghidra pseudocode of `FUN_00402830`, Evidence `ev_e6e1baccd3a3654b918f775584319cc2e4d619fea9c5edf112b77dbf1fcbeec2`, establishes the bridge path more closely. It obtains stdin and stdout through `GetStdHandle`, derives a Windows session ID, and opens `\\.\pipe\IDMNetworkMonitor.<session-id>` for bidirectional I/O. It uses events, semaphores and worker threads to forward traffic. Its receive loop first accumulates a four-byte size, then the declared payload. Browser-side messages are checked for surrounding quote bytes and sent through a decoding helper; the reverse path calls an encoding helper, adds surrounding quote bytes, and rewrites the four-byte length. These observations support an adapter between browser native-message JSON strings and IDM's internal pipe messages. The complete escaping-helper semantics, maximum frame bounds and every error branch were not independently verified.

`FUN_00403530` pseudocode, Evidence `ev_066365456f88307f907d5192ceed0a2a240e80913eeaf85e8b6eb0f93de81a36`, corroborates the handshake constructor's four-byte length and message prefix. Both complete Evidence records and a session snapshot are retained under `~/.cache/idm-analysis/msg-host-focused/`. The first host overview reports 162 procedure records and 152 defined strings; as with the main executable, this is the provider inventory, not original-source coverage.

The host retains a build-path string `X:\Projects\IDMRelease\6.38.20\IDMMsgHost\Release_Win32\IDMMsgHost.pdb`. That is embedded metadata, not a recovered PDB or proof that the installer version is wrong. No PDB is present in the supplied folder.

## Reproduce

```sh
python tools/analyze_idm.py --self-test
python tools/analyze_idm.py /tmp/idm-extracted-64318 /tmp/native-inventory.json
```

Raw GNU objdump imports and disassembly are retained locally under `~/.cache/idm-analysis/`. `tools/ghidra_analysis.py` runs the free REA/Ghidra backend in an isolated process with a 2 GB Java heap and retains exact Evidence records, pseudocode and snapshots outside the repository. Focused pseudocode requests avoid the 10 MB MCP transport limit encountered by a full dossier; complete retained Evidence is exported before closing even if a later query fails. It does not alter the installed REA client configuration or execute the Windows target.

## Open questions

Original source names/types, the entire connection scheduler, retry/backoff policy, full on-disk IDM queue format, site-specific media parsing, legacy browser behavior and runtime Windows network integration are not established by this inventory. Full original-source reconstruction and behavioral equivalence remain unproven. Publisher signatures were not authenticated.

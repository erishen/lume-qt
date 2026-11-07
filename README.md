# lume-qt

> **English** | [简体中文](README.zh.md)

Lume's Qt desktop workbench for a **clinical monitoring demo** · Qt WebEngine / C++17 / React (Vite + TypeScript).

> **Non-clinical use only.** This is a research and engineering demo, not a medical device. It must not be used for diagnosis, treatment, or clinical decision-making.
> For the privacy/compliance audit, data flow, and pre-deployment checklist, see [`docs/privacy-compliance.md`](docs/privacy-compliance.md).

## What it is

`lume-qt` is a native desktop shell around a React monitoring UI, with a second native OpenGL page for 3D DICOM volume rendering. It is built to exercise the full path from a local device feed and a PACS/DICOM source to an auditable local event ledger and an optional Lume-based review service.

It is a **workbench and demonstrator**, not a certified clinical product:

- The monitoring signals are simulated; ECG, SpO₂, EEG, EMG and neural activity are generated locally.
- DICOM series can be loaded from disk or retrieved from a PACS using C-GET or C-MOVE.
- Every meaningful action and alarm is written to a local append-only JSONL ledger.
- An optional Lume service mirrors a minimized subset of that ledger and exposes read-only agent tools for engineering review.

## Features

| Area | What is implemented |
|---|---|
| Monitoring cockpit | Heart rate, SpO₂, respiration, NIBP, temperature; ECG and ultrasound panels; EEG/EMG/neural activity; 3D neural pathway view; link diagnostics and watchdog |
| Native imaging | Offscreen OpenGL volume rendering; clinical CT presets and manual window/level; color map; CT-only bone removal; clip plane; 1:1 device-pixel rendering |
| DICOM | Series parsing via DCMTK; 8-bit volume plus int16 HU volume; CPU copy release after upload; chunked texture upload |
| PACS | C-ECHO heartbeat; C-FIND study query; C-GET retrieval with C-MOVE fallback; local C-MOVE receiver; retry and online heartbeat; DICOM TLS for SCU paths |
| Reliability | Single-instance lock with PID liveness; crash recovery and previous-study restore; alarm mute/expiry; watchdog and latency board; 8 MB JSONL rotation |
| Audit | Local JSONL is the source of truth; event types are filterable and searchable; Lume mirror is optional and non-blocking |
| Lume review | Read-only `medmon_summary`, `medmon_events`, `medmon_review` tools; review panel in the UI; strict non-clinical labeling |
| Privacy | Source-side hashing/removal of identifiers before any Lume uplink; second whitelist at the Lume service; local full-fidelity ledger stays local |

## Architecture

```text
┌──────────────────────────────── lume-qt ────────────────────────────────┐
│  Qt / C++ host                                      React / Vite        │
│                                                                         │
│  MainWindow ── QWebEngineView ───────────────────→  app://app/index.html│
│       │                  QWebChannel  ←──────────→  qwebchannel.ts      │
│       │                                                                 │
│       ├── Bridge (QObject: methods + signals)                           │
│       ├── AuditSink → local JSONL ledger                                │
│       ├── LumeSink  → minimized batch POST → Lume service               │
│       ├── PACS worker (DCMTK: C-ECHO/C-FIND/C-GET/C-MOVE)               │
│       └── Native page: QOpenGLWidget volume renderer + vital strip      │
└─────────────────────────────────────────────────────────────────────────┘
```

Key design points:

- The Web page is served through a custom `app://` scheme instead of `file://`, so SPA routing and bundled assets work normally.
- The `Bridge` object is exposed over `QWebChannel`; C++ methods are called from JS and C++ signals are pushed back.
- Web content and native content are **strictly separated into two top-level tabs**. The monitoring tab contains only the `QWebEngineView`; the native imaging tab contains only the OpenGL renderer and the native vital strip. No native widget is overlaid on the Web page.
- The persistent `QWebEngineProfile` is required for the custom scheme handler on current Qt versions. Its state lives under the app data directory, so only one instance should run at a time.
- The native volume renderer uses its own offscreen OpenGL path, avoiding a second on-window GL context competing with WebEngine.

## Repository layout

```text
lume-qt/
├── CMakeLists.txt
├── Makefile
├── src/
│   ├── main.cpp                 # app entry, app:// scheme, profile migration
│   ├── mainwindow.{h,cpp}       # tabs, WebEngine view, native page, dark QSS
│   ├── bridge.{h,cpp}           # QWebChannel methods/signals
│   ├── auditsink.{h,cpp}        # append-only JSONL ledger
│   ├── lumesink.{h,cpp}         # batched Lume uplink + source-side sanitization
│   ├── alarmconfig.{h,cpp}      # env/Lume-configurable alarm thresholds
│   ├── deviceprovider.{h,cpp}   # simulated device abstraction
│   ├── pacs*.{h,cpp}            # DCMTK SCU/SCP paths
│   ├── volumeview.{h,cpp}       # native offscreen volume renderer
│   └── volumestats.{h,cpp}      # pure window/level helpers and tests
├── frontend/                    # React 19 + Vite 6 + TypeScript
│   ├── src/App.tsx              # monitoring cockpit
│   ├── src/AuditStream.tsx      # ledger + Lume review UI
│   └── src/qwebchannel.ts       # typed bridge wrapper
├── tools/lume-medmon/           # reference Lume service (.lume)
│   ├── app.lume
│   ├── .env.example
│   └── .data/                   # local mirror DB, gitignored
├── resources/web/               # frontend build output, gitignored
├── tests/                       # Google Test suites
├── docs/                        # privacy, integration, traceability, risk, soak
└── scripts/build.sh
```

## Requirements

- macOS or Linux. Windows is not part of the current test matrix; path handling would need adjustment.
- Qt 6.5+ with `WebEngineWidgets` and `WebChannel`.
  - macOS Homebrew: `brew install qt`
- DCMTK 3.6+ (the CMake build requires DCMTK; the current implementation uses it for DICOM parsing and networking).
  - macOS Homebrew: `brew install dcmtk`
- Google Test for C++ unit tests: `brew install googletest`
- CMake 3.21+, Ninja (or Make)
- Node.js 18+ and npm 9+

The examples below assume Homebrew's Qt at `/opt/homebrew/opt/qt`. Adjust `CMAKE_PREFIX_PATH` for other installations.

## Build and run

Recommended:

```bash
make            # build frontend + C++
make run        # build and launch the app
make dev        # Vite hot reload + Qt app; stale port 5173 is cleaned first
make dev-hot    # only start Vite; run the app separately with QTRS_DEV_URL
make dev-stop   # stop a stale Vite dev server
make frontend   # build frontend only
make clean      # remove build/ and frontend output
make rebuild    # clean + full rebuild
```

Script route:

```bash
./scripts/build.sh
./build/lume-qt
```

Manual route:

```bash
# 1) frontend
cd frontend && npm install && npm run build && cd ..

# 2) C++
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt -G Ninja
cmake --build build

# 3) run
./build/lume-qt
```

The CMake `frontend-build` target only rebuilds the frontend when its sources are newer than `resources/web/`; it also copies `qwebchannel.js` from the Qt installation.

### Frontend hot reload

```bash
make dev
# or, in two terminals:
make dev-hot
QTRS_DEV_URL=http://localhost:5173 ./build/lume-qt
```

`make dev` uses `QTRS_DEV_URL` to load the Vite dev server instead of `app://app/index.html`. It cleans a stale listener on port 5173 before starting and stops it on exit. Use `DEV_KILL_STALE=0 make dev` to fail instead of killing a stale process.

## Environment variables

All switches are read once at startup. `0` or unset means off unless noted.

### Runtime and diagnostics

| Variable | Default | Purpose |
|---|---|---|
| `QTRS_DEV_URL` | unset (`app://app/index.html`) | Load from a Vite dev server; only http(s) + loopback (`localhost` / `127.0.0.1` / `::1`) is accepted, remote values are ignored |
| `QTRS_TOPMOST` | unset | Keep the window on top (debug/screenshot helper) |
| `QTRS_RELOAD_MIN` | `30` | Rebuild profile/page every N minutes; `0` disables it |
| `QTRS_MUTE_SIGNALS` | unset | `1` stops forwarding waveform samples to the WebView; native strip still paints |
| `QTRS_SAMPLE_DIV` | `1` | Forward 1 of every N waveform samples (diagnostic downsampling) |
| `QTRS_FAULT` | unset | Comma-separated channels (`ecg,spo2,eeg,emg`) stopped 3 s after launch |
| `QTRS_AUDIT_MAX_MB` | `8` | JSONL rotation threshold per file, in MB |
| `QTRS_AUDIT_RETAIN_DAYS` | unset (no pruning) | Prune old audit files by filename date; opt-in |

### Simulated device and alarms

| Variable | Default | Purpose |
|---|---|---|
| `QTRS_SIM_HR` | unset (varies) | Fixed simulated heart rate |
| `QTRS_SIM_SPO2` | unset (varies) | Fixed simulated SpO₂, e.g. `86` to trigger a high alarm |
| `QTRS_ALARM_SPO2_HIGH` / `QTRS_ALARM_SPO2_MID` | `90` / `92` | High/medium SpO₂ thresholds, clamped to `[50,100]` |
| `QTRS_ALARM_HR_HIGH` / `QTRS_ALARM_HR_MID` | `50` / `55` | High/medium HR thresholds, clamped to `[20,200]` |
| `QTRS_ALARM_MUTE_SEC` | `120` | Default mute duration, clamped to `[5,300]` s |
| `QTRS_ALARM_SOUND` | on | `0` disables alarm audio; visual alarm state remains |
| `QTRS_LUME_URL` | unset | Lume base URL, e.g. `http://127.0.0.1:8090`; unset means no Lume network stack |
| `QTRS_LUME_TOKEN` | unset | Adds `Authorization: Bearer …` to audit/config/chat requests; intended for a reverse proxy, never logged |

### Volume and windowing

| Variable | Default | Purpose |
|---|---|---|
| `QTRS_NATIVE_VOLUME` | on | `0` disables the native OpenGL volume page and falls back to the Web `Volume3D` view |
| `QTRS_VOLUME_DIR` | unset | Load a DICOM directory 6 s after startup, overriding the demo sample |
| `QTRS_NO_RESTORE` | unset | Do not restore the previous volume source |
| `QTRS_KEEP_VOLUME_BYTES` | unset | Keep both 8-bit and HU CPU copies after upload |
| `QTRS_VOLUME_UPLOAD_MB` | `16` | Texture upload chunk size; `0` restores one-shot upload |
| `QTRS_VOLUME_PRESET` | user preference | `0`–`3` preset; `4` custom (with `QTRS_VOLUME_WL`) |
| `QTRS_VOLUME_WL` | user preference | Manual `width,level`, e.g. `800,150` |
| `QTRS_VOLUME_COLOR` | user preference | `1` color map / `0` grayscale |
| `QTRS_VOLUME_BONE` | user preference | `1` bone removal / `0` keep; CT only |
| `QTRS_VOLUME_CLIP` | user preference | Clip plane `axis,position`, axis `0`–`2`, position `0`–`1`, e.g. `1,0.6` |

### PACS

| Variable | Default | Purpose |
|---|---|---|
| `QTRS_PACS` | unset | `host:port:calledAE`; no PACS networking when unset |
| `QTRS_PACS_MODE` | `auto` | `auto` (C-GET, fall back to C-MOVE) / `get` / `move` |
| `QTRS_PACS_STORE_AE` | `QTRSSTORE` | AE title of the local C-MOVE receiver |
| `QTRS_PACS_STORE_PORT` | `11113` | Local C-MOVE receiver port |
| `QTRS_PACS_HEARTBEAT_MS` | `30000` | C-ECHO heartbeat interval; `0` disables it |
| `QTRS_PACS_AUTO_GET` | unset | `1` retrieves the first C-FIND match; a number is a delay in ms |
| `QTRS_PACS_TLS` | off | `1` enables DICOM TLS; `0`/`false` disables it |
| `QTRS_PACS_TLS_CA` | unset | CA certificate path; required when TLS verification is not `ignore` |
| `QTRS_PACS_TLS_CERT` / `QTRS_PACS_TLS_KEY` | unset | Client certificate/key PEM paths; required for TLS |
| `QTRS_PACS_TLS_VERIFY` | `require` | `require` / `check` / `ignore` |
| `QTRS_PACS_RETAIN_DAYS` | unset (no pruning) | Prune old studies by directory mtime at startup; opt-in |

The Chinese README contains the exhaustive table and the long engineering notes for every switch.

## UI: two tabs, no mixed content

The central area is a custom `QTabBar` + `QStackedWidget`, not `QTabWidget` (macOS `QTabWidget` exposes a light native strip on a dark shell).

| Tab | Contents |
|---|---|
| **Monitoring** | React cockpit: vital cards, ECG/ultrasound, neuro channels, 3D neural path, latency/watchdog, audit ledger, Lume review, developer tools |
| **Native imaging** | Native offscreen OpenGL volume, window/level and clip controls, color/bone controls, native ECG/SpO₂ strip |

- `Ctrl+1` / `Ctrl+2` switch tabs; Qt maps `Ctrl` to Command on macOS (`⌘1` / `⌘2`).
- The selected tab is stored in `QSettings` as `ui/mainTab` and restored on restart.
- The bottom native strip is **only** on the native imaging tab. The monitoring page has no native overlay.
- The monitoring page is capped at `max-width: 1040px` so ECG/ultrasound keep a usable clinical-monitoring aspect ratio on wide windows.
- The native side panel is 360 px; the volume view remains 1:1 device-pixel rendering.
- The top bar carries a persistent **“Demo · Non-clinical”** badge.

## DICOM volume pipeline

1. DCMTK parses the selected series on a background worker.
2. Two volumes are produced on the same grid: an 8-bit display volume and an int16 HU volume with rescale slope/intercept applied.
3. The native renderer receives the volume and uploads it in chunks (default 16 MB) to avoid one long UI stall.
4. After upload, CPU copies are released by default; `QTRS_KEEP_VOLUME_BYTES` keeps them for diagnostics.
5. Window/level is computed from CT presets (`80/40`, `1500/300`, `400/100`, adaptive) or from the non-CT tissue span; manual W/L is clamped to the value range.
6. The native shader supports CT bone removal (`HU ≥ 300` transparent), an arbitrary-axis clip plane, grayscale/color mapping, and 1:1 rendering.

Limitations:

- Bone removal is a threshold, not connected-component segmentation. Contrast-enhanced vessels with HU ≥ 300 can be removed too.
- The clip plane hides samples; it does not create a closed cut surface, so it is not a measurement plane.
- Non-CT data has no HU bone semantics; the bone control is hidden instead of shown as a disabled checkbox.

## PACS and networking

`QTRS_PACS=host:port:calledAE` enables the DCMTK path:

- **C-ECHO** heartbeat to keep connectivity visible, including during retrieval.
- **C-FIND** study query; the UI lets the operator choose the study rather than always taking the first result.
- **C-GET** retrieval, with automatic fallback to **C-MOVE** when the server does not support C-GET.
- A local **C-MOVE storage SCP** receiver on `QTRS_PACS_STORE_PORT` (default `11113`) accepts the incoming series.
- Retrieval retries under transient failures; a failure is surfaced in the audit stream and UI.
- TLS is available for the SCU paths with CA/client cert/key and `require`/`check`/`ignore` verification.

Current limitation: the local C-MOVE receiver is still a plaintext SCP; TLS covers the outbound SCU paths, not that receiver. Treat the receiver port as local-only.

## Alarms and audit ledger

- Alarm levels are high and medium, with distinct tones and a re-ring throttle.
- Mute is a UI state, not a threshold bypass; the alarm state and red frame remain.
- Thresholds come from environment defaults first, then a Lume `/api/medmon/config` response when configured. Invalid or contradictory values are rejected and logged.
- `AuditSink` writes JSON Lines under the app data directory. The local file is the authoritative record; the Lume mirror never blocks the UI or the ledger write.
- Events are rotated at 8 MB by default; the event stream UI supports type filtering and numeric/text search.

Typical app data (macOS):

```text
~/Library/Application Support/research/lume-qt/
├── audit/audit-YYYYMMDD.jsonl
├── pacs/<StudyInstanceUID>/
└── state/instance.lock
```

## Lume integration and AI review

Set `QTRS_LUME_URL` to enable the mirror and review features:

```bash
QTRS_LUME_URL=http://127.0.0.1:8090 ./build/lume-qt
```

The desktop shell:

- queues up to 500 events, sends up to 50 per batch, times out at 3 s, and retries after 5 s;
- drops the oldest events on overflow instead of growing without bound;
- keeps the local JSONL ledger authoritative;
- sanitizes events at the source before they enter the network queue.

Reference Lume service:

```bash
cd tools/lume-medmon
test -f .env || cp .env.example .env   # fill in local model settings; never commit .env
./run.sh start             # start + write probe; unhealthy instance is restarted
./run.sh watch             # foreground supervisor: restart after 2 failed probes
```

Useful endpoints:

```text
GET  /health
GET  /api/medmon/health     # write-path probe: zero-row DELETE, real SQLite write txn
GET  /api/agent/ready
POST /api/medmon/audit
GET  /api/medmon/config
POST /api/medmon/config
GET  /api/medmon/summary
GET  /api/medmon/events
POST /react/api/chat        # agent chat used by the review panel
```

Agent tools are read-only: `medmon_summary`, `medmon_events`, and `medmon_review`. The review panel is for engineering review only; it is not a diagnostic opinion.

## Privacy and compliance

- The repository contains **no hard-coded API keys**; `.env`, sample DICOM, PACS output, local SQLite data and build artifacts are gitignored.
- `LumeSink::sanitizedForLume()` runs before the network queue: `studyUid` / `dir` / `path` become non-reversible 16-hex digests, patient keys are removed, and absolute paths plus DICOM UIDs inside message strings are scrubbed.
- The Lume service applies a second whitelist before writing to its mirror DB and before serving `/api/medmon/events` and summary payloads.
- The local JSONL ledger intentionally keeps full fidelity for local troubleshooting. It is PHI-bearing data and must not be uploaded, committed, or shared.
- The UI carries a **“Demo · Non-clinical”** badge and the README repeats the non-clinical disclaimer.

Residual deployment risks:

1. The Lume HTTP service has no authentication or TLS; keep it on `127.0.0.1` or put an authenticated reverse proxy in front.
2. A third-party model endpoint still receives minimized event summaries; review its data-processing terms, region and retention policy, or use an on-prem model.
3. Local PACS output and JSONL ledgers have no automatic retention, deletion workflow, or disk encryption yet.
4. Historical rows in an existing `tools/lume-medmon/.data/medmon.db` may predate the whitelist and contain raw identifiers; rebuild or migrate that DB if it ever held real data.
5. The project has no medical-device registration, clinical validation, QMS, electronic signature, or clinical audit trail. A disclaimer does not replace regulatory clearance.

See [`docs/privacy-compliance.md`](docs/privacy-compliance.md) for the data inventory, threat path, implemented controls and pre-deployment checklist.

## Testing and quality gates

```bash
make run-tests                  # C++ unit tests through ctest
make frontend-tests             # Vitest frontend tests
cd frontend && npm run typecheck
cd frontend && npm run build
make trace                      # validate requirement traceability IDs/status/references
make perf-bench                 # performance benchmark target
```

Current suites:

- `VolumeStatsUnitTests` — pure window/level and percentile helpers.
- `BridgeUnitTests` — bridge contracts, PACS state, Lume sink queue/redaction behavior.
- Frontend tests — eight Vitest cases for the latency/event measurement helpers.

The project deliberately has **no CI pipeline**. Qt WebEngine + DCMTK make CI images expensive; the local Makefile targets are the quality gate. If CI is added later, verify `qt6-webengine-dev` and `libdcmtk-dev` are available in the target image first.

## Known limitations

- Not a medical device and not clinically validated.
- React component tests are not implemented yet; the existing frontend tests cover pure helpers.
- Bone removal is threshold-only; clip planes are visual cuts, not closed surfaces.
- C-MOVE receiver is plaintext; TLS is limited to SCU paths.
- Lume HTTP is loopback-only by default and has no auth/TLS; `QTRS_LUME_TOKEN` only adds a header for a reverse proxy, the reference service does not verify it.
- Windows is not tested; macOS/Linux are the supported demo environments.
- Local PHI is owner-only (`700` directories / `600` files) with opt-in retention (`QTRS_AUDIT_RETAIN_DAYS` / `QTRS_PACS_RETAIN_DAYS`); nothing is deleted by default.

## Documentation

| Document | Contents |
|---|---|
| [`docs/demo-handoff.md`](docs/demo-handoff.md) | Demo handoff: scope, startup, final verification, known boundaries (Chinese) |
| [`docs/architecture.md`](docs/architecture.md) | System architecture, module boundaries, threading, native/Web split, DICOM/PACS and audit flows (Chinese) |
| [`docs/implementation-notes.md`](docs/implementation-notes.md) | Chronological implementation and verification notes split out of the Chinese README (Chinese) |
| [`docs/privacy-compliance.md`](docs/privacy-compliance.md) | Data inventory, outbound path, implemented controls, residual risks, pre-deployment checklist |
| [`docs/lume-integration.md`](docs/lume-integration.md) | Event contract, reference service, config downlink, agent review, next steps |
| [`docs/traceability.md`](docs/traceability.md) | Requirement → implementation → evidence matrix |
| [`docs/risk-analysis.md`](docs/risk-analysis.md) | Hazard/control analysis, including alarm and AI risks |
| [`docs/soak-2026-11-06.md`](docs/soak-2026-11-06.md) | Long-running soak notes and observations |
| [`README.zh.md`](README.zh.md) | Chinese README: overview, build/run, environment variables, tests, known issues |

## License

No license file is included in this repository snapshot. Add one before distributing the project outside the current workspace.

# AGENTS.md

DistroAV is a C++ OBS Studio plugin (native module, `distroav.dll`/`.so`/`.plugin`)
that sends/receives video+audio over NDI. It links against libobs, Qt6 Widgets,
and the NDI SDK (vendored headers in [`lib/ndi/`](lib/ndi)). No test suite exists;
correctness is verified by building and exercising the plugin inside OBS.

For anything not covered here, see [docs/agent_docs/architecture.md](docs/agent_docs/architecture.md)
and [docs/agent_docs/build-system.md](docs/agent_docs/build-system.md).

## Build (Windows — verified in this checkout)

A configured `build_x64/` already exists in this repo (first-configure fetches
OBS source + prebuilt deps into `.deps/`, which is large and slow; skip it when
`build_x64/` is already present).

```powershell
cmake --build build_x64 --preset windows-x64
```

This was run and confirmed to succeed end-to-end in this checkout, producing
`build_x64/RelWithDebInfo/distroav.dll` and copying it into `build_x64/rundir`.

From scratch (no `build_x64/` yet):

```powershell
cmake --preset windows-x64
cmake --build build_x64 --preset windows-x64
```

Equivalent wrapper (also handles `CI`/`GITHUB_EVENT_NAME` env defaults the
underlying script expects): `.\tools\build-helper-windows.ps1`.

macOS/Linux use the same pattern with `macos`/`ubuntu-x86_64` presets, or
`.github/scripts/build-macos` / `.github/scripts/build-ubuntu` — **unverified in
this session** (this checkout only has Windows tooling available).

## Run / manually test a change

```powershell
.\tools\build-helper-windows.ps1 ; .\tools\install-windows.ps1 ; .\tools\run-obs-debug-windows.cmd
```

`install-windows.ps1` copies `release/RelWithDebInfo/distroav/` into
`%ProgramData%\obs-studio\plugins\distroav` — it **requires admin elevation** and
will self-elevate via a UAC prompt. Run each step separately when iterating rather
than the chained one-liner, so a build failure doesn't fall through to installing
stale output.

## Lint / format (CI-enforced, Linux/macOS runners only)

`clang-format` (C/C++/ObjC, config in [`.clang-format`](.clang-format), v16+) and
`gersemi` (CMake files, config in [`.gersemirc`](.gersemirc)) run in CI on every
PR via `check-format.yaml`. Neither tool is installed in this Windows checkout —
there is no local way to run them here short of WSL or a Linux/macOS machine. Match
the style visible in surrounding code (tabs, brace placement, existing CMake
formatting) and expect CI to be the actual gate.

## Architecture (see [docs/agent_docs/architecture.md](docs/agent_docs/architecture.md) for detail)

- [`src/plugin-main.cpp`](src/plugin-main.cpp) — module entry point, registers
  the source/output/filter types below with libobs.
- [`src/ndi-source.cpp`](src/ndi-source.cpp) — NDI→OBS source (largest file).
- [`src/main-output.cpp`](src/main-output.cpp) / [`src/preview-output.cpp`](src/preview-output.cpp) — OBS→NDI program/preview outputs.
- [`src/ndi-filter.cpp`](src/ndi-filter.cpp) — per-source "dedicated NDI output" filter.
- [`src/ndi-finder.cpp`](src/ndi-finder.cpp) — locates/loads the NDI runtime at startup.
- [`src/config.cpp`](src/config.cpp) — plugin settings, persisted to OBS's `global.ini`.
- [`src/forms/`](src/forms) — Qt Widgets dialogs (`.ui` + code-behind).
- [`src/obs-support/`](src/obs-support) — OBS C-API/Qt glue helpers.
- NDI send/receive runs on dedicated worker threads, separate from the Qt UI
  thread — see the threading note in `docs/agent_docs/architecture.md` before
  touching source/output code; cross-thread state bugs here have a real history.

## Conventions (not enforced by clang-format/gersemi)

- Naming: `snake_case` for C-style names, `CamelCase` for C++ class/method names
  (mixed within the same file is normal — match whichever the surrounding code
  already uses, not a global rule).
- Indentation: tabs, 8 columns wide; ~80 col soft line limit (per [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md)).
- Commit messages: 50-char title / blank line / 72-col-wrapped body, present
  tense, prefixed with a scope when there's an obvious one (`CI:`, `UI:`,
  `Source:`, `PluginUpdate:` are all attested in history) — but a large fraction
  of real commits skip the scope prefix entirely, so don't invent one that
  doesn't fit.
- `buildspec.json`'s `version` field is the single source of truth for the
  plugin version; bump it in its own commit, separate from feature/fix work.

## Boundaries

**Always fine, no need to ask:**
- Editing files under `src/`, `data/locale/` (translation strings), `docs/`.
- Building locally (`cmake --build build_x64 --preset windows-x64`).
- Reading anything in `.deps/`, `build_x64/`, `release/` for reference (all gitignored, regenerated, never hand-edit).

**Ask first:**
- Changing `CMakeLists.txt`, `CMakePresets.json`, `buildspec.json`, or anything
  under `.github/` (workflows, actions, scripts) — these affect CI and release
  packaging for every platform, not just this checkout.
- Bumping the plugin version in `buildspec.json`.
- Installing the built plugin system-wide (`tools/install-windows.ps1` requires
  admin elevation and overwrites the user's live OBS plugin).
- Editing `.clang-format` / `.gersemirc` (repo-wide style contract).

**Never touch:**
- [`lib/ndi/`](lib/ndi) — vendored third-party NDI SDK headers under their own license.
- `.deps/`, `build_x64/`, `build_macos/`, `build_x86_64/`, `release*/` — generated, gitignored.
- Root-level `CLAUDE_HANDOFF.md`, `adapter-table-columns.md`, `drift-fix.patch`,
  `ReceiverStats.md`, `SenderStats.md` — gitignored personal scratch files left in
  this working tree from prior sessions/branches, not part of the tracked project
  (see "Uncertain" below).

## Uncertain / please confirm

- **`CLAUDE_HANDOFF.md`** at repo root describes an in-progress, non-compiling
  feature (`networkmonitor` branch: `ndi-network-report.*`, per-adapter firewall
  diagnostics) whose files don't exist anywhere in this `master` checkout's
  `src/`. It's gitignored, so it isn't part of the tracked repo — I didn't fold
  anything from it into this AGENTS.md. Is that branch still active, and should
  its own AGENTS.md guidance live separately (e.g. on that branch) rather than here?
- I could only verify the **Windows** build path end-to-end (ran the actual
  `cmake --build` command in this checkout). The macOS/Ubuntu commands in
  `docs/agent_docs/build-system.md` are transcribed from the CI scripts/presets,
  not executed — worth a sanity check on those platforms if agents will build there.
- Neither `clang-format` nor `gersemi` is installed in this environment, so I
  couldn't confirm the exact invocation (flags, file selection) actually used —
  only that CI runs them via the composite actions in `.github/actions/`.
- Commit-scope prefixes (`CI:`, `UI:`, `Source:`, etc.) are documented in
  `.github/CONTRIBUTING.md` but inconsistently used in real history — I described
  it as a loose convention rather than a hard rule; let me know if it's actually
  meant to be mandatory going forward.
- I added `!AGENTS.md` and `!/docs` to `.gitignore` so this file and the new
  `docs/agent_docs/` directory aren't silently excluded from commits (the repo's
  `.gitignore` allowlists everything from `/*`). Flagging since it's a repo-wide
  config file, even though the change is purely additive.

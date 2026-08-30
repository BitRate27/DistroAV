# Architecture notes

Background for the summary in [AGENTS.md](../../AGENTS.md).

## Module entry point

[`src/plugin-main.cpp`](../../src/plugin-main.cpp) is the OBS module entry point
(`OBS_DECLARE_MODULE`, `obs_module_load`/`obs_module_unload`). It registers the
source/output/filter types with libobs and owns the global `ndiLib` pointer — the
loaded `NDIlib_v6` function table from `libndi` (dynamically loaded at runtime, not
linked; see `ndi-finder.cpp` for how the library is located on each OS).

## The five OBS plugin objects

Each corresponds to one `obs_*_info` struct registered in `plugin-main.cpp`:

| File | OBS object type | Purpose |
|---|---|---|
| [`ndi-source.cpp`](../../src/ndi-source.cpp) | source | Receives an NDI stream into OBS (video/audio/PTZ/tally). Largest file in the plugin. |
| [`main-output.cpp`](../../src/main-output.cpp) | output | Sends the OBS program mix out as an NDI stream. |
| [`preview-output.cpp`](../../src/preview-output.cpp) | output | Sends the OBS Studio-mode preview out as a separate NDI stream. |
| [`ndi-filter.cpp`](../../src/ndi-filter.cpp) | filter | Per-source/scene "dedicated NDI output" filter. |
| [`test-output.cpp`](../../src/test-output.cpp) | output | Minimal test-pattern NDI output, used for diagnostics. |

## Supporting modules

- [`ndi-finder.cpp`/`.h`](../../src/ndi-finder.cpp) — locates and dynamically loads
  the installed NDI runtime library across OS.
- [`config.cpp`/`.h`](../../src/config.cpp) — reads/writes plugin settings to OBS's
  `global.ini` under the `[NDIPlugin]` section (see the doc comment at the top of
  `config.h` for exact file paths per OS).
- [`sync-debug.cpp`/`.h`](../../src/sync-debug.cpp) — compile-time-gated (off by
  default) A/V sync logging, added recently; see git log around "sync-debug" for
  why (drift diagnostics).
- [`premultiplied-alpha-filter.cpp`](../../src/premultiplied-alpha-filter.cpp) —
  small standalone OBS filter for alpha premultiplication, independent of the NDI
  filter above.
- [`src/forms/`](../../src/forms) — Qt Widgets dialogs (`.ui` + `.cpp`/`.h` pairs):
  output settings dialog, update-check dialog. `AUTOMOC`/`AUTOUIC` are on, so new
  `.ui` files just need adding to `target_sources` in `CMakeLists.txt`.
- [`src/obs-support/`](../../src/obs-support) — small helpers bridging OBS's C
  frontend API and Qt (`obs-app.hpp`, `qt_wrapper.hpp`, `curl-helper.h`,
  `remote-text.cpp` for the update-checker's HTTP fetch).
- [`lib/ndi/`](../../lib/ndi) — vendored NDI SDK headers (C++ wrapper +
  DynamicLoad). Third-party, do not edit.
- [`data/locale/`](../../data/locale) — translation `.ini` files, one per
  language, keyed by the same string IDs used via `Str("NDIPlugin....")` in C++.

## Threading model (read before touching source/output code)

NDI frame receive/send happens off the Qt/UI thread. `ndi-source.cpp` and
`main-output.cpp`/`preview-output.cpp` each run their own worker thread(s) for
frame pumping; UI-thread code (settings dialogs, `config.cpp` callbacks) must not
block on NDI I/O. Recent commit history (search log for "Lock keys", "mutex",
"queued on the UI thread") shows this has been an active source of real bugs —
treat any new cross-thread state as something to explicitly synchronize, not an
oversight to fix later.
